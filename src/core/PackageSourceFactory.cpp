// SPDX-License-Identifier: MIT

#include "PackageSourceFactory.h"

#include "FsScanSource.h"
#include "Paths.h"
#include "WingetComSource.h"

#include <utility>

namespace syncwingetlink
{
namespace
{
// Invokes factory and normalizes its result to PackageSourceCreation's "exactly one of
// source/error is set" postcondition (PackageSourceFactory.h), regardless of what the
// factory itself returned:
// - a missing factory, or a factory that hands back neither a source nor an error, is a
//   programming error, but it must not become a null dereference - reported as
//   PackageSourceCreation::error using the fallback kind/message instead;
// - a factory that hands back both is not trusted to have meant the error: callers below
//   treat a non-null source as success (requireSource(), and
//   AutoPackageSource::enumeratePackages()'s `if (comCreation.source)`), so a source and
//   an error would otherwise be inconsistent - the source wins and the error is dropped.
// This is the only place either gap is normalized; callers below never construct the
// fallback error themselves, nor re-check the postcondition.
[[nodiscard]] PackageSourceCreation invokeFactory(const PackageSourceFactoryFn& factory,
                                                  PackageSourceErrorKind fallbackKind,
                                                  const char* fallbackMessage)
{
    if (!factory)
    {
        return PackageSourceCreation{nullptr, PackageSourceError(fallbackKind, fallbackMessage)};
    }

    PackageSourceCreation result = factory();
    if (result.source)
    {
        result.error.reset();
        return result;
    }

    if (!result.error.has_value())
    {
        return PackageSourceCreation{nullptr, PackageSourceError(fallbackKind, fallbackMessage)};
    }

    return result;
}

// Calls factory and throws if it did not produce a source. Used by explicit --source
// com/fs, where a failure is the user's to see rather than something to degrade away from
// (docs/adr-phase-2.md ADR-0010) - unlike AutoPackageSource::enumeratePackages() below,
// which calls invokeFactory() directly so it can branch on the result instead of
// catching a throw.
[[nodiscard]] std::unique_ptr<IPackageSource> requireSource(const PackageSourceFactoryFn& factory,
                                                            PackageSourceErrorKind kind,
                                                            const char* message)
{
    PackageSourceCreation result = invokeFactory(factory, kind, message);
    if (!result.source)
    {
        throw result.error.value_or(PackageSourceError(kind, message));
    }

    return std::move(result.source);
}
} // namespace

AutoPackageSource::AutoPackageSource(PackageSourceFactoryFn makeComSource,
                                     PackageSourceFactoryFn makeFsSource,
                                     PackageSourceDegradeHandler onDegrade)
    : m_makeComSource(std::move(makeComSource)), m_makeFsSource(std::move(makeFsSource)),
      m_onDegrade(std::move(onDegrade))
{
}

std::vector<InstalledPackage> AutoPackageSource::enumeratePackages()
{
    // Reported (not thrown) construction failure, so a host where COM activation is
    // expected to fail every time (issue #143) degrades without a first-chance
    // exception. docs/adr-phase-9.md ADR-0040.
    PackageSourceCreation comCreation =
        invokeFactory(m_makeComSource, PackageSourceErrorKind::AppInstallerMissing,
                      "No winget COM package source was available to try");

    if (comCreation.source)
    {
        try
        {
            std::vector<InstalledPackage> packages = comCreation.source->enumeratePackages();

            // An empty result is not a failure. A machine with no portable packages
            // legitimately enumerates zero of them, and scanning the filesystem in that
            // case would be guessing that COM was wrong rather than degrading away from
            // a fault.
            m_resolvedSource = PackageSource::Com;
            m_degradationKind.reset();
            return packages;
        }
        catch (const PackageSourceError& error)
        {
            // Unlike construction, enumeratePackages() only runs after COM already
            // connected successfully, so a failure here is a genuine runtime exception
            // to catch - not the "COM is unavailable" case the non-throwing
            // construction path exists to avoid throwing for.
            comCreation.error = error;
        }
    }

    // comCreation.error is guaranteed to hold a value here: either invokeFactory()
    // reported it, or the catch above just set it.
    m_degradationKind = comCreation.error->kind();
    if (m_onDegrade)
    {
        m_onDegrade(*comCreation.error);
    }

    m_resolvedSource = PackageSource::FileSystem;
    return requireSource(m_makeFsSource, PackageSourceErrorKind::ScanFailed,
                         "No filesystem package source was available to fall back to")
        ->enumeratePackages();
}

std::unique_ptr<IPackageSource> createPackageSource(PackageSource requested,
                                                    PackageSourceFactoryFn makeComSource,
                                                    PackageSourceFactoryFn makeFsSource,
                                                    PackageSourceDegradeHandler onDegrade)
{
    switch (requested)
    {
    case PackageSource::Com:
        // No degradation: the user named COM, so a failure is theirs to see (the M6 CLI
        // maps the PackageSourceErrorKind onto an exit code).
        return requireSource(makeComSource, PackageSourceErrorKind::AppInstallerMissing,
                             "No winget COM package source was available");
    case PackageSource::FileSystem:
        return requireSource(makeFsSource, PackageSourceErrorKind::ScanFailed,
                             "No filesystem package source was available");
    case PackageSource::Auto:
    default:
        return std::make_unique<AutoPackageSource>(
            std::move(makeComSource), std::move(makeFsSource), std::move(onDegrade));
    }
}

std::unique_ptr<IPackageSource> createPackageSource(const AppOptions& options,
                                                    PackageSourceDegradeHandler onDegrade)
{
    // The Packages directory is resolved inside the lambda, not here, so that
    // `--source com` never pays for (or fails on) a path lookup it does not use.
    return createPackageSource(
        options.source,
        [] {
            std::optional<PackageSourceError> error;
            std::unique_ptr<IPackageSource> source = WingetComSource::tryCreate(error);
            return PackageSourceCreation{std::move(source), std::move(error)};
        },
        [packagesOverride = options.packagesDirectory] {
            return PackageSourceCreation{
                std::make_unique<FsScanSource>(paths::getPackagesDirectory(packagesOverride)),
                std::nullopt};
        },
        std::move(onDegrade));
}
} // namespace syncwingetlink
