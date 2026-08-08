// SPDX-License-Identifier: MIT

#include "WingetComSource.h"

#include "ExecutableScanner.h"
#include "PackageSourceError.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>, which is where CLSCTX_LOCAL_SERVER and friends live; include
// <combaseapi.h> explicitly rather than relying on it coming in transitively.
#include <unknwn.h>
#include <Windows.h>
#include <combaseapi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.Management.Deployment.h>

// <winrt/base.h> is a header-only library with no #pragma comment(lib, ...) of its own
// (confirmed by inspecting the SDK headers), unlike <combaseapi.h>'s ole32.lib. Without
// this, RoGetActivationFactory/RoOriginateLanguageException/etc. are unresolved externals
// in any project linking this static library. A linker directive embedded in an .obj file
// propagates through the .lib archive to every consumer automatically, so this one line
// covers both syncwingetlink.vcxproj and syncwingetlink.tests.vcxproj without editing
// either project file.
#pragma comment(lib, "runtimeobject.lib")

#include <algorithm>
#include <format>
#include <utility>

namespace syncwingetlink
{
namespace
{
using namespace winrt::Microsoft::Management::Deployment;

// CLSIDs registered by the Microsoft Desktop App Installer package's AppxManifest.xml
// (<com:Class Id="..."> entries under the "Windows Package Manager Server" ExeServer).
// There is no documented, version-stable API to look these up programmatically; they are
// fixed constants confirmed against Microsoft.DesktopAppInstaller 1.29.280.0. See
// docs/adr-phase-2.md ADR-0009. Every activatable class in this namespace needs its own
// CLSID when called from an unpackaged desktop process - not just PackageManager - because
// none of them are registered for the ordinary WinRT activation path outside a package
// graph.
constexpr GUID kPackageManagerClsid = {
    0xC53A4F16, 0x787E, 0x42A4, {0xB3, 0x04, 0x29, 0xEF, 0xFB, 0x4B, 0xF5, 0x97}};
constexpr GUID kFindPackagesOptionsClsid = {
    0x572DED96, 0x9C60, 0x4526, {0x8F, 0x92, 0xEE, 0x7D, 0x91, 0xD3, 0x8C, 0x1A}};
constexpr GUID kPackageMatchFilterClsid = {
    0xD02C9DAF, 0x99DC, 0x429C, {0xB5, 0x03, 0x4E, 0x50, 0x4E, 0x4A, 0xB0, 0x00}};

[[nodiscard]] PackageSourceErrorKind classifyFindPackagesStatus(FindPackagesResultStatus status)
{
    switch (status)
    {
    case FindPackagesResultStatus::BlockedByPolicy:
        return PackageSourceErrorKind::PolicyBlocked;
    case FindPackagesResultStatus::AccessDenied:
        return PackageSourceErrorKind::AccessDenied;
    case FindPackagesResultStatus::CatalogError:
        return PackageSourceErrorKind::CatalogError;
    default:
        // InternalError, InvalidOptions, AuthenticationError: none of these map to a
        // more specific kind the CLI could act on differently today.
        return PackageSourceErrorKind::Unknown;
    }
}

[[nodiscard]] std::wstring getMetadataOrEmpty(const PackageVersionInfo& info,
                                              PackageVersionMetadataField field)
{
    try
    {
        return std::wstring(info.GetMetadata(field));
    }
    catch (const winrt::hresult_error&)
    {
        // Not every field is populated for every installer type; treat absence as empty
        // rather than failing the whole package.
        return {};
    }
}
} // namespace

struct WingetComSource::Impl
{
    // No longer owns its own ComApartment (removed in #56, per the forward note this
    // class's own header comment and docs/adr-phase-2.md ADR-0009 both carried since
    // M2): main.cpp now constructs the single process-wide ComApartment before any
    // core call, including the one that constructs this Impl, so a per-instance
    // apartment here would only be a redundant (if harmless) nested one. Any caller
    // that constructs a WingetComSource is responsible for the process already having
    // an initialized apartment - see docs/adr-phase-5.md ADR-0024.
    PackageManager manager{nullptr};
    PackageCatalog catalog{nullptr};
    // Activated during construction, not lazily in enumeratePackages(), so that every COM
    // activation this type performs fails at one predictable point - see the class comment
    // in WingetComSource.h. The object carries no per-call state (it is a filter list the
    // server reads), so a single instance is reused across repeated enumeratePackages()
    // calls.
    FindPackagesOptions findOptions{nullptr};

    Impl()
    {
        try
        {
            manager = winrt::create_instance<PackageManager>(kPackageManagerClsid,
                                                              CLSCTX_LOCAL_SERVER);
        }
        catch (const winrt::hresult_error& error)
        {
            const PackageSourceErrorKind kind = mapHresultToKind(error.code());
            const auto rawHresult = static_cast<uint32_t>(static_cast<int32_t>(error.code()));
            // PackageIdentityRequired gets a message naming the actual cause
            // (APPMODEL_ERROR_NO_PACKAGE, docs/adr-phase-9.md ADR-0039, issue #143)
            // rather than the generic wording, which previously gave no hint that the
            // server was found and registered but refused typed activation.
            const std::string message =
                kind == PackageSourceErrorKind::PackageIdentityRequired
                    ? std::format("The winget PackageManager COM server rejected typed "
                                  "activation from this unpackaged process "
                                  "(APPMODEL_ERROR_NO_PACKAGE, HRESULT {:#010x})",
                                  rawHresult)
                    : std::format(
                          "Failed to activate the winget PackageManager COM server "
                          "(HRESULT {:#010x})",
                          rawHresult);
            throw PackageSourceError(kind, message, error.code());
        }

        PackageCatalogReference catalogRef{nullptr};
        try
        {
            catalogRef = manager.GetLocalPackageCatalog(LocalPackageCatalog::InstalledPackages);
        }
        catch (const winrt::hresult_error& error)
        {
            throw PackageSourceError(mapHresultToKind(error.code()),
                                     "PackageManager::GetLocalPackageCatalog failed",
                                     error.code());
        }

        ConnectResult connectResult{nullptr};
        try
        {
            connectResult = catalogRef.Connect();
        }
        catch (const winrt::hresult_error& error)
        {
            throw PackageSourceError(mapHresultToKind(error.code()),
                                     "PackageCatalogReference::Connect threw", error.code());
        }

        if (connectResult.Status() != ConnectResultStatus::Ok)
        {
            // Covers both CatalogError and SourceAgreementsNotAccepted: neither has a
            // dedicated PackageSourceErrorKind today, and both are genuinely "the catalog
            // is not usable" rather than an activation-level failure.
            throw PackageSourceError(
                PackageSourceErrorKind::CatalogError,
                "PackageCatalogReference::Connect did not return Ok",
                static_cast<int32_t>(connectResult.ExtendedErrorCode()));
        }

        catalog = connectResult.PackageCatalog();

        findOptions = buildFindPackagesOptions();
    }

    [[nodiscard]] static FindPackagesOptions buildFindPackagesOptions()
    {
        try
        {
            FindPackagesOptions options =
                winrt::create_instance<FindPackagesOptions>(kFindPackagesOptionsClsid,
                                                            CLSCTX_LOCAL_SERVER);

            // An empty options object (no filters, no selectors) has been observed to come
            // back FindPackagesResultStatus::InvalidOptions against some winget versions
            // (docs/com-api.md "known caveats"). A single filter that matches every
            // package - "Id contains the empty string" - works around that without
            // narrowing the result set.
            PackageMatchFilter filter =
                winrt::create_instance<PackageMatchFilter>(kPackageMatchFilterClsid,
                                                           CLSCTX_LOCAL_SERVER);
            filter.Field(PackageMatchField::Id);
            filter.Option(PackageFieldMatchOption::ContainsCaseInsensitive);
            filter.Value(L"");
            options.Filters().Append(filter);

            return options;
        }
        catch (const winrt::hresult_error& error)
        {
            // FindPackagesOptions and PackageMatchFilter are separately registered
            // out-of-process classes, so activating them can fail independently of
            // PackageManager. Translate here rather than letting an hresult_error escape:
            // callers of IPackageSource are documented to see PackageSourceError only.
            throw PackageSourceError(mapHresultToKind(error.code()),
                                     "Failed to activate the winget FindPackagesOptions or "
                                     "PackageMatchFilter COM server",
                                     error.code());
        }
    }
};

WingetComSource::WingetComSource() : m_impl(std::make_unique<Impl>())
{
}

WingetComSource::~WingetComSource() = default;
WingetComSource::WingetComSource(WingetComSource&&) noexcept = default;
WingetComSource& WingetComSource::operator=(WingetComSource&&) noexcept = default;

std::vector<InstalledPackage> WingetComSource::enumeratePackages()
{
    FindPackagesResult findResult{nullptr};
    try
    {
        findResult = m_impl->catalog.FindPackages(m_impl->findOptions);
    }
    catch (const winrt::hresult_error& error)
    {
        throw PackageSourceError(mapHresultToKind(error.code()),
                                 "PackageCatalog::FindPackages threw", error.code());
    }

    if (findResult.Status() != FindPackagesResultStatus::Ok)
    {
        throw PackageSourceError(classifyFindPackagesStatus(findResult.Status()),
                                 "PackageCatalog::FindPackages did not return Ok",
                                 static_cast<int32_t>(findResult.ExtendedErrorCode()));
    }

    std::vector<InstalledPackage> packages;
    for (auto const& match : findResult.Matches())
    {
        // A single package with unexpected metadata should not abort the whole
        // enumeration; skip it and continue with the rest.
        try
        {
            const CatalogPackage package = match.CatalogPackage();
            const PackageVersionInfo installed = package.InstalledVersion();
            if (!installed)
            {
                continue;
            }

            const std::wstring installerType =
                getMetadataOrEmpty(installed, PackageVersionMetadataField::InstallerType);
            if (!isPortableInstallerType(installerType))
            {
                continue;
            }

            const std::wstring installedLocation =
                getMetadataOrEmpty(installed, PackageVersionMetadataField::InstalledLocation);
            if (installedLocation.empty())
            {
                // COM identified this as an installed portable package but did not report
                // where it lives; there is nothing to scan for executables. See
                // docs/adr-phase-2.md ADR-0009 for why this is dropped rather than
                // guessed at (e.g. from a Packages/<id>_<source> naming convention).
                continue;
            }

            InstalledPackage entry;
            entry.id = std::wstring(package.Id());
            entry.name = std::wstring(package.Name());
            entry.version = std::wstring(installed.Version());
            entry.installLocation = std::filesystem::path(installedLocation);
            entry.executables = collectExecutables(entry.installLocation);

            packages.push_back(std::move(entry));
        }
        catch (const winrt::hresult_error&)
        {
            continue;
        }
    }

    std::sort(packages.begin(), packages.end(),
             [](const InstalledPackage& a, const InstalledPackage& b) { return a.id < b.id; });
    return packages;
}
} // namespace syncwingetlink
