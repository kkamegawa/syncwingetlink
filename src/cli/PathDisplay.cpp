// SPDX-License-Identifier: MIT

#include "PathDisplay.h"

#include "Console.h"

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

std::wstring abbreviateKnownFolders(std::wstring_view path,
                                    std::span<const KnownFolderMapping> folders)
{
    const KnownFolderMapping* best = nullptr;
    for (const KnownFolderMapping& folder : folders)
    {
        if (folder.root.empty() || !startsWithOrdinalIgnoreCase(path, folder.root))
        {
            continue;
        }

        // Component boundary: either the path *is* the folder, or the next character
        // starts a new component. Without this, "C:\Users\bob" would rewrite the
        // unrelated "C:\Users\bobby\...".
        if (path.size() != folder.root.size() && !isSeparator(path[folder.root.size()]))
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

    if (best == nullptr)
    {
        return std::wstring(path);
    }

    std::wstring result = best->variable;
    result.append(path.substr(best->root.size()));
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
} // namespace syncwingetlink::cli
