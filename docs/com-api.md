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

## C++/WinRT skeleton (reference)

> Actual type/method names depend on the winget version. Reference the
> `Microsoft.Management.Deployment` projection at build time.

```cpp
#include <winrt/Microsoft.Management.Deployment.h>

using namespace winrt;
using namespace winrt::Microsoft::Management::Deployment;

void EnumerateInstalledPortables()
{
    winrt::init_apartment();

    // Activate PackageManager via a factory
    PackageManager manager{};

    // Installed local catalog
    auto catalogRef = manager.GetLocalPackageCatalog(LocalPackageCatalog::InstalledPackages);
    auto connectResult = catalogRef.Connect();
    if (connectResult.Status() != ConnectResultStatus::Ok)
    {
        // Fall back to FsScanSource
        return;
    }
    auto catalog = connectResult.PackageCatalog();

    // Search all installed packages
    FindPackagesOptions options{};
    auto result = catalog.FindPackages(options);

    for (auto const& match : result.Matches())
    {
        auto pkg = match.CatalogPackage();
        auto installed = pkg.InstalledVersion();
        if (!installed) continue;

        // Get Id / Name / Version / install location, and
        // target only those whose installer type == portable.
        // ...
    }
}
```

## Capabilities / permissions

- COM calls require the `packageQuery` capability, or a Medium+ integrity level (varies by
  environment).
- If App Installer is not installed or the winget policy is disabled, COM activation fails.
  In that case, `--source auto` automatically degrades to `FsScanSource`.

## Fallback behavior (`--source`)

| Value | Behavior |
|---|---|
| `com` | Use the COM API only. Error out on failure. |
| `fs` | Use filesystem scanning only (do not try COM). |
| `auto` (default) | Try COM; degrade to FS scanning on failure. |

## Known caveats

- The COM API exposes identifier, version, and install location, but **may not fully
  return the per-file symlink alias mapping**. In that case, supplement the alias with the
  regex rules in `docs/rules.md`.
- Local catalog enumeration behavior has version-dependent quirks reported. Therefore the
  FS fallback via `auto` is effectively required in practice.
- With C++/WinRT, ensure `winrt::init_apartment()` is called and handle exceptions on
  activation failure.
