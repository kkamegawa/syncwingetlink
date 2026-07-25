# syncwingetlink — Implementation Plan (for AI coding agents)

> A native CLI tool that re-scans the real exes under
> `%LOCALAPPDATA%\Microsoft\WinGet\Packages`, detects command-alias symlinks that are
> missing/broken under `%LOCALAPPDATA%\Microsoft\WinGet\Links`, and recreates them after
> user confirmation.

📖 日本語版は [`PLAN_ja.md`](./PLAN_ja.md) を参照してください。

---

## 1. Background / Problem

- Installing a portable package with winget places the real exe under
  `%LOCALAPPDATA%\Microsoft\WinGet\Packages\<Package>\...`.
- A command-alias symlink is normally created at
  `%LOCALAPPDATA%\Microsoft\WinGet\Links\<alias>.exe`, and because the `Links` directory
  is on `PATH`, the tool is callable from the CLI.
- In some environments this symlink is not created or becomes broken (e.g. a package with
  `PortableCommandAlias` is not reflected in `Links`, or portable installs in general do
  not create symlinks).
- Internally, winget's `PortableInstaller` manages symlinks via
  `CreateSymlink / VerifySymlink`, falling back to a copy when reparse points are
  unsupported.
- As a result, users can only launch the tool by a long real file name such as
  `codex_0.x_x86_64-pc-windows-msvc.exe`, and the `codex` alias does not work.

## 2. Goals

1. Enumerate installed portable packages and their real exes.
2. Compare against existing symlinks (or copies) under `Links` and detect links that are
   "expected but missing / broken".
3. Present the missing/broken links to the user and recreate/repair them with
   `CreateSymbolicLinkW` (Win32) after confirmation.
4. Support **regex replacement rules** to derive the alias name from the real file name,
   e.g. `codex-x86_64-pc-windows-msvc.exe → codex.exe`.

## 3. Data source policy: prefer the winget COM API (important)

Obtain installed portable package information via the winget COM API
(`Microsoft.Management.Deployment` namespace) as the first choice, **not by reading the
sqlite `PortableIndex` directly**.

### Rationale
- The COM API is a **stable, versioned public interface** separate from the CLI, usable
  from Win32 desktop apps via C++/WinRT (both out-of-proc and in-proc).
- The sqlite schema is internal and may change; depending on it directly is fragile.
- Going through the winget service means permissions and integrity checks are handled
  appropriately.

### COM API flow
1. Create a `PackageManager` (via a factory-based COM activation).
2. `GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)` to get the local
   (installed) catalog reference. Optionally compose with
   `CreateCompositePackageCatalog` + `CompositeSearchBehavior.LocalCatalogs`.
3. `PackageCatalogReference.Connect()` → `PackageCatalog`.
4. `FindPackages(FindPackagesOptions)` to enumerate `CatalogPackage` items.
5. From `CatalogPackage.InstalledVersion` (`PackageVersionInfo`), get the identifier
   (`Id`/`Name`), version, and **metadata such as install location**.
6. Filter to packages whose installer type is **portable**.

### Important limitation and mitigation (be honest)
- The COM API exposes metadata such as identifier, version, and install location, but it
  may not fully expose the **per-file symlink alias mapping** (real exe → `<alias>.exe`)
  held by the PortableIndex.
- Therefore **decide the alias name in this priority order**:
  1. COM API metadata (when a `PortableCommandAlias`-equivalent is available)
  2. the regex replacement rules in §7
  3. the raw file name as-is
- Use the two-stage approach "authoritative enumeration via COM → verify real files on
  the filesystem": resolve install location via COM, then scan under it (§6) and compare.

### Implementation options
- Use **C++/WinRT** to project and consume `Microsoft.Management.Deployment` (recommended).
- Default to the out-of-proc COM server (`WindowsPackageManagerServer.exe`) via a
  `CoCreateInstance`-equivalent factory activation.
- Note that COM calls require the `packageQuery` capability (or Medium+ integrity level).
- If COM is unavailable (App Installer not installed, policy disabled, etc.), switch to
  the **FS-scan fallback** in §9 (controlled by the `--source` option).

## 3b. Non-goals (out of scope for the first release)

- Writing to / maintaining the integrity of winget's sqlite DB (`PortableIndex`).
  Reads prefer the COM API; direct sqlite reads are a last-resort fallback only (see §9).
- Managing the machine scope (`C:\Program Files\WinGet\Links`) — user scope only in the
  first release.
- Replacing winget's own install/uninstall processing.
- Registering the PATH environment variable (first release only verifies/warns).

## 4. Target environment / constraints

- **OS**: Windows 11 24H2 (build 26100) or later.
- **Language / API**: C++ (C++20) + Win32 API. CRT/STL allowed.
- **Architecture**: x64 primary; arm64 also a build target.
- **Build system**: **MSBuild** (`.sln` + `.vcxproj`), driven by Visual Studio 2026.
  Platform toolset **v145**, Windows SDK 10.0.26100.0. CMake is not used — see
  [`adr.md`](./adr.md) ADR-0001.
- **Unit tests**: **MSTest** — the Microsoft Unit Testing Framework for C++
  (`CppUnitTest.h`), executed with `vstest.console.exe`. See ADR-0002.
- **Dependencies**: standard library and Windows APIs first (ADR-0005). If a native
  dependency ever becomes necessary it is managed with **vcpkg** in manifest mode; NuGet
  is not used (ADR-0007). C++/WinRT comes from the Windows SDK, so the expected
  dependency set is empty.
- **Encoding**: internally UTF-16 (`std::wstring`); long-path support via the `\\?\`
  prefix.
- **Permissions**:
  - On Windows 11 with Developer Mode on, non-admins can create symlinks
    (`SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`).
  - If Developer Mode is off, admin elevation is required → detect and message clearly.

## 5. Architecture / module layout

The solution is split into **three projects**. The core is a static library so that both
the executable and the MSTest DLL can link it — a C++ MSTest project must be a DLL and
cannot link an executable's object files. See [`adr.md`](./adr.md) ADR-0002 / ADR-0004.

| Project | Type | Output |
|---|---|---|
| `src/syncwingetlink.core.vcxproj` | StaticLibrary | `syncwingetlink.core.lib` |
| `src/syncwingetlink.vcxproj` | Application | `syncwingetlink.exe` |
| `tests/syncwingetlink.tests.vcxproj` | DynamicLibrary | `syncwingetlink.tests.dll` |

```
syncwingetlink/
├─ syncwingetlink.sln          # VS2026 solution: Debug|Release × x64|ARM64
├─ Directory.Build.props       # shared properties (toolset v145, SDK, output paths)
├─ props/
│  └─ syncwingetlink.common.props # shared compiler/linker settings (C++20, /W4 /WX, CRT)
├─ src/
│  ├─ syncwingetlink.core.vcxproj # static library: cli/ core/ rules/ tui/
│  ├─ syncwingetlink.vcxproj   # executable: main.cpp only
│  ├─ app.manifest             # longPathAware=true, requestedExecutionLevel=asInvoker
│  ├─ main.cpp                 # entry point, arg parsing, mode dispatch
│  ├─ cli/
│  │  ├─ ArgParser.{h,cpp}     # flag parsing (--help/--dry-run/--tui/--rules/--source)
│  │  └─ Console.{h,cpp}       # UTF-8/UTF-16 output, coloring, confirmation prompt
│  ├─ core/
│  │  ├─ Paths.{h,cpp}         # known-folder resolution (SHGetKnownFolderPath) / Links path
│  │  ├─ IPackageSource.h      # abstract IF for installed-package enumeration (COM/FS switch)
│  │  ├─ WingetComSource.{h,cpp} # ★COM API impl (Microsoft.Management.Deployment, C++/WinRT)
│  │  ├─ FsScanSource.{h,cpp}  # fallback: recursive scan of Packages, exe enumeration
│  │  ├─ LinkInspector.{h,cpp} # judge symlink state under Links (missing/broken/OK)
│  │  ├─ AliasResolver.{h,cpp} # decide alias by metadata > regex > raw name
│  │  ├─ SymlinkService.{h,cpp}# symlink create/delete/verify (Win32)
│  │  └─ Model.h               # PackageExe / InstalledPackage / LinkStatus / Plan types
│  ├─ rules/
│  │  ├─ RuleSet.{h,cpp}       # load/apply replacement rules
│  │  └─ default_rules.*       # default rules (embedded or JSON)
│  └─ tui/
│     └─ TuiApp.{h,cpp}        # interactive UI for --tui (Console Virtual Terminal)
├─ tests/
│  └─ syncwingetlink.tests.vcxproj # MSTest (Microsoft Unit Testing Framework for C++)
└─ docs/
   ├─ rules.md                 # replacement rule format & samples
   ├─ com-api.md               # COM API usage, activation, capability notes
   ├─ task.md                  # work log
   └─ adr.md                   # architecture decision records
```

### Data-source abstraction (switching COM and fallback)
- Introduce `IPackageSource` so `WingetComSource` (default) and `FsScanSource` (fallback)
  are interchangeable.
- Select via `--source com|fs|auto` (default `auto`). `auto` tries COM and degrades to FS
  on failure.
- `WingetComSource` returns "identifier, version, install location, and alias if
  available"; `LinkInspector` / `AliasResolver` are shared source-independent logic.

## 6. Processing flow

1. **Path resolution**: build `...\Microsoft\WinGet\Links` from
   `SHGetKnownFolderPath(FOLDERID_LocalAppData)` (overridable with `--links-dir`).
2. **Enumerate installed packages (COM first)**: `WingetComSource` runs
   `PackageManager` → `GetLocalPackageCatalog(InstalledPackages)` → `Connect()` →
   `FindPackages()` to get portable packages plus their **install location**, alias (if
   available), and version.
   - When COM is unavailable, fall back to `FsScanSource` and recursively scan `Packages`
     to collect `*.exe` (`recursive_directory_iterator`). Reparse points are not followed
     by default (loop prevention).
3. **Alias resolution**: determine `<alias>.exe` in priority order:
   (1) COM metadata (`PortableCommandAlias`-equivalent) → (2) `AliasResolver` regex rules
   → (3) raw file name.
4. **Comparison**: judge the state of `Links\<alias>.exe`.
   - `Missing`: the link does not exist
   - `Broken`: a symlink exists but its target is missing / points to a different real file
   - `Mismatch`: the real file points elsewhere (stale update)
   - `Ok`: correctly linked
5. **Plan generation**: list `Missing/Broken/Mismatch` as repair candidates.
6. **Confirmation**: unless `--yes`, confirm one-by-one or in bulk (a checklist in TUI).
7. **Execution**: create with `CreateSymbolicLinkW`. Delete broken links first, then
   recreate. With `--dry-run`, show the plan only, no execution.
8. **Result report**: summarize created/skipped/failed and reflect in the exit code.

## 7. Alias regex rules (key requirement)

- Purpose: strip platform suffixes such as in `codex-x86_64-pc-windows-msvc.exe` to derive
  the proper alias name `codex.exe`.
- Rules are an **ordered list**. Apply the first matching rule (an "apply all" mode may be
  considered).
- Rule structure (JSON example):

```json
{
  "version": 1,
  "rules": [
    {
      "name": "strip-rust-target-triple",
      "pattern": "^(.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
      "replacement": "$1.exe",
      "flags": ["ignorecase"]
    },
    {
      "name": "strip-version-and-arch",
      "pattern": "^(.+?)[-_]v?\\d+\\.\\d+.*?(windows|win)?[-_]?(amd64|x64|arm64)?\\.exe$",
      "replacement": "$1.exe"
    }
  ]
}
```

- Implementation: standard C++ `std::regex` (ECMAScript grammar), or consider RE2.
  - `std::regex` does not support named capture groups, so numbered captures are the
    default. The first release uses **numbered captures (`$1`)** as the default format;
    named captures are a future enhancement.
- Rule load priority:
  1. JSON specified via `--rules <path>`
  2. `%LOCALAPPDATA%\syncwingetlink\rules.json` (user settings)
  3. default rules embedded in the binary
- Provide a `test-rule "<filename>"` subcommand to dry-check which rule applies to a given
  real file name and what alias it yields.

## 8. CLI spec (proposal)

```
syncwingetlink [command] [options]

Commands:
  scan            detect only (read-only, default command)
  fix             create/repair missing/broken links
  test-rule NAME  show the replacement result for a real file name

Options:
  --source com|fs|auto  package enumeration source (default auto: COM first → FS fallback)
  --tui                 run in interactive TUI mode
  --dry-run             show the plan without executing (for fix)
  --yes, -y             skip all confirmations and execute
  --rules <path>        path to a replacement-rules JSON
  --packages-dir <p>    override the Packages directory
  --links-dir <p>       override the Links directory
  --include <glob>      narrow target packages/exes
  --exclude <glob>      exclude
  --json                emit results as JSON (scripting)
  --verbose / --quiet   log level
  --version / --help
```

### Exit codes
- `0`: success (nothing to fix or fixed)
- `1`: fix needed but not performed (e.g. missing detected in scan with `--fail-on-missing`)
- `2`: insufficient permission (Developer Mode off & non-admin, cannot create symlink)
- `3`: argument/config error (e.g. invalid rules JSON)
- `10`: some repairs failed

## 9. Fallback / future enhancements

- **FS-scan fallback (included in first release)**: when the COM API is unavailable,
  switch to `FsScanSource`, which recursively scans `Packages` to enumerate exes.
- **Direct sqlite read (last-resort fallback, optional)**: only when both COM and FS are
  insufficient, consider reading each package's `PortableIndex` (sqlite) **read-only** to
  obtain `PortableCommandAlias` (lowest priority due to schema-change risk).
- machine-scope support (requires admin).
- verify whether `Links` is on `PATH`, and assist registration if not.
- winget event integration (auto-repair via a post-install hook).
- a subcommand to register a scheduled task for periodic runs.

## 10. Technical risks / notes

- **COM API availability / capability**: COM calls require the `packageQuery` capability
  (or Medium+ integrity level). Activation fails when App Installer is not installed or the
  policy is disabled, so degrade to FS with `--source auto`.
- **Gap between COM and alias info**: if the COM API does not return per-file alias
  mapping, supplement with regex rules (§3, §7). Do not depend on COM alone.
- **C++/WinRT init**: call `winrt::init_apartment()` appropriately and handle exceptions
  when out-of-proc server activation fails.
- **Developer Mode / permission**: symlink creation is permission-dependent. Pass
  `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` to `CreateSymbolicLinkW`; on failure,
  distinguish the cause (permission vs. Developer Mode off) and advise.
- **Long paths**: `Packages` can be deep → use the `\\?\` prefix and set the manifest
  `longPathAware`.
- **Symlink judgment**: resolve the target via `GetFileAttributesW`'s
  `FILE_ATTRIBUTE_REPARSE_POINT` and `DeviceIoControl(FSCTL_GET_REPARSE_POINT)`. Treat the
  case where winget placed a copy (a normal file) as `Mismatch`.
- **Duplicate aliases**: if multiple exes resolve to the same alias, warn as a collision
  and let the user choose (do not auto-create).
- **Encoding / output**: use `SetConsoleOutputCP(CP_UTF8)` or wide APIs so non-ASCII paths
  render correctly.
- **Respect existing links**: do not change `Ok` links that winget created correctly.

## 11. Definition of Done

- [ ] Builds and runs on Windows 11 24H2 (x64/arm64).
- [ ] Enumerates installed portable packages via the COM API.
- [ ] Automatically falls back to FS scanning when COM is unavailable (`--source auto`).
- [ ] `scan` correctly classifies and lists missing/broken/ok.
- [ ] `fix` can create missing symlinks via a confirmation prompt.
- [ ] `--dry-run` outputs the plan with no side effects.
- [ ] Regex rules derive `codex-x86_64-pc-windows-msvc.exe → codex.exe`.
- [ ] `--tui` allows interactive checking and batch creation.
- [ ] On Developer Mode off, states the permission error and returns exit code 2.
- [ ] `AliasResolver` / `RuleSet` / `LinkInspector` have MSTest unit tests.
- [ ] **All unit tests pass** — `vstest.console.exe` reports green. A successful build is
      not sufficient evidence.
- [ ] **No dependency has a known vulnerability**; every dependency is MIT-compatible and
      justified in the PR that introduced it.
