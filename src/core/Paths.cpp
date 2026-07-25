// SPDX-License-Identifier: MIT

#include "Paths.h"

#include <ShlObj.h>

#include <memory>
#include <stdexcept>
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
            std::error_code(result, std::system_category()),
            "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed");
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

std::filesystem::path toExtendedLengthPath(const std::filesystem::path& path)
{
    if (path.empty())
    {
        throw std::invalid_argument("Cannot normalize an empty path");
    }

    const std::wstring original = path.native();
    if (original.starts_with(LR"(\\?\)") || original.starts_with(LR"(\\.\)"))
    {
        return path;
    }

    std::filesystem::path absolutePath =
        path.is_absolute() ? path : std::filesystem::absolute(path);
    absolutePath = absolutePath.lexically_normal();
    absolutePath.make_preferred();

    const std::wstring normalized = absolutePath.native();
    if (normalized.starts_with(LR"(\\?\)") || normalized.starts_with(LR"(\\.\)"))
    {
        return absolutePath;
    }

    if (normalized.starts_with(LR"(\\)"))
    {
        return std::filesystem::path(LR"(\\?\UNC\)" + normalized.substr(2));
    }

    return std::filesystem::path(LR"(\\?\)" + normalized);
}

std::filesystem::path fromExtendedLengthPath(const std::filesystem::path& path)
{
    const std::wstring native = path.native();

    if (native.starts_with(LR"(\\?\UNC\)"))
    {
        return std::filesystem::path(LR"(\\)" + native.substr(8));
    }

    if (native.starts_with(LR"(\\?\)"))
    {
        return std::filesystem::path(native.substr(4));
    }

    return path;
}
} // namespace syncwingetlink::paths
