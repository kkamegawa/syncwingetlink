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
// The result of attempting to create one concrete source: `source` on success, `error` on
// a reported (not thrown) failure. A factory that fails to construct its source (e.g.
// WingetComSource::tryCreate() finding COM activation unavailable) reports that by
// setting `error` rather than throwing, so --source auto can degrade to the filesystem
// source without raising a first-chance C++ exception for a condition it handles by
// design (docs/adr-phase-9.md ADR-0040, issue #143). A factory may still throw a
// non-PackageSourceError exception (std::bad_alloc, a programming error) - that is not
// "the source is unavailable" and must propagate rather than being silently converted
// into a filesystem scan.
//
// A factory is not required to leave exactly one of `source`/`error` set - invokeFactory()
// (PackageSourceFactory.cpp) is what enforces that as a postcondition on every result it
// returns: a non-null `source` always wins and any `error` alongside it is discarded,
// and neither set is normalized into the caller-supplied fallback error. Every
// PackageSourceCreation this codebase's own code observes has gone through
// invokeFactory(), so callers may rely on "exactly one is set" without re-checking it -
// but a raw PackageSourceFactoryFn value itself carries no such guarantee.
struct PackageSourceCreation
{
    std::unique_ptr<IPackageSource> source;
    std::optional<PackageSourceError> error;
};

// Creates one of the concrete sources, reporting failure via the returned
// PackageSourceCreation::error rather than by throwing. Injectable so the selection logic
// can be tested without activating real COM - see docs/adr-phase-2.md ADR-0009 for why no
// automated test constructs a live WingetComSource.
using PackageSourceFactoryFn = std::function<PackageSourceCreation()>;

// Called once if --source auto degrades from COM to the filesystem, so the CLI can warn
// about it. Not an error handler: by the time it runs, the degradation has been accepted.
using PackageSourceDegradeHandler = std::function<void(const PackageSourceError&)>;

// The `--source auto` behaviour: try the COM source, fall back to the filesystem scan if
// COM is unavailable.
//
// COM's construction failure is reported through PackageSourceCreation::error (not
// thrown), so a host where COM activation is expected to fail (issue #143) degrades to
// the filesystem source without a first-chance exception. The FindPackages query, called
// only after a successful construction, can still fail at runtime - that is caught here
// via the ordinary try/catch, since it is a genuine runtime failure rather than the
// "COM is unavailable" case the non-throwing construction path exists for. Both mean the
// same thing to a user who asked for `auto`.
//
// Only a reported/caught PackageSourceError triggers degradation. Any other exception
// (std::bad_alloc, a programming error) propagates: those are not "COM is unavailable"
// and must not be silently converted into a filesystem scan.
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
