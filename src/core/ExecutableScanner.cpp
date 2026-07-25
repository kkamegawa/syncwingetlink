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

void collectInto(const std::filesystem::path& directory, int depth,
                 std::vector<PackageExe>& executables)
{
    if (depth > kMaxScanDepth)
    {
        return;
    }

    std::error_code error;
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
    {
        return;
    }

    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            return;
        }

        const std::filesystem::directory_entry& entry = *iterator;

        // Reparse points are skipped before any other test so that neither a directory
        // junction nor a symlinked executable can be followed.
        if (isReparsePoint(entry.path()))
        {
            continue;
        }

        std::error_code entryError;
        if (entry.is_directory(entryError) && !entryError)
        {
            collectInto(entry.path(), depth + 1, executables);
            continue;
        }

        if (entry.is_regular_file(entryError) && !entryError && hasExecutableExtension(entry.path()))
        {
            executables.push_back(PackageExe{entry.path(), std::nullopt});
        }
    }
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

    collectInto(scanRoot, 0, executables);
    return executables;
}
} // namespace syncwingetlink
