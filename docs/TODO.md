# syncwingetlink — TODO (implementation checklist for AI coding agents)

Tasks can be started top-down. `[core]` requires tests. One task = one small PR.

📖 日本語版は [`TODO_ja.md`](./TODO_ja.md) を参照してください。

## M0. Project bootstrap

Build system decisions are recorded in [`adr.md`](./adr.md) (ADR-0001 … ADR-0004).

- [x] `.editorconfig` / clang-format / clang-tidy setup
- [x] Record the build-system decisions in `docs/adr.md`
- [x] Create the MSBuild solution `syncwingetlink.sln`
      (VS2026 / platform toolset v145, C++20, `Debug|Release` × `x64|ARM64`)
- [x] Add the three projects per ADR-0002: `syncwingetlink.core` (static library),
      `syncwingetlink` (executable), `syncwingetlink.tests` (MSTest DLL)
- [x] Create `Directory.Build.props` + `props/syncwingetlink.common.props` and import them
      from every project (re-derive from ADR-0001 … ADR-0003; an earlier draft was
      discarded, see `task.md`)
- [x] Add app manifest: `longPathAware=true`, `requestedExecutionLevel=asInvoker`
- [x] Set up MSTest (Microsoft Unit Testing Framework for C++) with one smoke test that
      passes under `vstest.console.exe`
- [x] **Resolve ADR-0003**: verify whether the CppUnitTest framework library links against
      a `/MT` test DLL, then fix the `StaticRuntime` default and update the ADR
- [x] Use the **Windows SDK's** bundled C++/WinRT headers — no package is required
      (ADR-0007). Do not add `Microsoft.Windows.CppWinRT`
- [x] **Spike**: determine where to source the `Microsoft.Management.Deployment` winmd,
      then generate the projection with the SDK's `cppwinrt.exe` in a pre-build step
      (open item 1 in `adr.md`). Do not implement `WingetComSource` until this is settled
- [ ] Add `vcpkg.json` **only if** a native dependency actually becomes necessary
      (ADR-0007); pin `builtin-baseline` and match the triplet to `StaticRuntime`
- [ ] CI (GitHub Actions): `msbuild` + `vstest.console.exe`, x64 and ARM64.
      Confirm whether a `windows-11-arm` runner is available; if not, ARM64 is
      build-only and must be documented as such (open item 3 in `adr.md`)
- [x] Evaluate a vulnerability scanner that understands vcpkg — evaluated OSV-Scanner;
      it does not support `vcpkg.json` (no scanner does, as of this check). Rather than
      claim coverage that doesn't exist, wired in a CI tripwire
      (`.github/dependency-inventory.json` + `tools/Test-DependencyInventory.ps1`/`.sh`)
      that keeps the tracked dependency set exactly what the inventory says it is, plus
      an enforced pin-and-allow-list check on GitHub Actions (open item 6 in `adr.md`,
      resolved by `docs/adr-phase-9.md` ADR-0043, issues #22/#164/#165). The vcpkg
      advisory-review procedure itself remains manual; do not claim otherwise

## M1. Paths / model foundation
- [x] `core/Model.h`: define `InstalledPackage`, `PackageExe`, `LinkStatus{Ok,Missing,Broken,Mismatch}`, `RepairItem`, `AppOptions`
- [x] `core/Paths`: resolve Links path (and Packages path for FS fallback) via
      `SHGetKnownFolderPath(FOLDERID_LocalAppData)` (support `--links-dir`/`--packages-dir` override)
- [x] `\\?\` long-path normalization helper
- [x] `core/IPackageSource.h`: define the abstract interface for installed-package enumeration

## M2. Package enumeration (COM first + FS fallback)
> Activation details (CLSIDs, why `winrt::init_apartment()` is not used) are recorded in
> `docs/adr-phase-2.md` ADR-0009 and `docs/com-api.md`; they differ from the shorthand
> below for a documented reason.
- [x] `[core] core/WingetComSource`: C++/WinRT `winrt::init_apartment()` →
      create `PackageManager`
- [x] `GetLocalPackageCatalog(LocalPackageCatalog.InstalledPackages)` → `Connect()`
- [x] `FindPackages()` to enumerate `CatalogPackage`; from `InstalledVersion` get
      Id/Name/version/install location/(alias if available)
- [x] Filter to installer type = portable
- [x] Detect COM activation failure (App Installer missing / policy disabled / no permission) and handle exceptions
- [x] `[core] core/FsScanSource`: recursively scan Packages to collect `*.exe` (fallback)
- [x] Do not follow reparse points (symlink/junction) by default (loop prevention)
- [x] Implement `--source com|fs|auto` switch (auto: COM → FS degrade) —
      `core/PackageSourceFactory` (`AutoPackageSource` + `createPackageSource`). Only
      `PackageSourceError` degrades, and an empty COM result is not a failure; see
      `docs/adr-phase-2.md` ADR-0010
- [x] `--include`/`--exclude` glob filters — `core/PackageFilter`. Matching is
      per-executable against the package id or the exe file name, ordinal and
      case-insensitive, `*`/`?` only; exclude beats include (ADR-0010). Wiring to
      `AppOptions` happens in M6, which owns the parsed options
- [x] Unit tests: FsScanSource exe enumeration, and COM/FS switching logic —
      the switching tests inject fake `IPackageSource`s rather than activating real COM
      (ADR-0009 explains why no test constructs a live `WingetComSource`)

## M3. Alias resolution + regex rules (most important)
- [x] `[core] rules/RuleSet`: load/validate rules JSON (exit code 3 on invalid)
- [x] Embed default rules in the binary (rust target triple stripping, etc.)
- [x] `[core] core/AliasResolver`: decide alias by priority ((1) regex rules →
      (2) raw file name). The originally planned tier 1 ("COM metadata
      `PortableCommandAlias`") is permanently unreachable and is not implemented at all —
      `WingetComSource` can never populate a per-file alias because the COM API exposes no
      such field, and the now-removed `PackageExe::metadataAlias` field it would have used
      confirmed there was no writer anywhere in the codebase. See `docs/adr-phase-2.md`
      ADR-0009 and ADR-0012.
- [x] `test-rule` subcommand: show file name → matched rule name → alias - implemented
      by `runTestRule()` (`src/cli/Dispatch.cpp`), issue #56; confirmed already done and
      closed as such by issue #40 (M8's #64 review folds this stale checkbox in)
- [x] Unit tests: cover cases including `codex-x86_64-pc-windows-msvc.exe → codex.exe`.
      `tests/AliasPipelineTests.cpp` adds cross-component coverage: `RuleSetSelector` →
      `AliasResolver` end to end for representative real file names, and the regression
      test that a selected user `RuleSet` *replaces* `defaultRules()` rather than merging
      with it.
- [x] Implement and test rule priority (`--rules` > user settings > embedded) —
      `rules/RuleSetSelector::selectRuleSet()`. An absent user rules file falls through to
      embedded defaults; a present-but-malformed one is a propagated error (never a
      silent fallback), same as an explicit `--rules` file. Not yet wired to `AppOptions`
      — that is M6's CLI, which is the first code with a parsed `AppOptions` to hand.
- [x] `docs/rules.md`: document format, captures, replacement syntax, samples. Corrected
      to drop the never-implemented COM-metadata priority tier (ADR-0009/ADR-0012) and to
      document the absent-vs-malformed user-rules-file distinction (ADR-0013) and the
      "replaces, not merges" property of rule-source selection.

## M4. Link state judgment
- [x] `[core] core/LinkInspector`: judge the state of `Links\<alias>.exe` — the pure
      `classifyLink()` (#44) and the production `inspectLink()` adapter (#46)
- [x] Resolve symlink target via `GetFileAttributesW` + `FSCTL_GET_REPARSE_POINT` —
      `readSymbolicLinkTarget()`/`decodeSymbolicLinkTarget()` (#45), wired into
      `inspectLink()` (#46). The `REPARSE_DATA_BUFFER` layout is hand-rolled (DDK-only
      header) per `docs/adr-phase-3.md` ADR-0015
- [x] Treat copy placement (a normal file) or a different target as `Mismatch` —
      `LinkEntryKind::RegularFile`/`OtherReparsePoint` and
      `TargetRelation::DifferentFile` (#44, #46); `Broken` is limited to a symbolic link
      whose target does not currently exist (ADR-0014), correcting the conflicting
      `docs/PLAN.md` sentence
- [x] Detect and warn on alias collisions (multiple exes → same alias) —
      `detectAliasCollisions()` (#47), reported separately from `LinkStatus` so a
      colliding alias can never enter automatic repair (ADR-0017)
- [x] Unit tests: classification of Ok/Missing/Broken/Mismatch —
      `tests/LinkInspectorTests.cpp` covers the pure classifier, reparse decoding,
      file-identity comparison, collision detection, and a cross-component regression
      matrix (#48). A real symbolic link's `Ok`/`Broken`/`Mismatch`-different-file
      outcomes attempt real creation and log-and-skip where symlink privilege is
      unavailable (ADR-0016) rather than being omitted

## M5. Symlink creation service
> The service boundary, error model, and re-inspection/no-rollback rules are recorded in
> `docs/adr-phase-4.md` ADR-0018; the permission-query source and the exit-code boundary
> M5 does not own are in ADR-0019.
- [x] `core/SymlinkService`: `CreateSymbolicLinkW`
      + `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` (#49)
- [x] Delete broken links then recreate (#49, hardened with stale-candidate and
      real-filesystem regression coverage in #50)
- [x] Permission/Developer Mode detection and distinction - `SymlinkServiceError` carries
      the state; mapping to exit code 2 and rendering guidance text is M6's job (#51)
- [x] `--dry-run` outputs the plan with no side effects - proven with zero
      delete/create/permission-query callbacks invoked across every state, including two
      filesystem-backed checks (#52)

## M6. CLI
> The security and Win32 contract for this milestone, and the exit-code map (including
> a new code `4` for package-enumeration failure), are recorded in the Wiki page
> `plan/syncwingetlink/m6-command-line-interface` and `docs/adr-phase-5.md`.
- [x] `cli/ArgParser`: `scan`/`fix`/`test-rule` and each option, including
      `--fail-on-missing` and `--no-color` (#53). Path overrides
      (`--links-dir`/`--packages-dir`/`--rules`) are validated but not required to
      already exist (ADR-0020)
- [x] `cli/Console`: UTF-8/UTF-16 output, coloring (probed VT + `--no-color`/`NO_COLOR`),
      confirmation prompt (`--yes` support), and untrusted-string sanitization before
      display (#54, ADR-0021)
- [x] `--json` output (for scripting): `cli/Json` serializes `RepairItem`,
      `AliasCollision`, and `SymlinkRepairResult` into the schema documented in
      `docs/PLAN.md` §8 (#55, ADR-0022). Ensuring stdout carries only the JSON document
      when `--json` is set is `main.cpp`'s (#56) job, not this module's
- [x] `main.cpp`: mode dispatch and exit-code mapping. `cli/Dispatch::run()` implements
      `scan`/`fix`/`test-rule`, the total exit-code map, alias-collision exclusion from
      `fix`, and Ctrl+C handling (#56, ADR-0024). `syncwingetlink.exe` links
      successfully for the first time in this project's history
- [x] `--help`/`--version`: full usage/exit-code text and a single `cli::kVersion`
      source of truth (#57, ADR-0025)

## M7. TUI (`--tui`)
- [x] Enable Console Virtual Terminal Sequences (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`) -
      issue #58, ADR-0026 (`docs/adr-phase-6.md`)
- [x] Checklist UI for repair candidates (space to select, Enter to execute) - issue #59,
      ADR-0027 (`docs/adr-phase-6.md`)
- [x] Progress / result summary display - issue #60, ADR-0028 (`docs/adr-phase-6.md`)

## M8. Quality / polish
- [x] Integration test: with a dummy Packages/Links tree, scan→fix→re-scan becomes Ok -
      issue #61 (`tests/IntegrationTests.cpp`). Drives the real `cli::run()` dispatch
      path (`--source fs`, explicit `--packages-dir`/`--links-dir`/`--rules`, never the
      real `%LOCALAPPDATA%`) and asserts exit codes plus filesystem state - not output
      text, since `cli::run()` has no `Console`-injection seam. Hosts without symlink
      privilege log and skip per ADR-0016; `Debug|Release` × `x64|ARM64` all build
      clean, `vstest.console.exe` reports 394/394 for `Debug|x64`/`Release|x64`
- [x] Verify display/creation with non-ASCII paths - issue #62. Fixture combines
      Japanese katakana (no case distinction) with U+1F600 (non-BMP, also no case
      distinction, sidestepping any `CompareStringOrdinal`-vs-NTFS-`$UpCase` mismatch);
      written with `\uXXXX`/`\UXXXXXXXX` escapes only, no raw non-ASCII bytes in any
      test source. Covers `Paths::toExtendedLengthPath`/`fromExtendedLengthPath`
      round-tripping (both absolute and relative input), `ExecutableScanner`
      enumeration, `LinkInspector::inspectLink` (`Ok`/`Broken`/`Mismatch`), `Console`
      display fidelity via the `ConsoleOperations` seam, `escapeJsonString`'s
      surrogate-pair encoding (ADR-0022), and the full `scan`→`fix`→`scan` round trip
      via `cli::run()` (extends #61's harness). Hosts without symlink privilege log
      and skip per ADR-0016; `Debug|Release` × `x64|ARM64` all build clean,
      `vstest.console.exe` reports 405/405 for `Debug|x64`/`Release|x64`
- [x] Diagnostic localization policy: English by default for the first release, with a
      targeted exception for startup permission guidance on Japanese UI OSes - issue
      #63, ADR-0031 (`docs/adr-phase-6.md`) plus the startup-permission follow-up ADR in
      `docs/adr-phase-9.md`. Japanese is still not a general runtime message-table
      system; non-ASCII **data** (paths, file names) is unaffected and remains #62's
      scope
- [x] `VS_VERSION_INFO` resource and a single version source - issue #118, ADR-0032
      (`docs/adr-phase-6.md`). One new `Directory.Build.props` property
      (`ProductVersion`) is what `src/syncwingetlink.rc` (new, executable project
      only) and `src/cli/Version.h`'s generated `kVersion` both derive from;
      `app.manifest`'s version stays hand-maintained (ADR-0025) but a new
      `VerifyManifestVersionMatchesProductVersion` MSBuild target fails the build
      if it drifts (verified by deliberately drifting it and confirming the
      build fails, then reverting). `(Get-Item .\syncwingetlink.exe).VersionInfo`
      on the built `Release|x64` and cross-built `ARM64` executables shows
      populated `FileVersion`/`ProductVersion`/`FileDescription`/`LegalCopyright`;
      `Debug|Release` × `x64|ARM64` all build clean, `vstest.console.exe` reports
      405/405 for `Debug|x64`/`Release|x64`
- [x] README (install, usage, permission requirements, examples) - issue #64, commit
      `65008fc`. This box was left unchecked after #64 merged; corrected during M9
      (issue #138) bookkeeping.
- [x] Harden the release binary: `/guard:cf`, `/guard:ehcont`, `/CETCOMPAT`, `/Gy` -
      issue #106, ADR-0029 (`docs/adr-phase-6.md`). `/guard:ehcont`/`/CETCOMPAT` are
      x64-only per Microsoft's own reference; `Debug|Release` × `x64|ARM64` all build
      clean, `vstest.console.exe` is green for `Debug|x64`/`Release|x64`, and
      `dumpbin /headers /loadconfig` confirms CF Guard on both platforms and the EH
      Continuation table plus CET compatibility on `x64` only (including with
      `-p:StaticRuntime=true`)
- [x] Wire up `--verbose`/`--quiet` - issue #113, ADR-0030 (`docs/adr-phase-6.md`).
      `Console::MessageImportance` (`Supplementary`/`Normal`/`Diagnostic`) gates every
      line against `AppOptions::logLevel`; `--verbose` additionally reports effective
      paths, package source, and rule source on stderr; `--verbose --quiet` is
      last-wins in either order; `Debug|Release` × `x64|ARM64` all build clean,
      `vstest.console.exe` reports 393/393 for `Debug|x64`/`Release|x64`
- [x] Release: attach an unsigned single exe (static link) to GitHub Releases -
      issue #65, ADR-0033 (`docs/adr-phase-6.md`). `v0.1.0` published as a GitHub
      **pre-release** (unsigned, no CI, ARM64 cross-built-not-run, manual
      dependency gate - not a missing `--tui`, which is implemented);
      `syncwingetlink-0.1.0-x64.exe`/`-arm64.exe` built with
      `-p:StaticRuntime=true`, `SHA256SUMS.txt` attached and verified,
      `dumpbin` confirms #106's hardening and no dynamic CRT dependency, and
      `VersionInfo` confirms the tag/`kVersion`/`app.manifest` version agree
      (#118/ADR-0032)

## M9. Documentation (COM API)
> Tracked as three stacked sub-issues under #11 (#66, #137, #138); see the Wiki page
> `plan/syncwingetlink/m9-com-api-documentation` and `docs/adr-phase-8.md` ADR-0037.
- [x] `docs/com-api.md`: COM activation steps, required capabilities,
      out-of-proc/in-proc differences, fallback behavior on failure - issues #66, #137,
      #138, `docs/adr-phase-8.md` ADR-0037

## Scan/fix result presentation (separate from M0-M9, issue #145)
> Tracked as four stacked sub-issues (#146, #147, #148, #149); see `docs/adr-phase-9.md`
> ADR-0038.
- [x] Group `scan`'s console output (and `fix`'s pre-batch preview) into NG-first/OK
      tables with a `package | status | alias | target` column layout - issues #145-#149,
      `docs/adr-phase-9.md` ADR-0038. `RepairItem` gains a display-only `packageId`
      field; `classifyLink()`/`inspectLink()` signatures and `--json` output/ordering
      are unchanged; `Debug|Release` × `x64|ARM64` all build clean, `vstest.console.exe`
      reports 422/422 for `Debug|x64`

## COM activation fallback and remediation (issue #143)
- [x] Add a structural exception boundary in `WingetComSource` so no raw
      `winrt::hresult_error` escapes the two public entry points; translate only
      `winrt::hresult_error`, and let unrelated exceptions propagate unchanged
- [x] Add `remediationFor(PackageSourceErrorKind)` with actionable guidance and a stable
      public troubleshooting URL
- [x] Print a `hint:` line on terminal package-enumeration failure without changing the
      successful `--source auto` degrade warning
- [x] Add `docs/troubleshooting.md` and `docs/troubleshooting_ja.md`, and link them from
      `README*` and `docs/com-api*`
- [x] Record ADR-0041 and update `docs/task.md`
- [x] Verify `Debug|Release` × `x64`, cross-build `ARM64`, and run the relevant manual
      `--source com|auto|fs` checks on the reporting host

## Future enhancements (separate milestone)
- [ ] Read winget `PortableIndex` (sqlite) read-only (last resort when COM/FS are insufficient)
- [ ] machine-scope support (requires admin)
- [ ] Verify whether Links is on PATH, and assist registration
- [ ] Register a scheduled task for periodic auto-repair
