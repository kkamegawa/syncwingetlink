// SPDX-License-Identifier: MIT

#include "RuleSetSelector.h"

#include "core/Paths.h"
#include "DefaultRules.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>; only MultiByteToWideChar is needed here, which lives in
// <stringapiset.h> (pulled in transitively via <Windows.h>).
#include <Windows.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace syncwingetlink
{
namespace
{
// Rules files are JSON text, conventionally UTF-8 on disk regardless of platform (unlike
// this codebase's internal std::wstring convention) - this is the one place that boundary
// is crossed. A leading UTF-8 BOM is tolerated since some Windows editors add one.
// Throws RuleSetError(FileReadError) if utf8Text is not valid UTF-8, rather than letting
// MultiByteToWideChar's failure (it returns 0 and leaves the output untouched) silently
// turn into an empty string that RuleSet::parse() would then reject with a confusing
// "not well-formed JSON" instead of the real problem.
[[nodiscard]] std::wstring utf8ToWide(std::string_view utf8Text)
{
    if (utf8Text.empty())
    {
        return {};
    }

    // MB_ERR_INVALID_CHARS makes an invalid byte sequence a hard failure (0, with
    // GetLastError() == ERROR_NO_UNICODE_TRANSLATION) instead of the default lenient
    // behavior of silently substituting a replacement character.
    const int required =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(),
                              static_cast<int>(utf8Text.size()), nullptr, 0);
    if (required <= 0)
    {
        throw RuleSetError(RuleSetErrorKind::FileReadError, "rules file is not valid UTF-8");
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(),
                              static_cast<int>(utf8Text.size()), result.data(), required);
    if (written <= 0)
    {
        throw RuleSetError(RuleSetErrorKind::FileReadError, "rules file is not valid UTF-8");
    }

    return result;
}

[[nodiscard]] std::string toUtf8(const std::filesystem::path& path)
{
    const std::wstring& wide = path.native();
    if (wide.empty())
    {
        return {};
    }

    const int required = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                                static_cast<int>(wide.size()), nullptr, 0,
                                                nullptr, nullptr);
    std::string result(static_cast<std::size_t>(required), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(),
                         required, nullptr, nullptr);
    return result;
}
} // namespace

RuleSet loadRuleSetFromFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        throw RuleSetError(RuleSetErrorKind::FileReadError,
                           "could not open rules file for reading: " + toUtf8(path));
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad())
    {
        throw RuleSetError(RuleSetErrorKind::FileReadError,
                           "could not read rules file: " + toUtf8(path));
    }

    const std::string text = buffer.str();
    std::string_view content = text;
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    if (content.starts_with(kUtf8Bom))
    {
        content.remove_prefix(kUtf8Bom.size());
    }

    return RuleSet::parse(utf8ToWide(content));
}

std::filesystem::path defaultUserRulesPathProvider()
{
    return paths::getUserRulesFilePath();
}

RuleSet selectRuleSet(const std::optional<std::filesystem::path>& explicitPath,
                     const UserRulesPathProvider& userRulesPath)
{
    if (explicitPath.has_value())
    {
        return loadRuleSetFromFile(*explicitPath);
    }

    const std::filesystem::path candidate = userRulesPath();
    std::error_code existsError;
    const bool userFileExists = std::filesystem::exists(candidate, existsError);
    if (existsError)
    {
        // An error here means exists() could not determine the answer (e.g. a denied
        // parent directory) - genuinely different from "nothing is there," which clears
        // existsError and returns false. Falling through to embedded defaults in this
        // case would silently hide the failure, contradicting the documented
        // absent-falls-through/failure-does-not rule this tier is supposed to follow
        // (docs/adr-phase-2.md ADR-0013). Not exercised by an automated test: reliably
        // provoking this from a non-elevated test process is the same difficulty
        // FsScanSource's access-denied path already documents (ADR-0010).
        throw RuleSetError(RuleSetErrorKind::FileReadError,
                           "could not determine whether the user rules file exists: " +
                               toUtf8(candidate));
    }
    if (userFileExists)
    {
        return loadRuleSetFromFile(candidate);
    }

    return defaultRules();
}
} // namespace syncwingetlink
