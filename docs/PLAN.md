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
  from Win32 desktop apps via C++/WinRT. This project activates it out-of-proc only
  (`CLSCTX_LOCAL_SERVER`); see `docs/com-api.md` "Out-of-proc vs in-proc" for what that
  choice implies and why no in-proc fallback exists.
- The sqlite schema is internal and may change; depending on it directly is fragile.
- Going through the winget service means permissions and integrity checks are handled
  appropriately.

### COM API flow
1. Create a `PackageManager` (via a factory-based COM activation).
2. `GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)` to get the local
   (installed) catalog reference. (`CreateCompositePackageCatalog` +
   `CompositeSearchBehavior.LocalCatalogs` was considered here but is **not used** by the
   shipped implementation — only `GetLocalPackageCatalog` is called; see
   `docs/com-api.md` "Enumeration".)
3. `PackageCatalogReference.Connect()` → `PackageCatalog`.
4. `FindPackages(FindPackagesOptions)` to enumerate `CatalogPackage` items.
5. From `CatalogPackage.InstalledVersion` (`PackageVersionInfo`), get the identifier
   (`Id`/`Name`), version, and **metadata such as install location**.
6. Filter to packages whose installer type is **portable**.

### Important limitation and mitigation (be honest)
- The COM API exposes metadata such as identifier, version, and install location, but it
  does **not** expose the **per-file symlink alias mapping** (real exe → `<alias>.exe`)
  held by the PortableIndex — confirmed empirically (`docs/adr-phase-2.md` ADR-0009):
  `PackageVersionMetadataField` and `IPackageVersionInfo2/3/4` have no such field, so
  there is no `PortableCommandAlias`-equivalent to read. This is not merely "may not fully
  expose" — the API has no alias field at all, so this source is never consulted.
- Therefore **decide the alias name in this priority order**:
  1. the regex replacement rules in §7
  2. the raw file name as-is
- Use the two-stage approach "authoritative enumeration via COM → verify real files on
  the filesystem": resolve install location via COM, then scan under it (§6) and compare.

### Implementation options
- Use **C++/WinRT** to project and consume `Microsoft.Management.Deployment` (recommended,
  and what is implemented).
- Activate the out-of-proc COM server (`WindowsPackageManagerServer.exe`) via a
  non-throwing `CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, guid_of<T>(), ...)`
  call (functionally equivalent to `winrt::create_instance<T>(clsid,
  CLSCTX_LOCAL_SERVER)`, without its C++ exception on failure — `docs/adr-phase-9.md`
  ADR-0040) with the fixed CLSIDs recorded in `docs/com-api.md` — there is no in-process
  fallback.
- The capability/permission requirements for calling this API from an unpackaged process
  were not established as a documented, citable fact (no first-party Microsoft
  documentation states one, and `src/app.manifest` declares no AppX capability because it
  is a plain unpackaged manifest); treat COM availability as something to verify on the
  target environment (`scan --source com --verbose`) rather than assume. See
  `docs/com-api.md` "Capabilities / permissions" and `docs/adr-phase-8.md` ADR-0037 for a
  concrete, environment-specific failure this verification reproduced.
- If COM is unavailable (App Installer not installed, policy disabled, activation fails
  for any other reason, etc.), switch to the **FS-scan fallback** in §9 (controlled by the
  `--source` option). Under `--source auto` this degrade is unconditional on the kind of
  failure — see `docs/com-api.md` "Failure and fallback".

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
│  │  ├─ ComApartment.{h,cpp}  # process-wide CoInitializeEx RAII; see adr-phase-2.md ADR-0009
│  │  ├─ PackageSourceError.{h,cpp} # typed enumeration-failure exception + HRESULT mapping
│  │  ├─ ExecutableScanner.{h,cpp} # shared *.exe walk used by WingetComSource and FsScanSource
│  │  ├─ FsScanSource.{h,cpp}  # fallback: recursive scan of Packages, exe enumeration
│  │  ├─ PackageSourceFactory.{h,cpp} # --source com|fs|auto selection + COM→FS degrade
│  │  ├─ PackageFilter.{h,cpp} # --include/--exclude glob filtering over enumerated packages
│  │  ├─ LinkInspector.{h,cpp} # judge symlink state under Links (missing/broken/OK)
│  │  ├─ AliasResolver.{h,cpp} # decide alias by regex rule > raw name (no COM tier: see §3)
│  │  ├─ SymlinkService.{h,cpp}# symlink create/delete/verify (Win32)
│  │  └─ Model.h               # PackageExe / InstalledPackage / LinkStatus / Plan types
│  ├─ rules/
│  │  ├─ RuleSet.{h,cpp}       # load/validate/apply replacement rules
│  │  ├─ DefaultRules.{h,cpp}  # the embedded default rules
│  │  └─ RuleSetSelector.{h,cpp} # explicit --rules > user rules.json > embedded (ADR-0013)
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
- `WingetComSource` returns identifier, version, and install location (never a per-file
  alias — see §3); `LinkInspector` / `AliasResolver` are shared source-independent logic.

## 6. Processing flow

1. **Path resolution**: build `...\Microsoft\WinGet\Links` from
   `SHGetKnownFolderPath(FOLDERID_LocalAppData)` (overridable with `--links-dir`).
2. **Enumerate installed packages (COM first)**: `WingetComSource` runs
   `PackageManager` → `GetLocalPackageCatalog(InstalledPackages)` → `Connect()` →
   `FindPackages()` to get portable packages plus their **install location** and version
   (never a per-file alias — see §3).
   - When COM is unavailable, fall back to `FsScanSource` and recursively scan `Packages`
     to collect `*.exe` (`recursive_directory_iterator`). Reparse points are not followed
     by default (loop prevention).
3. **Alias resolution**: determine `<alias>.exe` in priority order:
   (1) `AliasResolver` regex rules → (2) raw file name. There is no COM-metadata tier —
   see §3's "Important limitation and mitigation."
4. **Comparison**: judge the state of `Links\<alias>.exe` (implemented as `LinkInspector`;
   see `docs/adr-phase-3.md` ADR-0014 for the authoritative classification contract).
   - `Missing`: the entry does not exist
   - `Broken`: a symbolic link exists, but its target does not currently exist
   - `Mismatch`: a regular file, a non-symlink reparse point, or a symbolic link that
     resolves to a different, existing file (not the expected package executable)
   - `Ok`: a symbolic link that resolves to the expected package executable
5. **Plan generation**: list `Missing/Broken/Mismatch` as repair candidates.
6. **Confirmation**: unless `--yes`, confirm one-by-one or in bulk (a checklist in TUI).
7. **Execution**: implemented as `SymlinkService::repairLink()`; see `docs/adr-phase-4.md`
   ADR-0018 for the authoritative state machine. Always re-inspects each candidate first -
   a candidate's status from step 5 is never trusted as permission to mutate, so a stale
   candidate can still resolve to `SkippedOk` or `RefusedMismatch` here even though step 5
   only forwarded `Missing`/`Broken`/`Mismatch` candidates. From that fresh result: creates
   a missing link with `CreateSymbolicLinkW`; for a broken link, deletes first, then
   recreates via the same creation step; a healthy or mismatched entry is never touched.
   Every successful creation is re-verified by a second, post-mutation inspection. With
   `--dry-run`, the fresh re-inspection still runs and can report any of the four outcomes
   (`WouldCreate`/`WouldReplaceBroken`/`SkippedOk`/`RefusedMismatch`), but delete, create,
   and that post-mutation verification step are never invoked.
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

  Once selected, a tier's rules **replace** any lower tier entirely rather than merging
  with it. An explicit `--rules <path>` must exist and parse — any failure is a
  configuration error (exit code 3). The user rules file is optional: absence falls
  through to the embedded defaults, but a file that exists and fails to parse is a
  configuration error too, not a silent fallback — see `docs/adr-phase-2.md` ADR-0013.
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
  --fail-on-missing     scan exits 1 if a Missing/Broken/Mismatch candidate is found
  --no-color            disable colored/VT output regardless of TTY state (also honors
                        the NO_COLOR environment variable)
  --version / --help
```

### `scan`/`fix` console output

`scan`'s human-readable output (and `fix`'s pre-batch preview, printed before its own
`[current/total]` progress lines) groups candidates into two tables, NG always first:
NG collects `Missing`/`Broken`/`Mismatch`; OK collects `Ok`. Each table has four
columns - `package | status | alias | target` - sorted within the group by alias
(ordinal case-insensitive ascending). A group with zero items renders as just its
heading followed by a bare `nothing` line rather than an empty table:

```
NG
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
package                   | status  | alias       | target
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
GitHub.Copilot.Prerelease | Missing | copilot.exe | C:\Users\user\AppData\Local\Microsoft\WinGet\Packages\GitHub.Copilot.Prerelease_Microsoft.Winget.Source_8wekyb3d8bbwe\copilot.exe
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

OK
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
package                   | status  | alias       | target
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
AgileBits.1Password.CLI   | Ok      | op.exe      | C:\Users\user\AppData\Local\Microsoft\WinGet\Packages\AgileBits.1Password.CLI_Microsoft.Winget.Source_8wekyb3d8bbwe\op.exe
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
```

Column widths (`package`/`status`/`alias`) are computed once across both groups so the
two tables always line up; `target` is the last column and is never padded or
truncated. `--json` output (schema, fields, and item ordering) is unchanged by this
format - it reflects `scan`/`fix`'s underlying candidate list directly, not the
grouped/sorted console presentation. See `docs/adr-phase-9.md` ADR-0038.

`--include`/`--exclude` take shell-style wildcards (`*` and `?` only — no character
classes, no path semantics), matched per executable against either the package identifier
or the executable file name, ordinal and case-insensitive. An exclude match always beats
an include match. See `docs/adr-phase-2.md` ADR-0010.

`--links-dir`/`--packages-dir`/`--rules` are rejected only if empty or if they resolve to
a `\\.\` device path; they are **not** required to already exist. An absent Packages
directory is a normal, tolerated state (ADR-0010), and an absent Links directory is the
exact condition `fix` exists to correct. See `docs/adr-phase-5.md` ADR-0020.

### `--tui`

`fix --tui` runs the M7 interactive checklist instead of the line-oriented
confirm-per-item flow (`docs/adr-phase-6.md` ADR-0026-0028). Its real, implemented
behavior - documented here rather than left as "run in interactive TUI mode", per issue
#64:

- **Parse-time conflicts, exit code 3**: `--tui` combined with `scan`, `test-rule`,
  `--json`, or `--yes` is rejected by `ArgParser` before anything is enumerated. `--tui`
  is meaningful only for an interactive `fix`; the other three all imply an unattended or
  non-interactive invocation.
- **Non-interactive fallback, no TUI escape sequence emitted**: if `console.stdinInteractive()`,
  `console.stdoutInteractive()`, or `console.vtEnabled()` is false, or the terminal
  session otherwise fails to start, `cli::Dispatch` prints one warning line to stderr
  and falls back to the existing line-oriented confirmation flow.
- `--dry-run` and `--no-color` both remain compatible with `--tui`.

### `--verbose` / `--quiet` (log level)

`--verbose` and `--quiet` set `AppOptions::logLevel` to `Verbose`/`Quiet`; the default is
`Normal`. Repeating either flag, in either order, is last-wins - `--verbose --quiet`
leaves `Quiet` in effect, `--quiet --verbose` leaves `Verbose` in effect - matching every
other repeatable `ArgParser` option (e.g. `--source`). `cli::Console` gates every line on
a `MessageImportance` (`Supplementary`/`Normal`/`Diagnostic`) against the active log
level; the three levels form a strict chain, each a superset of the one before:

| Log level | Emits |
|---|---|
| `Quiet` | `Normal`-importance lines only (warnings, errors, the `--json` document) |
| `Normal` (default) | `Supplementary` + `Normal` |
| `Verbose` | `Supplementary` + `Normal` + `Diagnostic` |

`Supplementary` covers routine, skippable-under-`--quiet` output: `scan`'s OK table (and
its NG table/heading too, when there are zero NG items - `docs/adr-phase-9.md`
ADR-0038), `fix`'s pre-batch preview (both its NG and OK tables, unconditionally),
`fix`'s per-item progress lines, and the batch summary headings. Anything a user must
act on - `scan`'s NG table when it has at least one row, warnings, errors - is `Normal`
importance and is never suppressed by `--quiet`. `Diagnostic` is exclusively
`--verbose`'s additional stderr-only reporting (never stdout, regardless of `--json` -
ADR-0022's stdout-purity rule is unaffected by log level): the resolved effective
`Links`/`Packages` directories, the package source actually used (including a COM→FS
`auto` degrade), and which rule tier was selected (`--rules`, the user rules file, or the
embedded defaults). See `docs/adr-phase-6.md` ADR-0030.

### Exit codes
- `0`: success (nothing to fix or fixed)
- `1`: fix needed but not performed (e.g. missing detected in scan with `--fail-on-missing`)
- `2`: insufficient permission (Developer Mode off & non-admin, cannot create symlink)
- `3`: argument/config error (e.g. invalid rules JSON, unknown option, an invalid
  `--links-dir`/`--packages-dir`/`--rules` value, `--json` combined with `fix` and no
  `--yes` - a parse-time conflict, not a statement about confirmation-prompt behavior
  at runtime - or an unexpected link-inspection failure). An EOF or declined
  confirmation during an actual `fix` run is normal refusal, not an error: that
  candidate is simply run in dry-run mode instead, contributing to exit `0`/`10` like
  any other outcome, never to `3`.
- `4`: package enumeration failed (an explicit `--source com`/`--source fs` could not
  enumerate at all; `auto` only reaches this if both COM and the FS fallback fail)
- `10`: some repairs failed

### `--json` output schema

When `--json` is set, stdout carries exactly one JSON document and nothing else -
diagnostics, warnings, and prompts move to stderr instead (see `docs/adr-phase-5.md`
ADR-0022). `schemaVersion` follows `rules.json`'s own precedent (`docs/rules.md`) for a
stable, versioned document shape.

`scan` output:

```json
{
  "schemaVersion": 1,
  "command": "scan",
  "repairItems": [
    {
      "executable": "<path>",
      "alias": "<name>.exe",
      "linkPath": "<path>",
      "status": "Ok" | "Missing" | "Broken" | "Mismatch",
      "entryKind": "None" | "RegularFile" | "SymbolicLink" | "OtherReparsePoint",
      "existingTarget": "<path>" | null
    }
  ],
  "collisions": [
    { "alias": "<name>.exe", "executables": ["<path>", "<path>"] }
  ]
}
```

`fix` output:

```json
{
  "schemaVersion": 1,
  "command": "fix",
  "results": [
    {
      "item": { "...": "a repairItems entry, as observed immediately before acting" },
      "outcome": "WouldCreate" | "WouldReplaceBroken" | "Created" | "ReplacedBroken" |
                 "SkippedOk" | "RefusedMismatch",
      "verifiedItem": { "...": "the post-creation re-inspection" } | null
    }
  ],
  "collisions": [
    { "alias": "<name>.exe", "executables": ["<path>", "<path>"] }
  ]
}
```

`collisions` here lists candidates excluded from repair before `fix` ran at all - they
never appear inside `results`.

Every string value (`executable`, `alias`, `linkPath`, `existingTarget`, the elements of
`executables`) is sanitized the same way console output is - see
`docs/adr-phase-5.md` ADR-0021's `sanitizeForDisplay()` - before JSON-escaping, so a
crafted file name cannot inject raw control characters into the document. String
escaping follows RFC 8259; a UTF-16 surrogate pair in a Windows file name encodes as its
one non-BMP code point, and an unpaired surrogate becomes U+FFFD (see ADR-0022) rather
than invalid UTF-8 or a hard failure.

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

- **COM API availability**: activation can fail for several reasons — App Installer not
  installed, policy disabled, or (confirmed by live verification,
  `docs/adr-phase-8.md` ADR-0037) activation of the typed WinRT interface failing on a
  given machine/App Installer version even though the winget CLI itself works. No
  documented capability requirement could be confirmed as fact; degrade to FS with
  `--source auto`, which is unconditional on the failure kind. See `docs/com-api.md`
  "Capabilities / permissions" and "Failure and fallback".
- **The COM API has no per-file alias mapping at all** (not merely a gap that might be
  filled) — regex rules (§3, §7) are the only source. Do not depend on COM alone.
- **C++/WinRT init**: `winrt::init_apartment()` is deliberately **not** used anywhere in
  this codebase — it throws on `RPC_E_CHANGED_MODE`, which a static library must
  tolerate. `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` is called directly instead,
  once, process-wide, by `main.cpp`. See `docs/com-api.md` "Activation" and
  `docs/adr-phase-2.md` ADR-0009.
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

- [ ] Builds and runs on Windows 11 24H2 (x64/arm64) - `Debug`/`Release` × `x64`/`ARM64`
      all build clean as of every milestone through M9; x64 has been run repeatedly
      (most recently for `docs/adr-phase-8.md` ADR-0037). ARM64 remains cross-built, not
      run, per `docs/adr.md` open item 3 - left unchecked until it is actually executed
      on an ARM64 host.
- [x] Enumerates installed portable packages via the COM API - `WingetComSource` (M2,
      issues #27-#31); see `docs/com-api.md` for the full activation/enumeration
      contract.
- [x] Automatically falls back to FS scanning when COM is unavailable (`--source auto`) -
      `PackageSourceFactory`'s `AutoPackageSource` (M2, issue #34), exercised by M8's
      `--source fs` integration coverage (#61) and by a live `--source auto` run
      recorded in `docs/adr-phase-8.md` ADR-0037.
- [x] `scan` correctly classifies and lists missing/broken/ok - `LinkInspector` (M4,
      issues #44-#48), dispatched by `cli::run()`'s `scan` command (M6, issue #56).
- [x] `fix` can create missing symlinks via a confirmation prompt - `SymlinkService` (M5,
      issues #49-#50) plus `Console`'s confirmation prompt / `--yes` (M6, issue #54).
- [x] `--dry-run` outputs the plan with no side effects - issue #52, proven with zero
      delete/create/permission-query callbacks invoked across every state.
- [x] Regex rules derive `codex-x86_64-pc-windows-msvc.exe → codex.exe` - `AliasResolver`
      + embedded `DefaultRules` (M3, issues #38-#42), covered by
      `tests/AliasResolverTests.cpp`/`AliasPipelineTests.cpp`.
- [x] `--tui` allows interactive checking and batch creation (issues #58, #59, #60;
      `docs/adr-phase-6.md` ADR-0026 through ADR-0028).
- [x] On Developer Mode off, states the permission error and returns exit code 2 - issue
      #51 (`SymlinkServiceError` permission/Developer-Mode distinction), mapped to
      `ExitCode::InsufficientPermission` in `src/cli/Dispatch.cpp`.
- [x] `AliasResolver` / `RuleSet` / `LinkInspector` / `SymlinkService` have MSTest unit
      tests - `tests/AliasResolverTests.cpp`, `RuleSetTests.cpp`,
      `LinkInspectorTests.cpp`, `SymlinkServiceTests.cpp` all exist and run.
- [x] **All unit tests pass** — `vstest.console.exe` reports green. A successful build is
      not sufficient evidence. Reconfirmed for M9 (issues #66/#137/#138):
      `Debug|x64`/`Release|x64` both report 407/407 passing. Caveat: this build
      environment lacks Developer Mode/elevation, so privilege-gated tests — including
      `nonAsciiBrokenSymbolicLinkIsBroken`, which an earlier session on a
      privilege-enabled machine reported as a genuine failure — are skipped rather than
      exercised here, same as every other symlink-creation test in this suite. That
      earlier finding is tracked separately as issue #144 and was not re-verified on a
      privilege-enabled host during this M9 pass.
- [x] **No dependency has a known vulnerability**; every dependency is MIT-compatible and
      justified in the PR that introduced it - the project has **zero third-party
      dependencies** (no `vcpkg.json`; C++/WinRT comes from the Windows SDK), stated as
      such rather than as "scanned clean," consistent with every M8 entry that touched
      this point. A CI tripwire now enforces that stays true -
      `.github/workflows/dependency-audit.yml` fails the build if a dependency manifest
      or vendored tree appears without being recorded in
      `.github/dependency-inventory.json`, and if a GitHub Actions `uses:` is unpinned or
      un-allow-listed (`docs/adr.md` open item 6 resolved, `docs/adr-phase-9.md`
      ADR-0043, issues #22/#164/#165). No scanner was found that understands
      `vcpkg.json`, so the vcpkg-specific advisory check itself remains manual.
- [x] `0.1.0` published as an unsigned GitHub **pre-release** (issue #65,
      `docs/adr-phase-6.md` ADR-0033) - statically linked x64/ARM64 executables with
      `SHA256SUMS.txt`, release notes stating the build was local (no CI, #21 is open),
      ARM64 was cross-built and not executed, the dependency-vulnerability gate is
      manual (zero third-party dependencies), and the executable is unsigned. The
      pre-release designation is **not** about missing functionality - `--tui` (M7) is
      implemented; see ADR-0033 for the four reasons that are.
