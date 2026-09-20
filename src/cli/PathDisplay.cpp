// SPDX-License-Identifier: MIT

#include "PathDisplay.h"

#include "Console.h"

#include "core/Paths.h"

#include <Windows.h>

#include <KnownFolders.h>
#include <ShlObj.h>

#include <memory>
#include <utility>

namespace syncwingetlink::cli
{
namespace
{
[[nodiscard]] bool isSeparator(wchar_t ch) noexcept
{
    return ch == L'\\' || ch == L'/';
}

[[nodiscard]] bool startsWithOrdinalIgnoreCase(std::wstring_view text,
                                               std::wstring_view prefix) noexcept
{
    if (prefix.empty() || text.size() < prefix.size())
    {
        return false;
    }
    return ::CompareStringOrdinal(text.data(), static_cast<int>(prefix.size()), prefix.data(),
                                  static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

// Trailing separators would otherwise make the boundary check below reject every path:
// a root of "C:\Users\bob\" followed by "\Documents" is not what any caller means.
[[nodiscard]] std::wstring stripTrailingSeparators(std::wstring value)
{
    while (!value.empty() && isSeparator(value.back()))
    {
        value.pop_back();
    }
    return value;
}

// Non-throwing counterpart to paths::getLocalAppDataDirectory(): a folder the shell
// will not report is simply one this feature cannot abbreviate.
[[nodiscard]] std::wstring queryKnownFolder(REFKNOWNFOLDERID folderId) noexcept
{
    PWSTR rawPath = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath);
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> owned(rawPath, &::CoTaskMemFree);

    if (FAILED(result) || owned == nullptr)
    {
        return {};
    }
    return std::wstring(owned.get());
}

[[nodiscard]] std::vector<KnownFolderMapping> buildKnownFolderMappings()
{
    // Only the three folders whose real form carries the account name. %PROGRAMFILES%
    // and friends identify nobody, so abbreviating them would shorten output without
    // serving the purpose this option exists for (docs/adr-phase-10.md ADR-0048).
    const std::pair<const KNOWNFOLDERID*, const wchar_t*> kCandidates[] = {
        {&FOLDERID_LocalAppData, L"%LOCALAPPDATA%"},
        {&FOLDERID_RoamingAppData, L"%APPDATA%"},
        {&FOLDERID_Profile, L"%USERPROFILE%"},
    };

    std::vector<KnownFolderMapping> mappings;
    mappings.reserve(std::size(kCandidates));
    for (const auto& [folderId, variable] : kCandidates)
    {
        std::wstring root = stripTrailingSeparators(queryKnownFolder(*folderId));
        if (root.empty())
        {
            continue;
        }
        mappings.push_back(KnownFolderMapping{std::move(root), variable});
    }
    return mappings;
}
} // namespace

namespace
{
// The longest folder whose root is a component-boundary prefix of text[at..], or
// nullptr. `boundary` decides what may legally follow the match: the prefix form
// accepts only a separator or the end of the string, the in-text form also accepts a
// quote (see abbreviateKnownFoldersInText()).
enum class MatchBoundary
{
    SeparatorOnly,
    SeparatorOrQuote,
};

[[nodiscard]] bool endsOnABoundary(std::wstring_view text, std::size_t after,
                                   MatchBoundary boundary) noexcept
{
    if (after >= text.size())
    {
        return true; // The text *is* the folder.
    }
    if (isSeparator(text[after]))
    {
        return true;
    }
    return boundary == MatchBoundary::SeparatorOrQuote &&
           (text[after] == L'\'' || text[after] == L'"');
}

[[nodiscard]] const KnownFolderMapping* longestMatchAt(std::wstring_view text, std::size_t at,
                                                       std::span<const KnownFolderMapping> folders,
                                                       MatchBoundary boundary) noexcept
{
    const std::wstring_view rest = text.substr(at);

    const KnownFolderMapping* best = nullptr;
    for (const KnownFolderMapping& folder : folders)
    {
        if (folder.root.empty() || !startsWithOrdinalIgnoreCase(rest, folder.root))
        {
            continue;
        }

        // Component boundary: either the text ends with the folder, or what follows
        // starts a new component. Without this, "C:\Users\bob" would rewrite the
        // unrelated "C:\Users\bobby\...".
        if (!endsOnABoundary(rest, folder.root.size(), boundary))
        {
            continue;
        }

        // Longest match wins: %LOCALAPPDATA% is itself under %USERPROFILE%, and the
        // more specific of the two is the more informative.
        if (best == nullptr || folder.root.size() > best->root.size())
        {
            best = &folder;
        }
    }
    return best;
}
} // namespace

std::wstring abbreviateKnownFolders(std::wstring_view path,
                                    std::span<const KnownFolderMapping> folders)
{
    // An extended-length path never matches a known folder literally: ArgParser accepts
    // "\\?\C:\Users\..." for --links-dir/--packages-dir/--rules verbatim (it rejects
    // only "\\.\" device paths), while SHGetKnownFolderPath always reports the ordinary
    // form. Matching against the non-extended spelling is what keeps those overrides
    // from printing the account name under -s.
    const std::wstring plain = paths::fromExtendedLengthPath(std::filesystem::path(path)).native();

    const KnownFolderMapping* best =
        longestMatchAt(plain, 0, folders, MatchBoundary::SeparatorOnly);
    if (best == nullptr)
    {
        // Nothing matched, so nothing is rewritten - including the "\\?\" prefix, which
        // is the caller's own spelling and not this function's to normalize away.
        return std::wstring(path);
    }

    std::wstring result = best->variable;
    result.append(std::wstring_view(plain).substr(best->root.size()));
    return result;
}

std::wstring abbreviateKnownFoldersInText(std::wstring_view text,
                                          std::span<const KnownFolderMapping> folders)
{
    if (folders.empty() || text.empty())
    {
        return std::wstring(text);
    }

    // A single left-to-right pass. Unlike the prefix form there is no extended-length
    // normalization here: rewriting arbitrary text would mean editing bytes this
    // function cannot prove are a path, so a "\\?\"-spelled path inside a message is
    // left alone rather than guessed at.
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();)
    {
        const KnownFolderMapping* match =
            longestMatchAt(text, index, folders, MatchBoundary::SeparatorOrQuote);
        if (match == nullptr)
        {
            result += text[index];
            ++index;
            continue;
        }
        result += match->variable;
        index += match->root.size();
    }
    return result;
}

std::span<const KnownFolderMapping> knownFolderMappings()
{
    // Queried once per process: the answer cannot change while this process runs, and
    // every path in a report would otherwise repeat three shell calls.
    static const std::vector<KnownFolderMapping> mappings = buildKnownFolderMappings();
    return mappings;
}

std::wstring formatPathForDisplay(const std::filesystem::path& path,
                                  const PathDisplayOptions& options)
{
    if (!options.showSpecialFolders)
    {
        return sanitizeForDisplay(path.native());
    }
    return sanitizeForDisplay(abbreviateKnownFolders(path.native(), knownFolderMappings()));
}

std::wstring formatDiagnosticForDisplay(std::wstring_view text, const PathDisplayOptions& options)
{
    // No sanitizeForDisplay() here, unlike formatPathForDisplay(): every caller is an
    // existing Console::writeLine() site that already sanitizes at its own boundary, and
    // adding a second pass would change what non-path diagnostics look like today.
    if (!options.showSpecialFolders)
    {
        return std::wstring(text);
    }
    return abbreviateKnownFoldersInText(text, knownFolderMappings());
}
} // namespace syncwingetlink::cli
