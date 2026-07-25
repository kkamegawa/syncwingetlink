// SPDX-License-Identifier: MIT

#pragma once

#include "IPackageSource.h"

#include <memory>

namespace syncwingetlink
{
// COM-backed IPackageSource, talking to the winget COM API
// (Microsoft.Management.Deployment) through the C++/WinRT projection generated at build
// time from the installed Desktop App Installer package; see
// props/syncwingetlink.winget-projection.targets and docs/adr-phase-2.md ADR-0008.
//
// This header intentionally does not include any winrt header: only
// src/syncwingetlink.core.vcxproj imports the projection-generation target, so its
// generated include path is not available to every consumer of this header (in
// particular, tests/syncwingetlink.tests.vcxproj is not). All winrt types are confined to
// WingetComSource.cpp behind this pimpl.
//
// Construction performs the full COM activation and catalog-connect sequence, so a
// caller implementing --source auto degradation can catch construction failure alone,
// without calling enumeratePackages() first. Throws PackageSourceError on any
// unrecoverable failure; see docs/adr-phase-2.md ADR-0009 for the HRESULT-to-kind
// mapping this uses.
class WingetComSource final : public IPackageSource
{
public:
    WingetComSource();
    ~WingetComSource() override;

    WingetComSource(const WingetComSource&) = delete;
    WingetComSource& operator=(const WingetComSource&) = delete;
    WingetComSource(WingetComSource&&) noexcept;
    WingetComSource& operator=(WingetComSource&&) noexcept;

    [[nodiscard]] std::vector<InstalledPackage> enumeratePackages() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace syncwingetlink
