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
- [ ] Evaluate a vulnerability scanner that understands vcpkg (OSV-Scanner is a candidate,
      coverage unverified) and wire it into CI — Dependabot cannot do this
      (open item 6 in `adr.md`). Until then the gate is manual; do not claim otherwise

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
- [ ] `test-rule` subcommand: show file name → matched rule name → alias
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
- [ ] `core/SymlinkService`: `CreateSymbolicLinkW`
      + `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`
- [ ] Delete broken links then recreate
- [ ] Permission/Developer Mode detection and distinction (exit code 2 + guidance on failure)
- [ ] `--dry-run` outputs the plan with no side effects

## M6. CLI
- [ ] `cli/ArgParser`: `scan`/`fix`/`test-rule` and each option
- [ ] `cli/Console`: UTF-8/UTF-16 output, coloring, confirmation prompt (`--yes` support)
- [ ] `--json` output (for scripting)
- [ ] `main.cpp`: mode dispatch and exit-code mapping
- [ ] `--help`/`--version`

## M7. TUI (`--tui`)
- [ ] Enable Console Virtual Terminal Sequences (`ENABLE_VIRTUAL_TERMINAL_PROCESSING`)
- [ ] Checklist UI for repair candidates (space to select, Enter to execute)
- [ ] Progress / result summary display

## M8. Quality / polish
- [ ] Integration test: with a dummy Packages/Links tree, scan→fix→re-scan becomes Ok
- [ ] Verify display/creation with non-ASCII paths
- [ ] Decide the localization policy for error messages (English/Japanese)
- [ ] README (install, usage, permission requirements, examples)
- [ ] Release: attach an unsigned single exe (static link) to GitHub Releases
      (build with `-p:StaticRuntime=true`; depends on ADR-0003 being resolved)

## M9. Documentation (COM API)
- [ ] `docs/com-api.md`: COM activation steps, required capabilities,
      out-of-proc/in-proc differences, fallback behavior on failure

## Future enhancements (separate milestone)
- [ ] Read winget `PortableIndex` (sqlite) read-only (last resort when COM/FS are insufficient)
- [ ] machine-scope support (requires admin)
- [ ] Verify whether Links is on PATH, and assist registration
- [ ] Register a scheduled task for periodic auto-repair
