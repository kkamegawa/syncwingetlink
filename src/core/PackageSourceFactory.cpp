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
// A factory that hands back nothing is a programming error, but it must not become a null
// dereference. Report it as the source failing so --source auto can still degrade.
[[nodiscard]] std::unique_ptr<IPackageSource> requireSource(const PackageSourceFactoryFn& factory,
                                                            PackageSourceErrorKind kind,
                                                            const char* message)
{
    if (!factory)
    {
        throw PackageSourceError(kind, message);
    }

    std::unique_ptr<IPackageSource> source = factory();
    if (!source)
    {
        throw PackageSourceError(kind, message);
    }

    return source;
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
    try
    {
        const std::unique_ptr<IPackageSource> comSource =
            requireSource(m_makeComSource, PackageSourceErrorKind::AppInstallerMissing,
                          "No winget COM package source was available to try");

        std::vector<InstalledPackage> packages = comSource->enumeratePackages();

        // An empty result is not a failure. A machine with no portable packages
        // legitimately enumerates zero of them, and scanning the filesystem in that case
        // would be guessing that COM was wrong rather than degrading away from a fault.
        m_resolvedSource = PackageSource::Com;
        m_degradationKind.reset();
        return packages;
    }
    catch (const PackageSourceError& error)
    {
        m_degradationKind = error.kind();
        if (m_onDegrade)
        {
            m_onDegrade(error);
        }
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
        options.source, [] { return std::make_unique<WingetComSource>(); },
        [packagesOverride = options.packagesDirectory] {
            return std::make_unique<FsScanSource>(paths::getPackagesDirectory(packagesOverride));
        },
        std::move(onDegrade));
}
} // namespace syncwingetlink
