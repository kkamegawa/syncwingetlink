// SPDX-License-Identifier: MIT

#include "FsScanSource.h"

#include "ExecutableScanner.h"
#include "PackageSourceError.h"
#include "Paths.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace syncwingetlink
{
namespace
{
// "The directory is not there" is the normal state of a machine that has never installed
// a portable package - it is an empty result, not a failure. Anything else (denied
// access, an I/O error, a disconnected network path) is a genuine enumeration failure
// that IPackageSource requires us to report rather than silently return empty for.
[[nodiscard]] bool isNotFound(const std::error_code& error)
{
    return error == std::errc::no_such_file_or_directory;
}

[[noreturn]] void throwScanFailure(const char* what, const std::error_code& error)
{
    // No HRESULT is attached: PackageSourceError documents that field as COM-only, and a
    // std::error_code from <filesystem> is a Win32/errno value, not an HRESULT.
    throw PackageSourceError(PackageSourceErrorKind::ScanFailed,
                             std::string(what) + ": " + error.message());
}
} // namespace

FsScanSource::FsScanSource(std::filesystem::path packagesDirectory)
    : m_packagesDirectory(std::move(packagesDirectory))
{
}

std::wstring packageIdFromDirectoryName(const std::wstring& directoryName)
{
    const std::size_t separator = directoryName.find(L'_');
    if (separator == std::wstring::npos)
    {
        return directoryName;
    }

    return directoryName.substr(0, separator);
}

std::vector<InstalledPackage> FsScanSource::enumeratePackages()
{
    std::vector<InstalledPackage> packages;
    if (m_packagesDirectory.empty())
    {
        return packages;
    }

    const std::filesystem::path root = paths::toExtendedLengthPath(m_packagesDirectory);

    std::error_code error;
    const bool isDirectory = std::filesystem::is_directory(root, error);
    if (error)
    {
        if (isNotFound(error))
        {
            return packages;
        }

        throwScanFailure("Cannot access the WinGet Packages directory", error);
    }

    if (!isDirectory)
    {
        // The path exists but is a file; there is nothing to enumerate and nothing broken.
        return packages;
    }

    std::filesystem::directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
    {
        throwScanFailure("Cannot enumerate the WinGet Packages directory", error);
    }

    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            throwScanFailure("Failed while enumerating the WinGet Packages directory", error);
        }

        const std::filesystem::directory_entry& entry = *iterator;
        if (isReparsePoint(entry.path()))
        {
            continue;
        }

        std::error_code entryError;
        if (!entry.is_directory(entryError) || entryError)
        {
            continue;
        }

        std::vector<PackageExe> executables = collectExecutables(entry.path());
        if (executables.empty())
        {
            continue;
        }

        const std::wstring directoryName = entry.path().filename().native();
        std::wstring id = packageIdFromDirectoryName(directoryName);
        if (id.empty())
        {
            // A directory name beginning with '_' (or otherwise deriving an empty
            // identifier) cannot be turned into a usable InstalledPackage - skip it
            // rather than emitting one with an empty id/name that downstream logic
            // (alias resolution, --include/--exclude matching) is not prepared for.
            continue;
        }

        InstalledPackage package;
        package.id = std::move(id);
        package.name = package.id;
        // entry.path() is rooted at the \\?\-prefixed root; un-prefix it before storing,
        // matching the paths collectExecutables() already returns for package.executables.
        package.installLocation = paths::fromExtendedLengthPath(entry.path());
        package.executables = std::move(executables);
        packages.push_back(std::move(package));
    }

    std::sort(packages.begin(), packages.end(),
             [](const InstalledPackage& a, const InstalledPackage& b) { return a.id < b.id; });
    return packages;
}
} // namespace syncwingetlink
