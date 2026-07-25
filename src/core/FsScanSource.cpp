// SPDX-License-Identifier: MIT

#include "FsScanSource.h"

#include "ExecutableScanner.h"
#include "Paths.h"

#include <system_error>
#include <utility>

namespace syncwingetlink
{
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
    if (!std::filesystem::is_directory(root, error) || error)
    {
        return packages;
    }

    std::filesystem::directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
    {
        return packages;
    }

    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
        {
            break;
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
        InstalledPackage package;
        package.id = packageIdFromDirectoryName(directoryName);
        package.name = package.id;
        package.installLocation = entry.path();
        package.executables = std::move(executables);
        packages.push_back(std::move(package));
    }

    return packages;
}
} // namespace syncwingetlink
