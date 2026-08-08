// SPDX-License-Identifier: MIT

#include "WingetComSource.h"

#include "ExecutableScanner.h"
#include "PackageSourceError.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>, which is where CLSCTX_LOCAL_SERVER, CoCreateInstance and
// friends live; include <combaseapi.h> explicitly rather than relying on it coming in
// transitively.
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

// winrt::create_instance<T>(clsid, ctx) expands to
// CoCreateInstance(clsid, outer, ctx, guid_of<T>(), &result) followed by
// winrt::check_hresult(), which throws on failure. On a host where an activation
// reproducibly fails (issue #143, APPMODEL_ERROR_NO_PACKAGE - docs/adr-phase-9.md
// ADR-0040) that throw is a first-chance C++ exception on every single run, for a
// condition this file handles by design. Calling CoCreateInstance directly and branching
// on the HRESULT gets the identical activation attempt without the throw.
//
// winrt::try_create_instance<T>() is throw-free too, but it discards the HRESULT (it
// reports only null-or-not) - and the HRESULT is exactly what mapHresultToKind() and the
// diagnostic messages below need - so it is not a substitute here.
//
// On success this does exactly what winrt::capture() does internally: adopt the returned
// pointer without an extra AddRef via the T(void*, take_ownership_from_abi_t)
// constructor every projected runtime class declares. winrt::guid_of<T>() resolves to
// T's *default interface* (e.g. IPackageManager), not IUnknown - deliberately: ADR-0037
// recorded a host where activating the bare CLSID for IUnknown succeeds while the typed
// interface is refused, so requesting IUnknown here would turn a clean, classifiable
// failure into a much later and much worse one.
//
// CLSCTX_LOCAL_SERVER is fixed, not a parameter: winget's classes have no in-process
// server and this codebase deliberately has no CLSCTX_INPROC_SERVER fallback
// (docs/com-api.md "Out-of-proc vs in-proc"). This helper is winget-activation-specific,
// not a general-purpose utility - resist widening it with a context parameter.
//
// [[nodiscard]] is load-bearing: an ignored failure leaves `instance` null, and the very
// next marshalled call on it throws the exact hresult_error this helper exists to avoid.
template <typename T>
[[nodiscard]] HRESULT createInstanceNoThrow(const GUID& clsid, T& instance) noexcept
{
    void* raw = nullptr;
    const HRESULT hr =
        ::CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, winrt::guid_of<T>(), &raw);
    if (SUCCEEDED(hr))
    {
        instance = T{raw, winrt::take_ownership_from_abi};
    }

    return hr;
}

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

    Impl() = default;

    // Performs every COM activation this type needs plus the catalog connect. Returns
    // nullopt on success; on failure, returns the PackageSourceError rather than throwing
    // it, so tryCreate() can report initialization failure without a first-chance C++
    // exception (docs/adr-phase-9.md ADR-0040, issue #143).
    [[nodiscard]] std::optional<PackageSourceError> initialize()
    {
        const HRESULT managerResult = createInstanceNoThrow(kPackageManagerClsid, manager);
        if (FAILED(managerResult))
        {
            const auto hresult = static_cast<int32_t>(managerResult);
            const PackageSourceErrorKind kind = mapHresultToKind(hresult);
            const auto rawHresult = static_cast<uint32_t>(hresult);
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
            return PackageSourceError(kind, message, hresult);
        }

        PackageCatalogReference catalogRef{nullptr};
        try
        {
            catalogRef = manager.GetLocalPackageCatalog(LocalPackageCatalog::InstalledPackages);
        }
        catch (const winrt::hresult_error& error)
        {
            return PackageSourceError(mapHresultToKind(error.code()),
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
            return PackageSourceError(mapHresultToKind(error.code()),
                                      "PackageCatalogReference::Connect threw", error.code());
        }

        try
        {
            if (connectResult.Status() != ConnectResultStatus::Ok)
            {
                // Covers both CatalogError and SourceAgreementsNotAccepted: neither has a
                // dedicated PackageSourceErrorKind today, and both are genuinely "the
                // catalog is not usable" rather than an activation-level failure.
                return PackageSourceError(
                    PackageSourceErrorKind::CatalogError,
                    "PackageCatalogReference::Connect did not return Ok",
                    static_cast<int32_t>(connectResult.ExtendedErrorCode()));
            }

            catalog = connectResult.PackageCatalog();
        }
        catch (const winrt::hresult_error& error)
        {
            // Status(), ExtendedErrorCode() and PackageCatalog() are marshalled
            // cross-process calls like any other and can each fail independently, even
            // though the Connect() call that produced connectResult already succeeded.
            // The PackageSourceError returned just above is not caught here - it is not
            // a winrt::hresult_error (it has no relation to it at all, so this handler
            // cannot match it), and it is returned rather than thrown in any case.
            return PackageSourceError(mapHresultToKind(error.code()),
                                      "Reading the winget catalog connect result threw",
                                      error.code());
        }

        return buildFindPackagesOptions(findOptions);
    }

    // Activates FindPackagesOptions and PackageMatchFilter and configures the
    // match-everything filter (see the comment below). Returns nullopt on success and
    // writes the result into `outOptions`; returns the error on failure without throwing.
    [[nodiscard]] static std::optional<PackageSourceError>
    buildFindPackagesOptions(FindPackagesOptions& outOptions)
    {
        FindPackagesOptions options{nullptr};
        const HRESULT optionsResult = createInstanceNoThrow(kFindPackagesOptionsClsid, options);
        if (FAILED(optionsResult))
        {
            return PackageSourceError(mapHresultToKind(static_cast<int32_t>(optionsResult)),
                                      "Failed to activate the winget FindPackagesOptions or "
                                      "PackageMatchFilter COM server",
                                      static_cast<int32_t>(optionsResult));
        }

        // An empty options object (no filters, no selectors) has been observed to come
        // back FindPackagesResultStatus::InvalidOptions against some winget versions
        // (docs/com-api.md "known caveats"). A single filter that matches every
        // package - "Id contains the empty string" - works around that without
        // narrowing the result set.
        PackageMatchFilter filter{nullptr};
        const HRESULT filterResult = createInstanceNoThrow(kPackageMatchFilterClsid, filter);
        if (FAILED(filterResult))
        {
            return PackageSourceError(mapHresultToKind(static_cast<int32_t>(filterResult)),
                                      "Failed to activate the winget FindPackagesOptions or "
                                      "PackageMatchFilter COM server",
                                      static_cast<int32_t>(filterResult));
        }

        try
        {
            filter.Field(PackageMatchField::Id);
            filter.Option(PackageFieldMatchOption::ContainsCaseInsensitive);
            filter.Value(L"");
            options.Filters().Append(filter);
        }
        catch (const winrt::hresult_error& error)
        {
            // FindPackagesOptions and PackageMatchFilter are separately registered
            // out-of-process classes, so activating them can fail independently of
            // PackageManager. The property setters and Filters().Append() above are
            // marshalled calls too and can fail after activation already succeeded.
            // Translate here rather than letting an hresult_error escape: callers of
            // IPackageSource are documented to see PackageSourceError only.
            return PackageSourceError(mapHresultToKind(error.code()),
                                      "Failed to activate the winget FindPackagesOptions or "
                                      "PackageMatchFilter COM server",
                                      error.code());
        }

        outOptions = options;
        return std::nullopt;
    }
};

std::unique_ptr<WingetComSource> WingetComSource::tryCreate(std::optional<PackageSourceError>& error)
{
    // new, not std::make_unique: the constructor is private, and make_unique's own
    // implementation - not this static member function - is what would need access to
    // it.
    auto instance = std::unique_ptr<WingetComSource>(new WingetComSource());
    std::optional<PackageSourceError> initError = instance->m_impl->initialize();
    if (initError.has_value())
    {
        error = std::move(initError);
        return nullptr;
    }

    // Clear rather than leave untouched, so a caller that reuses the same std::optional
    // across calls never sees a stale error from an earlier failed attempt.
    error.reset();
    return instance;
}

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

    try
    {
        if (findResult.Status() != FindPackagesResultStatus::Ok)
        {
            throw PackageSourceError(classifyFindPackagesStatus(findResult.Status()),
                                     "PackageCatalog::FindPackages did not return Ok",
                                     static_cast<int32_t>(findResult.ExtendedErrorCode()));
        }
    }
    catch (const winrt::hresult_error& error)
    {
        throw PackageSourceError(mapHresultToKind(error.code()),
                                 "Reading the winget FindPackages result status threw",
                                 error.code());
    }

    std::vector<InstalledPackage> packages;
    try
    {
        // Matches() and the range-for's own iteration machinery are marshalled calls
        // too - C++/WinRT's iterator calls Size()/GetAt() on the out-of-process
        // collection as the loop advances, so the loop scaffolding, not only its body,
        // can throw. The inner catch below is deliberately narrower: it recovers from
        // one bad match. A failure of the collection itself is not recoverable per-item
        // and is caught here instead.
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

                const std::wstring installedLocation = getMetadataOrEmpty(
                    installed, PackageVersionMetadataField::InstalledLocation);
                if (installedLocation.empty())
                {
                    // COM identified this as an installed portable package but did not
                    // report where it lives; there is nothing to scan for executables.
                    // See docs/adr-phase-2.md ADR-0009 for why this is dropped rather
                    // than guessed at (e.g. from a Packages/<id>_<source> naming
                    // convention).
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
    }
    catch (const winrt::hresult_error& error)
    {
        throw PackageSourceError(mapHresultToKind(error.code()),
                                 "Enumerating the winget FindPackages match list threw",
                                 error.code());
    }

    std::sort(packages.begin(), packages.end(),
             [](const InstalledPackage& a, const InstalledPackage& b) { return a.id < b.id; });
    return packages;
}
} // namespace syncwingetlink
