// SPDX-License-Identifier: MIT

#pragma once

#include "IPackageSource.h"
#include "Model.h"
#include "PackageSourceError.h"

#include <functional>
#include <memory>
#include <optional>

namespace syncwingetlink
{
// Creates one of the concrete sources. Injectable so the selection logic can be tested
// without activating real COM - see docs/adr-phase-2.md ADR-0009 for why no automated
// test constructs a live WingetComSource.
using PackageSourceFactoryFn = std::function<std::unique_ptr<IPackageSource>()>;

// Called once if --source auto degrades from COM to the filesystem, so the CLI can warn
// about it. Not an error handler: by the time it runs, the degradation has been accepted.
using PackageSourceDegradeHandler = std::function<void(const PackageSourceError&)>;

// The `--source auto` behaviour: try the COM source, fall back to the filesystem scan if
// COM is unavailable.
//
// The attempt is made in enumeratePackages() rather than in the constructor so that a COM
// failure is caught wherever it surfaces - activation and catalog connect happen when
// WingetComSource is constructed, but the FindPackages query can still fail afterwards,
// and both mean the same thing to a user who asked for `auto`.
//
// Only PackageSourceError triggers degradation. Any other exception (std::bad_alloc, a
// programming error) propagates: those are not "COM is unavailable" and must not be
// silently converted into a filesystem scan.
class AutoPackageSource final : public IPackageSource
{
public:
    AutoPackageSource(PackageSourceFactoryFn makeComSource, PackageSourceFactoryFn makeFsSource,
                      PackageSourceDegradeHandler onDegrade = {});

    [[nodiscard]] std::vector<InstalledPackage> enumeratePackages() override;

    // Which source produced the last result: PackageSource::Auto until enumeratePackages()
    // has been called, then Com or FileSystem.
    [[nodiscard]] PackageSource resolvedSource() const noexcept
    {
        return m_resolvedSource;
    }

    // Why COM was abandoned, when it was. Empty if COM succeeded or has not been tried.
    [[nodiscard]] std::optional<PackageSourceErrorKind> degradationKind() const noexcept
    {
        return m_degradationKind;
    }

private:
    PackageSourceFactoryFn m_makeComSource;
    PackageSourceFactoryFn m_makeFsSource;
    PackageSourceDegradeHandler m_onDegrade;
    PackageSource m_resolvedSource{PackageSource::Auto};
    std::optional<PackageSourceErrorKind> m_degradationKind;
};

// Selects a source, with the concrete sources injected. Never returns null.
//
// - PackageSource::Com        - the COM source alone; a failure propagates rather than
//                               degrading, because the user asked for COM specifically.
// - PackageSource::FileSystem - the filesystem source alone; makeComSource is never called.
// - PackageSource::Auto       - an AutoPackageSource wrapping both.
[[nodiscard]] std::unique_ptr<IPackageSource>
createPackageSource(PackageSource requested, PackageSourceFactoryFn makeComSource,
                    PackageSourceFactoryFn makeFsSource,
                    PackageSourceDegradeHandler onDegrade = {});

// Production entry point: builds the real WingetComSource / FsScanSource for the requested
// --source, resolving the Packages directory (honouring --packages-dir) only if the
// filesystem source is actually needed.
[[nodiscard]] std::unique_ptr<IPackageSource>
createPackageSource(const AppOptions& options, PackageSourceDegradeHandler onDegrade = {});
} // namespace syncwingetlink
