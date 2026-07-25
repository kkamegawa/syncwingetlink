// SPDX-License-Identifier: MIT

#include "ExecutableScanner.h"

#include "Paths.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <system_error>

namespace syncwingetlink
{
namespace
{
constexpr std::wstring_view kExecutableExtension = L".exe";

[[nodiscard]] bool hasExecutableExtension(const std::filesystem::path& path)
{
    std::wstring extension = path.extension().native();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return extension == kExecutableExtension;
}
} // namespace

bool isReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::vector<PackageExe> collectExecutables(const std::filesystem::path& root)
{
    std::vector<PackageExe> executables;
    if (root.empty())
    {
        return executables;
    }

    const std::filesystem::path scanRoot = paths::toExtendedLengthPath(root);

    std::error_code error;
    if (!std::filesystem::is_directory(scanRoot, error) || error)
    {
        return executables;
    }

    std::filesystem::recursive_directory_iterator iterator(
        scanRoot, std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
    {
        return executables;
    }

    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            break;
        }

        // recursive_directory_iterator never descends into a symlink or junction unless
        // directory_options::follow_directory_symlink is set (we do not set it), which is
        // what actually prevents a reparse-point traversal loop here. This depth cap is
        // only a second guard in case a future reparse tag is not covered by that check.
        if (iterator.depth() > kMaxScanDepth)
        {
            iterator.disable_recursion_pending();
            continue;
        }

        const std::filesystem::directory_entry& entry = *iterator;

        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError ||
            !hasExecutableExtension(entry.path()))
        {
            continue;
        }

        // entry.path() is rooted at scanRoot, which carries the \\?\ prefix needed to
        // walk long paths. Rebase onto the caller's original root, then strip any prefix
        // that root itself carried (a caller may pass an already-prefixed directory, as
        // FsScanSource does), so the model never stores a \\?\-prefixed path — it would
        // compare unequal to paths built elsewhere (rules, --json output, console).
        std::filesystem::path displayPath = entry.path();
        std::error_code relativeError;
        const std::filesystem::path relative =
            std::filesystem::relative(entry.path(), scanRoot, relativeError);
        if (!relativeError)
        {
            displayPath = root / relative;
        }
        displayPath = paths::fromExtendedLengthPath(displayPath);

        executables.push_back(PackageExe{displayPath, std::nullopt});
    }

    std::sort(executables.begin(), executables.end(),
             [](const PackageExe& a, const PackageExe& b) { return a.path < b.path; });
    return executables;
}
} // namespace syncwingetlink
