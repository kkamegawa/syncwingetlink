// SPDX-License-Identifier: MIT

#pragma once

#include "IPackageSource.h"

#include <filesystem>

namespace syncwingetlink
{
// Fallback package source used when the winget COM API is unavailable. It walks the
// WinGet Packages directory directly, so it recovers the executables but not the
// identifier metadata that only winget itself knows (version in particular).
class FsScanSource final : public IPackageSource
{
public:
    explicit FsScanSource(std::filesystem::path packagesDirectory);

    [[nodiscard]] std::vector<InstalledPackage> enumeratePackages() override;

private:
    std::filesystem::path m_packagesDirectory;
};

// Derives the package identifier from a Packages subdirectory name. winget names them
// "<PackageIdentifier>_<SourceIdentifier>", so everything before the first underscore is
// the identifier. Returns the whole name when no underscore is present.
[[nodiscard]] std::wstring packageIdFromDirectoryName(const std::wstring& directoryName);
} // namespace syncwingetlink
