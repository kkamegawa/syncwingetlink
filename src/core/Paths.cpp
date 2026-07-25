// SPDX-License-Identifier: MIT

#include "Paths.h"

#include <ShlObj.h>

#include <memory>
#include <system_error>

namespace syncwingetlink::paths
{
std::filesystem::path getLocalAppDataDirectory()
{
    PWSTR rawPath = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &rawPath);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, &CoTaskMemFree);

    if (FAILED(result))
    {
        throw std::system_error(
            std::error_code(result, std::system_category()), "SHGetKnownFolderPath failed");
    }

    return std::filesystem::path(path.get());
}

std::filesystem::path
getLinksDirectory(const std::optional<std::filesystem::path>& overridePath)
{
    if (overridePath.has_value())
    {
        return *overridePath;
    }

    return getLocalAppDataDirectory() / L"Microsoft" / L"WinGet" / L"Links";
}

std::filesystem::path
getPackagesDirectory(const std::optional<std::filesystem::path>& overridePath)
{
    if (overridePath.has_value())
    {
        return *overridePath;
    }

    return getLocalAppDataDirectory() / L"Microsoft" / L"WinGet" / L"Packages";
}
} // namespace syncwingetlink::paths
