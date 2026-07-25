# Using the winget COM API

syncwingetlink uses the winget **COM API (`Microsoft.Management.Deployment` namespace)**
as the first choice for enumerating installed portable packages. When it is unavailable,
it automatically falls back to filesystem scanning (`FsScanSource`).

📖 日本語版は [`com-api_ja.md`](./com-api_ja.md) を参照してください。

## Why the COM API

- It is a **stable, versioned public interface** provided separately from the CLI.
- It is usable from Win32 desktop apps via C++/WinRT, both out-of-proc and in-proc.
- The sqlite (`PortableIndex`) internal schema is undocumented and may change, so a direct
  read is avoided and kept as a last-resort fallback.

## Enumeration flow

```
PackageManager
   └─ GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)
        └─ PackageCatalogReference.Connect()  →  PackageCatalog
             └─ FindPackages(FindPackagesOptions)  →  [CatalogPackage]
                  └─ CatalogPackage.InstalledVersion (PackageVersionInfo)
                        →  Id / Name / Version / install location / (alias if available)
```

1. Create a `PackageManager` (COM activation via a factory).
2. `GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)` to get the installed
   catalog reference. Optionally `CreateCompositePackageCatalog` +
   `CompositeSearchBehavior.LocalCatalogs`.
3. `PackageCatalogReference.Connect()` to get the `PackageCatalog`.
4. `FindPackages()` to enumerate `CatalogPackage` items.
5. From `CatalogPackage.InstalledVersion`, get identifier, version, and **metadata such as
   install location**.
6. Filter to packages whose installer type is **portable**.

## C++/WinRT activation (as implemented)

> Confirmed against Microsoft.DesktopAppInstaller 1.29.280.0. Reference the generated
> `Microsoft.Management.Deployment` projection for the exact method signatures your SDK
> produces; the activation requirements below are the part that does not come from the
> winmd and is easy to get wrong. See `docs/adr-phase-2.md` ADR-0009.

**`winrt::init_apartment()` is not used.** It throws when the calling thread already has a
different concurrency model initialized (`RPC_E_CHANGED_MODE`), which
`WingetComSource`/`core/ComApartment` must tolerate rather than fail on, since a static
library does not own the process's apartment. Use `CoInitializeEx(nullptr,
COINIT_MULTITHREADED)` directly instead.

**Every activatable class in this namespace needs its own fixed CLSID when called from an
unpackaged desktop process** — not just `PackageManager`. None of them are registered for
ordinary WinRT activation (`T t{};`) outside a package graph; each is exposed as a
separately registered out-of-process `ExeServer` class in the installed Desktop App
Installer package's `AppxManifest.xml`:

| Class | CLSID |
|---|---|
| `PackageManager` | `C53A4F16-787E-42A4-B304-29EFFB4BF597` |
| `FindPackagesOptions` | `572DED96-9C60-4526-8F92-EE7D91D38C1A` |
| `PackageMatchFilter` | `D02C9DAF-99DC-429C-B503-4E504E4AB000` |

Activate each with `winrt::create_instance<T>(clsid, CLSCTX_LOCAL_SERVER)`.
`FindPackagesOptions` in particular cannot be constructed as `FindPackagesOptions{}` —
that throws `REGDB_E_CLASSNOTREG`.

```cpp
#include <winrt/Microsoft.Management.Deployment.h>

using namespace winrt::Microsoft::Management::Deployment;

constexpr GUID kPackageManagerClsid = { /* see table above */ };
constexpr GUID kFindPackagesOptionsClsid = { /* see table above */ };
constexpr GUID kPackageMatchFilterClsid = { /* see table above */ };

// CoInitializeEx(nullptr, COINIT_MULTITHREADED) must already have been called on this
// thread (tolerating RPC_E_CHANGED_MODE) - see core/ComApartment.
PackageManager manager =
    winrt::create_instance<PackageManager>(kPackageManagerClsid, CLSCTX_LOCAL_SERVER);

auto catalogRef = manager.GetLocalPackageCatalog(LocalPackageCatalog::InstalledPackages);
auto connectResult = catalogRef.Connect();
if (connectResult.Status() != ConnectResultStatus::Ok)
{
    // Fall back to FsScanSource
    return;
}
auto catalog = connectResult.PackageCatalog();

FindPackagesOptions options =
    winrt::create_instance<FindPackagesOptions>(kFindPackagesOptionsClsid, CLSCTX_LOCAL_SERVER);
// An empty options object has been observed to return FindPackagesResultStatus::
// InvalidOptions against some winget versions; append one permissive filter
// (Field=Id, Option=ContainsCaseInsensitive, Value=L"") built the same way, via
// kPackageMatchFilterClsid, to match every package instead.

auto result = catalog.FindPackages(options);
for (auto const& match : result.Matches())
{
    auto pkg = match.CatalogPackage();
    auto installed = pkg.InstalledVersion();
    if (!installed) continue;

    // GetMetadata(PackageVersionMetadataField::InstallerType) returns an hstring, not the
    // PackageInstallerType enum - there is no enum getter. Compare it to "portable"
    // ordinally (CompareStringOrdinal), not with towlower/_wcsicmp (locale-dependent).
}
```

## Capabilities / permissions

- COM calls require the `packageQuery` capability, or a Medium+ integrity level (varies by
  environment).
- If App Installer is not installed or the winget policy is disabled, COM activation fails.
  In that case, `--source auto` automatically degrades to `FsScanSource`.
- Out-of-process COM server activation (`WindowsPackageManagerServer.exe`) may not work in
  every automated environment (e.g. no interactive window station/desktop). Treat a
  process-level failure at the activation step as equivalent to
  `PackageSourceErrorKind::AppInstallerMissing`/`ServerUnavailable` for `--source auto`
  purposes, and verify `--source com` manually in a normal interactive session rather than
  assuming an automated test proves it works everywhere.

## Fallback behavior (`--source`)

| Value | Behavior |
|---|---|
| `com` | Use the COM API only. Error out on failure. |
| `fs` | Use filesystem scanning only (do not try COM). |
| `auto` (default) | Try COM; degrade to FS scanning on failure. |

## Known caveats

- **There is no per-file alias mapping in this API at all** — not "may not fully return
  it". `PackageVersionMetadataField` has exactly six members (`InstallerType`,
  `InstalledScope`, `InstalledLocation`, `StandardUninstallCommand`,
  `SilentUninstallCommand`, `PublisherDisplayName`); `IPackageVersionInfo2/3/4` add only
  version comparison, publisher, and installer metadata — nothing alias-related. Alias
  resolution is entirely the job of the M3 regex rules in `docs/rules.md`; see
  `docs/adr-phase-2.md` ADR-0009.
- A portable package whose `GetMetadata(InstalledLocation)` comes back empty is dropped by
  `WingetComSource` rather than guessed at from a directory-naming convention — there is
  nothing to scan for executables without it.
- Local catalog enumeration behavior has version-dependent quirks reported. Therefore the
  FS fallback via `auto` is effectively required in practice.
