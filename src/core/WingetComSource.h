// SPDX-License-Identifier: MIT

#pragma once

#include "IPackageSource.h"
#include "PackageSourceError.h"

#include <memory>
#include <optional>

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
// tryCreate() performs the class's activation-time work - PackageManager,
// FindPackagesOptions and PackageMatchFilter activation, plus the catalog connect - and
// reports expected activation and connection failures by returning null and setting
// `error`, never by throwing. This lets --source auto degrade to the filesystem source
// (docs/adr-phase-9.md ADR-0040, issue #143) without raising a first-chance C++
// exception on a host where COM activation is expected to fail every time. Unrelated
// exceptions (e.g. std::bad_alloc) still propagate unchanged. enumeratePackages()
// itself still throws PackageSourceError if the later FindPackages call or result
// inspection fails at runtime, after a successful connection, so an --source auto
// implementation must still cover both (see AutoPackageSource in PackageSourceFactory.h).
//
// This class structurally enforces its "no raw winrt::hresult_error escapes" contract:
// tryCreate() and enumeratePackages() both own a boundary that translates any escaping
// winrt::hresult_error into PackageSourceError, while still propagating unrelated
// std::exception/foreign exceptions unchanged. This matters because
// winrt::hresult_error has no std::exception base: AutoPackageSource
// (PackageSourceFactory.h) catches only PackageSourceError, so one escaping here would
// silently defeat --source auto's filesystem fallback, and cli::run()'s
// catch (const std::exception&) would not catch it either. See docs/adr-phase-2.md
// ADR-0009 for the HRESULT-to-kind mapping, and docs/adr-phase-9.md ADR-0040/ADR-0041
// for the activation and boundary rules this comment describes.
class WingetComSource final : public IPackageSource
{
public:
    // Attempts to construct a fully-initialized WingetComSource. On success, returns the
    // instance and clears `error` (so a caller reusing the same std::optional across
    // calls never observes a stale failure from an earlier attempt). On failure, returns
    // null and sets `error` - this is the one place in this class's public surface that
    // reports failure without throwing.
    [[nodiscard]] static std::unique_ptr<WingetComSource>
    tryCreate(std::optional<PackageSourceError>& error);

    ~WingetComSource() override;

    WingetComSource(const WingetComSource&) = delete;
    WingetComSource& operator=(const WingetComSource&) = delete;
    WingetComSource(WingetComSource&&) noexcept;
    WingetComSource& operator=(WingetComSource&&) noexcept;

    [[nodiscard]] std::vector<InstalledPackage> enumeratePackages() override;

private:
    // Private: the only supported way to obtain an instance is tryCreate(), which is the
    // sole place initialization failure is handled. A public constructor would let a
    // caller construct an object whose COM activation was never checked.
    WingetComSource();

    [[nodiscard]] std::vector<InstalledPackage> enumeratePackagesImpl();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace syncwingetlink
