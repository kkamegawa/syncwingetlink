# Architecture Decision Records — Post-M9 (scan/fix result presentation)

This file continues the chronological record in
[`adr-phase-8.md`](./adr-phase-8.md). M9 (documentation) closed issue #11; this ADR
belongs to a new, unnumbered milestone tracked as issue #145 (four stacked sub-issues:
#146-#149), since it does not fit any milestone recorded in `docs/TODO.md`.

---

## ADR-0042 — Startup Developer Mode gate, optional elevation prompt, and localized permission guidance

- **Date**: 2026-08-15
- **Affected**: `src/cli/ArgParser.*`, `src/cli/Console.*`, `src/cli/Dispatch.cpp`,
  `src/core/Model.h`, `src/core/SymlinkService.*`, `README.md`,
  `docs/troubleshooting.md`, issue #143 follow-up
- **Status**: Accepted

### Decision

1. **`fix` now performs a startup permission gate before repair begins, but only for a
   mutating run.** `scan`, `test-rule`, and `fix --dry-run` are unchanged. The gate calls
   the same `queryDeveloperMode()` / `queryElevation()` helpers that `SymlinkService`
   already uses after a permission-shaped Win32 failure, so startup guidance and
   post-failure guidance both read from the same registry/token source rather than
   duplicating logic with two potentially divergent implementations.
2. **If Developer Mode is disabled or unknown, the CLI prints an immediate warning before
   attempting repairs.** This is advisory, not a hard stop: the command may still succeed
   later when run elevated or when the host's policy allows symlink creation despite the
   preflight uncertainty.
3. **When the process is not already elevated and `--silent` is not set, the CLI asks
   whether to relaunch elevated.** Consent uses the existing `Console::confirm()` flow.
   If the user declines, an ordinary line-oriented `fix` continues in the current process,
   but `fix --tui` exits 2 without opening an editable checklist that cannot create links.
   If the relaunch succeeds, the original process exits successfully after handing off to
   the elevated instance. If the relaunch fails, the original process reports that failure
   and exits 2.
4. **`--silent` suppresses only the relaunch question, not the warning.** This keeps the
   command automation-safe: scripts still receive the diagnostic text on stderr, but the
   process never blocks waiting for input about privilege elevation. For `fix --tui`, the
   suppressed prompt is treated like a declined elevation and exits 2 before the TUI starts.
5. **Localized runtime text is introduced narrowly for this startup-permission path only.**
   On a Japanese UI OS (`GetUserDefaultUILanguage()` primary language `LANG_JAPANESE`),
   the startup warning and relaunch prompt are Japanese. Every other UI language falls
   back to English. This is an intentionally narrow exception to ADR-0031, not a general
   message-table or MUI system.

### Reason

- The user reported a host where Developer Mode is believed to be enabled but symlink
  creation still fails. The existing behavior only explains permission state after
  `CreateSymbolicLinkW` or delete-by-handle has already failed; it gives no early signal
  that the host's Developer Mode state looks suspicious before the batch starts.
- The new gate reuses `SymlinkService`'s own registry/token queries so the startup check
  and the operational error path cannot disagree about which state they observed.
- A full localization system remains out of scope, but the user explicitly requested
  Japanese messaging on Japanese Windows for this permission path. A narrow, startup-only
  exception meets that need without pretending the rest of the diagnostic surface is now
  localized.

### Consequences

- `AppOptions` gains `--silent`, documented as "print the startup permission warning but
  do not ask whether to restart elevated."
- `Console::confirm()` gains a suppress-prompt path so callers can opt out of the prompt
  without introducing a second confirmation API.
- ADR-0031 remains the general rule, but README/TODO/troubleshooting now record this
  startup-permission exception explicitly.

---

## ADR-0038 — Grouped NG/OK scan report, its column model, and the display-width approximation

- **Date**: 2026-08-08
- **Affected**: `src/cli/ScanReport.h`/`.cpp` (new), `src/core/Model.h`
  (`RepairItem::packageId`), `src/cli/Dispatch.cpp` (`runScan()`/`runFix()`), issue #145
- **Status**: Accepted

### Decision

1. **`scan`'s human-readable output is grouped into two tables, NG always first.** NG
   collects `LinkStatus::Missing`/`Broken`/`Mismatch` (anything a user must act on); OK
   collects `LinkStatus::Ok`. Before this issue, `scan` printed one `"{status}: {alias}
   -> {target}"` line per candidate in raw package-enumeration order (package id order,
   then per-package executable order) - actionable and routine lines were interleaved
   with no way to see all NG items at a glance. There was no ADR, `docs/PLAN.md` spec, or
   output-asserting test governing that format (`tests/DispatchTests.cpp` and
   `tests/IntegrationTests.cpp` both state explicitly that they never assert on printed
   text), so replacing it outright breaks no documented or tested contract.
2. **Each table has four columns: `package | status | alias | target`.** `package` is
   the owning `InstalledPackage::id`; `status` is the same `Ok`/`Missing`/`Broken`/
   `Mismatch` text the pre-existing per-item line used; `alias` and `target` are
   unchanged from before. `RepairItem` gains a `packageId` field for this - appended at
   the end of the struct (so existing aggregate-initialization call sites keep
   compiling), set by `cli::buildRepairCandidates()` after `inspectLink()` returns,
   never read or derived by `classifyLink()`/`inspectLink()` themselves.
   `classifyLink()`/`inspectLink()`'s signatures are unchanged - `packageId` is a
   display-only annotation `cli::` attaches to a `core::` value, the same relationship
   `AliasCollision` already has to `RepairItem` (reported separately, never merged into
   the classification itself, `docs/adr-phase-3.md`). An empty `packageId` (not expected
   in practice - `FsScanSource`/`WingetComSource` both always populate `id`) renders as
   `-` rather than an empty cell.
3. **Column widths for `package`/`status`/`alias` are computed once, across every row in
   both the NG and OK groups**, so the two tables always align at the same width even
   though they are printed as visually separate blocks. `target` is the last column and
   is never padded - a data row's rendered width can therefore be shorter than the
   table's separator line, but is designed to never exceed it. Paths are never truncated
   or elided; data fidelity was chosen over a fixed terminal width, since `target` is
   often the single piece of information a user needs to copy out of the report.
4. **Column measurement uses a terminal *display width* approximation
   (`cli::displayWidth()`), not UTF-16 code-unit count.** A surrogate pair counts as one
   code point; a code point in a commonly wide East Asian block (CJK ideographs, Hangul
   syllables, half/fullwidth forms, common emoji ranges, …) counts as 2 columns,
   everything else as 1. This is deliberately a best-effort approximation, not a
   wcwidth-equivalent implementation - combining marks, emoji ZWJ sequences, and
   variation selectors are not modeled. A miscount only misaligns a column; it never
   affects the data actually displayed, since every cell is still passed through
   `cli::sanitizeForDisplay()` (`docs/adr-phase-5.md` ADR-0021) before being measured or
   rendered, exactly as before this change.
5. **A group with zero items renders as just its heading followed by a bare `nothing`
   line, never an empty table.** `scan --source fs` with nothing to repair now reads `NG`
   / `nothing` rather than a table with a header and no rows, which would visually imply
   missing data rather than a clean result.
6. **`MessageImportance` (ADR-0030) is assigned per group, not per line, with one
   scan-specific exception.** For `scan` (`ReportMode::Scan`): the NG heading/table are
   `Normal` whenever at least one NG item exists (unchanged from the old per-item line's
   contract - actionable output is never suppressed under `--quiet`); a zero-NG `NG`/
   `nothing` pair drops to `Supplementary`, matching how the old code already treated a
   `LinkStatus::Ok` line. The OK heading/table are always `Supplementary`, in both cases.
   For `fix`'s preview (`ReportMode::FixPreview`, decision 7 below): every line is
   `Supplementary` regardless of NG/OK, since the preview is purely informational - `fix`
   still gates every actual repair on its own per-item consent prompt or `--yes`.
7. **`fix` (non-`--tui`, non-`--json`) prints the same grouped table as an up-front
   preview**, immediately after the alias-collision warnings and before the M7 TUI
   checklist / line-oriented confirmation flow. This is informational only - it changes
   nothing about *which* candidates end up needing consent, and does not appear when
   `--tui` is requested (the checklist already shows an equivalent view, and must not
   print into the alternate screen ahead of entering it) or when `--json` is set
   (`docs/adr-phase-5.md` ADR-0022's stdout-purity rule). `fix`'s own
   `[current/total] alias: result` progress lines (`docs/adr-phase-6.md` ADR-0028) and
   final summary (`printBatchSummary()`) are unchanged by this issue.
8. **`--json` output is unchanged**, for both `scan` and `fix`: schema, field set, and
   item ordering (still package-id enumeration order, not the grouped/sorted order the
   console report uses) are all untouched. Adding `packageId` to the JSON schema, or
   changing its item ordering, is a separate decision belonging to ADR-0022's schema
   contract and was deliberately left out of this issue's scope.
9. **Within a group, rows are sorted by alias (ordinal case-insensitive ascending), tied
   by executable path (same comparison).** The tie-break exists for alias-collision
   candidates - the console report renders every colliding item's row (the collision
   warning is a separate, pre-existing stderr line, `printCollisions()`, printed after
   the tables); a stable order is needed since collision items would otherwise compare
   equal on alias alone.

### Reason

- A long `scan --source fs` result (the motivating example: 20 items, 4 of them
  `Missing`, scattered through 16 `Ok` lines) makes the actionable subset easy to miss.
  Grouping NG first, with OK collapsed to `Supplementary` (already suppressed under
  `--quiet`, per ADR-0030), makes the actionable subset the first thing a reader sees
  and the only thing shown by default under `--quiet`.
- `RepairItem` already carries every other field a report needs (`alias`, `linkPath`,
  `status`, `executable.path`) except the package identifier; adding it as a pure
  display annotation, rather than threading it through `classifyLink()`, keeps M4's
  "pure and filesystem-independent" classification contract (`docs/adr-phase-3.md`
  ADR-0014) intact and untouched by this change.
- Computing column widths once across both groups (decision 3) was chosen over
  per-table independent widths so the NG and OK tables - printed one after another in
  the same output - visually line up; a reader scanning down the combined output sees
  one consistent table, not two independently-shaped ones.
- A best-effort `displayWidth()` (decision 4) was chosen over pulling in a full
  East-Asian-Width/wcwidth dependency: this project's standing policy is "standard
  library and Windows APIs first; a third-party dependency is a last resort"
  (`docs/adr.md` ADR-0005), and a misaligned column is a cosmetic
  imperfection, never a correctness or security issue, since `sanitizeForDisplay()`
  already owns display *safety* independently of this function.

### Verification

`Debug`/`Release` × `x64`/`ARM64` all build clean at `/W4 /WX` across all three stacked
layers (#146, #147, #148); `ARM64` is cross-built only, consistent with every prior
milestone's disclosure. `vstest.console.exe` reports 422/422 for `Debug|x64` (407
pre-existing + 15 new: 5 in `DisplayWidthTests`, 10 in `GroupedReportTests`,
`tests/ScanReportTests.cpp`).

Manual verification against a dummy Packages/Links tree (`--source fs`, no COM
dependency, matching `tests/IntegrationTests.cpp`'s own harness pattern), built
`Debug|x64`:

```
.\build\x64\Debug\syncwingetlink.exe scan --source fs --packages-dir <dir> --links-dir <dir>
→ NG table listing three Missing items, alias-sorted (copilot.exe, helm.exe, op.exe);
  OK section renders "nothing" (no Ok candidates in the fixture); exit code 0

.\build\x64\Debug\syncwingetlink.exe scan ... --quiet
→ Only the NG table is printed; no OK section at all

.\build\x64\Debug\syncwingetlink.exe scan ... --fail-on-missing
→ exit code 1 (unchanged)

.\build\x64\Debug\syncwingetlink.exe scan ... --json
→ byte-for-byte the pre-existing schema/shape: {"schemaVersion":1,"command":"scan",
  "repairItems":[...package-id-ordered...],"collisions":[]}

.\build\x64\Debug\syncwingetlink.exe fix ... --dry-run --yes
→ the same grouped NG/OK preview, followed by the unchanged
  "[current/total] alias: would create" progress lines and the unchanged
  "Summary: N selected, ..." block
```

Not verified: a live `--source com` run against a real winget install (out of scope -
this issue changes only the console formatting layer, downstream of package
enumeration); an environment with Developer Mode/elevation available, so no `Ok` sample
row was exercised end-to-end - `GroupedReportTests`' unit coverage exercises the `Ok`
path directly instead, since production symlink creation is unavailable on this build
host (the same limitation `docs/task.md`'s M9 entries and issue #144 already record for
this environment).

### Consequences

- `docs/PLAN.md` §8 gains the console-output spec this format previously lacked, and its
  `--verbose`/`--quiet` log-level table is updated to describe the report in terms of
  groups rather than per-item `Ok` lines.
- `README.md`'s `scan` usage example gains a sample of the grouped output, matching the
  precedent already set for `test-rule`'s three output shapes.
- A future change to add `packageId` (or reorder items) in the `--json` schema is a
  distinct ADR under ADR-0022's schema-versioning contract, not an extension of this one.
- A future change to add SGR color to the report is a distinct ADR: `Console::writeLine()`
  strips ESC via `sanitizeForDisplay()` today, so color would need a new `Console` API,
  and would still have to respect ADR-0026's "`--no-color`/`NO_COLOR` gate
  `colorEnabled()` only, never `vtEnabled()`" rule.

---

## ADR-0039 — `PackageIdentityRequired`: naming `APPMODEL_ERROR_NO_PACKAGE`, and the `RPC_S_SERVER_UNAVAILABLE` HRESULT-vs-Win32-code bug

- **Date**: 2026-08-08
- **Affected**: `src/core/PackageSourceError.h`/`.cpp`, `src/core/WingetComSource.cpp`
  (`Impl::Impl()`'s `PackageManager` activation catch), `src/cli/Dispatch.cpp`
  (`exitCodeFor(PackageSourceErrorKind)`), issue #143
- **Status**: Accepted

### Decision

1. **`--source auto`'s COM→filesystem fallback was already working correctly and is
   unchanged by this ADR.** Investigating issue #143 on the reporting machine confirmed
   `AutoPackageSource::enumeratePackages()` (`docs/adr-phase-2.md` ADR-0010) already
   catches the activation failure and degrades to `FsScanSource` with exit code 0; the
   default `--source` is already `auto` (`docs/adr-phase-2.md` ADR-0010, pinned by a new
   `ArgParserTests.cpp` test, `sourceDefaultsToAutoWhenOmitted`, since no prior test
   asserted the default). What issue #143 actually reported was a diagnostic-quality gap
   in the warning text printed on degrade, not a missing fallback. Making explicit
   `--source com` also degrade (reversing ADR-0010 decision 1) was considered and
   explicitly declined - the user confirmed only the diagnostic gap should be fixed, since
   `--source com`'s "the failure is the user's to see" contract still has value for anyone
   deliberately diagnosing a COM problem.
2. **`PackageSourceErrorKind` gains `PackageIdentityRequired`**, mapped from
   `HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)` (`0x80073D54`) in `mapHresultToKind`.
   This is the HRESULT `docs/adr-phase-8.md` ADR-0037 recorded `PackageManager` activation
   failing with on a machine where `winget list` itself worked normally - previously
   unclassified and falling through to `Unknown`, so the CLI's warning/error text gave no
   hint that the server was found and registered but refused typed WinRT interface
   activation from this unpackaged, out-of-process caller. It maps to
   `ExitCode::PackageEnumerationFailed` (`4`), same as every other kind
   (`docs/com-api.md` "Exit codes").
3. **`WingetComSource`'s `PackageManager` activation catch now names the cause when the
   kind is `PackageIdentityRequired`**, and includes the HRESULT in hex for every other
   activation failure too: `"The winget PackageManager COM server rejected typed
   activation from this unpackaged process (APPMODEL_ERROR_NO_PACKAGE, HRESULT
   0x80073d54)"` versus the prior always-generic `"Failed to activate the winget
   PackageManager COM server"`. Only the `PackageManager` activation call site
   (`Impl::Impl()`, the one ADR-0037 reproduced the failure against) was changed;
   `GetLocalPackageCatalog`, `Connect`, and `FindPackagesOptions`/`PackageMatchFilter`
   activation keep their existing per-site messages and still route through the same
   `mapHresultToKind`, so a `PackageIdentityRequired` classification is still possible
   from those sites, just without the specialized wording - out of scope for this issue.
4. **Fixed a pre-existing classification bug found while auditing `mapHresultToKind`**:
   the `RPC_S_SERVER_UNAVAILABLE` case compared against the raw Win32 error code (`1722`),
   but a COM failure surfaces it wrapped as `HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)`
   (`0x800706BA`) - so this case could never have matched a real failure; it only appeared
   to work because the pre-existing test fed the same raw constant back in. Fixed to match
   the wrapped form; a new regression test
   (`rawServerUnavailableWin32CodeDoesNotMatchAsAnHresult`) pins the raw constant falling
   through to `Unknown` so this cannot silently regress. `RPC_E_DISCONNECTED`/
   `RPC_E_SERVER_DIED` were already genuine HRESULTs and needed no change.

### Reason

- A degrade warning or `--source com` error that says only "Failed to activate the
  winget PackageManager COM server" gives a reader no way to distinguish "the server
  isn't installed" from "the server is installed and reachable, but this specific
  activation call is rejected for an unpackaged caller" - two failure modes with
  different, and possibly different-owner, remediation paths. Issue #143's own Test plan
  section asked for exactly this: a named `mapHresultToKind` case so the failure surfaces
  as something more specific than `Unknown`.
- Root-causing *why* `APPMODEL_ERROR_NO_PACKAGE` occurs on some hosts and not others
  (per-interface-version activation, a missing proxy/stub registration, or an inherent
  limitation of unpackaged out-of-proc activation for this interface) remains explicitly
  open, per issue #143's own open questions; this ADR only improves what the CLI reports
  once the failure has already happened; the four open questions in issue #143 do not need
  resolving for `--source auto` to keep working correctly for end users, since it already
  degrades regardless of which kind the failure classifies as.
- The `RPC_S_SERVER_UNAVAILABLE` fix was opportunistic, not requested by issue #143, but
  surfaced directly by reading every case in `mapHresultToKind` while adding the new one;
  leaving a known-dead case in an exhaustively-tested classifier would have been dishonest
  by omission.

### Verification

`Debug`/`Release` × `x64` both build clean at `/W4 /WX` with zero warnings.
`vstest.console.exe` reports 425/425 for both `Debug|x64` and `Release|x64` (407
pre-existing + 4 new in `PackageSourceErrorTests.cpp`
(`appmodelErrorNoPackageMapsToPackageIdentityRequired`,
`rawServerUnavailableWin32CodeDoesNotMatchAsAnHresult`, and the updated
`serverUnavailableHresultsMapToServerUnavailable`) + 1 in `DispatchTests.cpp`
(`PackageIdentityRequired` added to `everyKindMapsToPackageEnumerationFailed`) + 1 in
`ArgParserTests.cpp` (`sourceDefaultsToAutoWhenOmitted`); `ARM64` was not built for this
change (diagnostics-only, no platform-specific code path).

Manual verification on the reporting machine (the same host issue #143 was filed
against, which reproducibly hits `APPMODEL_ERROR_NO_PACKAGE`), `Debug|x64`:

```
.\build\x64\Debug\syncwingetlink.exe scan --verbose
→ warning: --source auto fell back to a filesystem scan: The winget PackageManager COM
  server rejected typed activation from this unpackaged process
  (APPMODEL_ERROR_NO_PACKAGE, HRESULT 0x80073d54)
  ...scan proceeds normally via the filesystem source; exit code 0

.\build\x64\Debug\syncwingetlink.exe scan --source com --verbose
→ The winget PackageManager COM server rejected typed activation from this unpackaged
  process (APPMODEL_ERROR_NO_PACKAGE, HRESULT 0x80073d54)
  exit code 4

.\build\x64\Debug\syncwingetlink.exe scan --source fs --verbose
→ unchanged: package source - requested: fs, used: fs; exit code 0
```

### Consequences

- `docs/com-api.md`'s "Failure and fallback" HRESULT table gains the
  `PackageIdentityRequired` row and the corrected `RPC_S_SERVER_UNAVAILABLE` wrapping, and
  its exit-code summary now says "eight" kinds rather than "seven".
- Issue #143 stays open, narrowed to its root-cause questions (which this ADR explicitly
  does not answer) rather than the diagnostic gap, which this ADR closes.
- A future ADR that changes explicit `--source com`'s no-degrade contract (ADR-0010
  decision 1) remains a distinct, separately-approved decision - this one does not touch
  it.

### Amendment (2026-08-08, issue #143)

Decision 3's "`FindPackagesOptions`/`PackageMatchFilter` activation keep their existing
per-site messages … out of scope for this issue" still holds - ADR-0040 changes the
*mechanism* those two activations use (a non-throwing `CoCreateInstance` call replacing
`winrt::create_instance`) but preserves their message text exactly. Decision 3's
`PackageManager` message is likewise preserved byte-for-byte by ADR-0040. Decisions 1, 2
and 4 are unaffected.

---

## ADR-0040 — Throw-free COM activation, and enforcing `WingetComSource`'s "no raw `hresult_error`" contract

- **Date**: 2026-08-08
- **Affected**: `src/core/WingetComSource.cpp`/`.h`, `src/core/PackageSourceFactory.cpp`/
  `.h`, `tests/PackageSourceFactoryTests.cpp`, issue #143
- **Status**: Accepted

### Decision

1. **The three COM activations (`PackageManager`, `FindPackagesOptions`,
   `PackageMatchFilter`) go through a file-local `createInstanceNoThrow<T>()` helper that
   calls `::CoCreateInstance` directly and returns the `HRESULT`, replacing
   `winrt::create_instance<T>()`.** `winrt::create_instance<T>(clsid, ctx)` is
   `CoCreateInstance(clsid, outer, ctx, guid_of<T>(), &result)` followed by
   `winrt::check_hresult()`, which throws on failure - so on a host where an activation
   reproducibly fails (this issue's own reporting machine, `PackageIdentityRequired` per
   ADR-0039), every single `scan`/`fix` run raised a first-chance C++ exception for a
   condition the code handles by design. The replacement performs the identical
   activation attempt - same CLSID, same `CLSCTX_LOCAL_SERVER`, same **typed default
   interface** IID via `winrt::guid_of<T>()` (not `IUnknown` - see decision 3 for why that
   distinction matters), and the same adopt-without-an-extra-`AddRef` ownership transfer
   `winrt::capture()` performs on success (`T{raw, winrt::take_ownership_from_abi}`) -
   without the throw. `winrt::try_create_instance<T>()` was considered and rejected: it is
   throw-free too, but discards the `HRESULT`, which `mapHresultToKind()` and the
   diagnostic text both need.
2. **The `PackageManager` activation's diagnostic message - including the
   `PackageIdentityRequired` special case and its `{:#010x}` HRESULT formatting added by
   ADR-0039 - is preserved byte-for-byte.** Verified by direct comparison against
   ADR-0039's own recorded transcript (see Verification below).
3. **`WingetComSource::Impl`'s constructor is split into a trivial `Impl() = default` plus
   an `initialize()` method that returns `std::optional<PackageSourceError>` instead of
   throwing.** `WingetComSource` itself gains a `static tryCreate(std::optional
   <PackageSourceError>& error)` factory and loses its public constructor (made private -
   the only way to obtain an instance is now `tryCreate()`, so no caller can hold one
   whose activation was never checked). `PackageSourceFactory.h` gains a
   `PackageSourceCreation{source, error}` struct, and `PackageSourceFactoryFn` now returns
   it instead of a bare `unique_ptr<IPackageSource>`; `AutoPackageSource::enumeratePackages()`
   branches on the returned struct for the construction step instead of catching a throw
   (it still uses an ordinary `try`/`catch` for the ensuing `enumeratePackages()` call,
   which is a ordinary runtime failure, not the "COM is unavailable" case the non-throwing
   path exists for). Explicit `--source com`/`--source fs` (`requireSource()` in
   `PackageSourceFactory.cpp`) still throw on failure - unchanged, per ADR-0010.
4. **Every remaining winrt call in `WingetComSource.cpp` is now inside a translating
   `catch`, closing a contract hole `WingetComSource.h` had documented since M2 without
   actually enforcing**: `ConnectResult::Status()`/`ExtendedErrorCode()`/`PackageCatalog()`,
   `FindPackagesResult::Status()`/`ExtendedErrorCode()`, and `Matches()` plus the
   range-for's own iteration machinery (C++/WinRT's iterator calls `Size()`/`GetAt()` as
   the loop advances, so the loop scaffolding, not only its body, could throw) were all
   previously unguarded. This mattered concretely: `winrt::hresult_error` has no
   `std::exception` base, so an escaping one would have defeated `AutoPackageSource`'s
   filesystem fallback outright (it catches only `PackageSourceError`) and would not have
   been caught by `Dispatch.cpp`'s or `main.cpp`'s `catch (const std::exception&)` either -
   only `main.cpp`'s bare `catch (...)`, printing `"unexpected error (not a
   std::exception)"` and exiting 3.
5. **Declined: making `IPackageSource` itself non-throwing** (e.g. `std::expected`-based).
   That would remove the last first-chance exception on a host where COM connects but
   `FindPackages` then fails, but it changes `IPackageSource`'s public contract,
   `AutoPackageSource`'s post-construction `try`/`catch`, and every `cli::run()` handler -
   a larger, separate decision. Recorded here as considered-and-rejected so this ADR is
   not read as claiming the program is now exception-free: it is not; the failure path
   this issue's reporting machine actually hits (COM construction failure, degrading to
   the filesystem source) now raises zero C++ exceptions, but `enumeratePackages()`
   failing after a successful connection still throws, as it always has.

### Reason

- A first-chance exception on every single run, for a condition the code already handles
  correctly and silently, is debugger noise that obscures genuine problems and makes this
  issue's reporting machine an unpleasant environment to debug anything else in. Removing
  it does not change behavior - `scan`/`fix` already completed successfully via the
  filesystem fallback before this change (verified in the ADR-0039 investigation) - it
  only removes the noise.
- The contract hole decision 4 closes was not hypothetical: it directly threatened the
  exact fallback issue #143 depends on. Any future edit that added a winrt call outside a
  guarded region would have silently broken `--source auto` on a COM-unavailable host with
  no compiler warning and no obviously-related test failure - the class's own header
  comment already claimed the contract that would have been violated.
- Making COM construction failure a return value rather than an exception was chosen over
  leaving it exception-based and merely narrowing the `winrt::hresult_error` exposure,
  because `--source auto` is *expected* to hit this failure routinely (it is the whole
  point of `auto`) - using C++ exceptions for an expected, routine control-flow outcome is
  the specific pattern this issue's first-chance-exception complaint identifies as
  unwanted, not merely the exception *type* involved.

### Verification

`Debug`/`Release` × `x64` both build clean at `/W4 /WX` with zero warnings; `ARM64`
(`Debug` and `Release`) cross-built clean at zero warnings too - unlike ADR-0039, this
change was not diagnostics-only (a new function template instantiates three times per
target), so ARM64 was not skipped this time. `vstest.console.exe` reports 426/426 for
both `Debug|x64` and `Release|x64` (425 pre-existing + 1 new,
`aFailingFactoryReportsViaReturnValueNotException` in `PackageSourceFactoryTests.cpp`;
the existing `AutoPackageSourceTests`/`CreatePackageSourceTests` cases needed only their
fake-factory helpers' return type updated to `PackageSourceCreation`, not new
assertions).

Message-text preservation (decision 2) was verified by direct comparison against
ADR-0039's own recorded transcript, on the same reporting machine, `Debug|x64`:

```
.\build\x64\Debug\syncwingetlink.exe scan --verbose
→ warning: --source auto fell back to a filesystem scan: The winget PackageManager COM
  server rejected typed activation from this unpackaged process
  (APPMODEL_ERROR_NO_PACKAGE, HRESULT 0x80073d54)
  ...exit code 0 - byte-for-byte identical to ADR-0039's transcript

.\build\x64\Debug\syncwingetlink.exe scan --source com --verbose
→ The winget PackageManager COM server rejected typed activation from this unpackaged
  process (APPMODEL_ERROR_NO_PACKAGE, HRESULT 0x80073d54)
  exit code 4 - byte-for-byte identical

.\build\x64\Debug\syncwingetlink.exe scan --source fs --verbose
→ unchanged, exit code 0
```

**The exception-elimination claim itself (decision 1/4) was verified empirically, not
just by code review**, using a small standalone harness (not part of the solution;
scratch-built and discarded) that links `syncwingetlink.core.lib` and installs a
`AddVectoredExceptionHandler` counting `RaiseException` calls with the MSVC C++-EH
exception code (`0xE06D7363`) - the same signal Visual Studio's "break on all C++
exceptions" debugger setting observes, but machine-checkable without an interactive
session. Run on the reporting machine around a call to `WingetComSource::tryCreate()`:
`tryCreate() succeeded: no`, `error.what()` matching the exact `PackageIdentityRequired`
text above, and **`C++ exceptions raised during tryCreate(): 0`**. This is direct,
on-the-actual-failing-host evidence that the activation failure this issue was filed
about no longer raises a C++ exception at all, not only that the exception (if any) would
be caught.

### What was not tested

No MSTest exercises any of the changed COM-activation or catalog-connect code paths -
consistent with `docs/adr-phase-2.md` ADR-0009, which records that out-of-process
activation for this CLSID is unreliable inside a sandboxed test runner. No fake-COM test
was added (a bogus-CLSID call to `createInstanceNoThrow`, an in-proc COM object registered
at test time, or an assertion on message strings, none of which either of the existing
`tests/DispatchTests.cpp`/`tests/IntegrationTests.cpp` do) - see `docs/task.md`'s entry
for this change for the specific alternatives considered and declined.

### Consequences

- `docs/com-api.md`'s "Activation" section, code sample, "Out-of-proc vs in-proc", "What
  happens when", and "Failure and fallback" sections are updated to describe
  `createInstanceNoThrow()`/`tryCreate()`/`PackageSourceCreation` rather than
  `winrt::create_instance`/a throwing constructor; ADR-0037's historical reproduction
  transcript (quoted in "Capabilities / permissions") is left as a historical record with
  a forward pointer to this ADR, not rewritten.
- `docs/PLAN.md` §3's "Implementation options" bullet is updated to describe the
  non-throwing call.
- `docs/adr-phase-8.md` ADR-0037's probe transcripts are historical evidence of a live run
  against the code as it was at the time and are deliberately left untouched.
- A future change to make `IPackageSource` itself non-throwing (decision 5) is a distinct,
  separately-approved ADR - this one does not attempt it.

---

## ADR-0041 — Remediation hints for package-source failures

- **Date**: 2026-08-15
- **Affected**: `src/core/PackageSourceError.cpp`/`.h`, `src/cli/Dispatch.cpp`,
  `docs/troubleshooting.md`, `docs/com-api.md`, `README.md`, issue #143
- **Status**: Accepted

### Decision

1. **Add a pure `remediationFor(PackageSourceErrorKind)` lookup in `PackageSourceError.*`.**
   It returns one actionable line per currently defined failure kind and is intentionally
   winrt-free, so MSTest can verify the text without depending on live COM activation.
2. **Runtime diagnostics point at a stable public GitHub URL, not a repository-relative
   path.** The release artifact is a single EXE, not a checkout containing
   `docs/troubleshooting.md`, so a relative path in stderr would frequently be unusable.
   Repository documents still use ordinary relative Markdown links.
3. **`cli::run()` prints `hint: <remediationFor(error.kind())>` only when package
   enumeration ultimately fails.** An explicit `--source com`/`--source fs` failure prints
   the hint. A successful `--source auto` degradation does not: the existing warning and
   verbose diagnostics stay byte-for-byte unchanged. If the filesystem fallback also fails,
   the final `PackageSourceError` reaches `cli::run()` and does print the hint.
4. **The reporting host's runtime verification proves the throw-free activation path, not
   the full `winrt::hresult_error` boundary backstop.** The observed host fails in
   `createInstanceNoThrow()` before later marshalled calls run, so the manual evidence for
   this issue is limited to user-visible fallback/remediation behavior and to zero-C++
   exception activation failure. This limitation is recorded explicitly in the ADR and PR
   rather than overstating what was proven by manual runs.

### Reason

- Issue #143's remaining UX problem was not misclassification; ADR-0039 already named
  `APPMODEL_ERROR_NO_PACKAGE`. The missing piece was actionable guidance that tells the
  user what to do next on a host where COM activation is unavailable.
- A troubleshooting page needs to be reachable from both the repository and the shipped
  executable. Those are different environments, so runtime and Markdown links cannot use
  the same form.
- Printing the hint on a successful `--source auto` degradation would turn an expected,
  self-healing path into repetitive noise. The warning already states that the tool
  degraded and why; the hint is reserved for the path that still ends in failure.

### Consequences

- `tests/PackageSourceErrorTests.cpp` gains coverage for the remediation text, including
  the stable troubleshooting URL and the `PackageIdentityRequired` recommendation to use
  `--source fs`.
- `README.md`/`README_ja.md` and `docs/com-api.md`/`docs/com-api_ja.md` link the new
  troubleshooting pages.
- The root cause of the activation rejection itself remains open and is not resolved by
  this ADR.

---

## ADR-0043 — Dependency-inventory tripwire and Actions pin check as the automated vulnerability gate

- **Date**: 2026-08-16
- **Affected**: `.github/dependency-inventory.json` (new), `tools/Test-DependencyInventory.ps1`
  (new), `tools/test-dependency-inventory.sh` (new), `.github/workflows/dependency-audit.yml`
  (new), `.github/skills/cpp-msbuild/SKILL.md` §5, `.github/dependabot.yml`, `AGENTS.md` §10,
  `docs/PLAN.md` §11, `CONTRIBUTING.md`, `docs/TODO.md` M0, issue #22, #164, #165
- **Status**: Accepted

### Decision

1. **Evaluated OSV-Scanner against `docs/adr.md` open item 6 and confirmed it does not
   support vcpkg.** Its documented supported artifacts and manifests list, checked
   2026-08-16 at <https://google.github.io/osv-scanner/supported-languages-and-lockfiles/>,
   covers only `conan.lock` for C/C++ package manifests (plus vendored/submoduled C/C++
   matched by vulnerable *commit ranges*, not a manifest). `vcpkg.json` is not an OSV
   ecosystem, and no GitHub Actions workflow extractor is documented either. No
   vcpkg-aware scanner of any kind was found. This is a factual finding, not a decision —
   it constrains everything below.
2. **The gate is built around what is actually true and checkable today: the dependency
   set is empty, and CI now enforces that it stays that way.**
   `.github/dependency-inventory.json` is the single tracked record of every dependency
   the project knowingly accepts (`nativeDependencies` — empty; `githubActions` — the
   allow-listed action repositories; `agentToolingDependencies` — the `apm`-managed
   `github/awesome-copilot` skill bundle; `acknowledgedPaths` — an explicit escape hatch).
   `tools/Test-DependencyInventory.ps1` (and its bash twin,
   `tools/test-dependency-inventory.sh`) fails CI the moment a tracked dependency
   manifest (`vcpkg.json`, `conan.lock`, `CMakeLists.txt`, `package.json`, `.gitmodules`,
   etc.), a vendored/third-party tree, a checked-in binary, or an MSBuild
   `<PackageReference>` appears that is not recorded there. This is deliberately a
   *presence* check, not a vulnerability database — it cannot tell whether a future
   vcpkg port has a known CVE. It can only guarantee that adding one is never silent.
3. **GitHub Actions — the one real dependency set this project has today — get a real
   pin-and-allow-list check**, not just Dependabot's routine SHA bumps. Every `uses:` in
   `.github/workflows/*.yml` must resolve to a full 40-character commit SHA, and its
   `owner/repo` must appear in the inventory's `githubActions` list. The inventory
   records **repositories, not SHAs**: Dependabot already keeps SHAs current, and
   recording them a second place would just create a value that goes stale. What the
   inventory exists to gate is a *new* third-party action being introduced at all — the
   decision that actually deserves review.
4. **OSV-Scanner forward coverage was attempted and reverted, not shipped.** The intent
   was to wire `google/osv-scanner-action`'s reusable `osv-scanner-reusable.yml@v2.5.0`
   workflow into `dependency-audit.yml` as forward coverage for whichever ecosystem a
   future dependency actually uses (with `upload-sarif: false`, since this repository is
   **private** and has no GitHub Advanced Security). In practice, calling it via `uses:`
   failed at GitHub's workflow-parse stage — `startup_failure`, **zero check-runs
   created**, before any job (including the unrelated `inventory-guard` job in the same
   file) could run. Two different fix attempts were tried: granting `actions: read` at
   the workflow level (the job's own `permissions:` block requested it, and a job cannot
   request more than the workflow-level ceiling for a reusable-workflow call), and
   separately removing the job-level `permissions:` override and the trailing comment on
   the `uses:` line entirely, in case either was mishandled by the parser. Neither
   fixed it. The SHA, the exact file path at that SHA, the repository's
   `allowed_actions: "all"` setting, and the account type (personal `User`, not an
   organization) were all verified valid via the GitHub REST API, so the cause is not
   diagnosable from a checkout — most likely an organization/enterprise Actions policy
   visible only in the repository's Settings UI. Rather than ship a required check that
   is permanently red (or block the working `inventory-guard` tripwire on an unrelated,
   undiagnosed failure), the job was removed. `dependency-audit.yml` ships with
   `inventory-guard` alone; OSV-Scanner forward coverage is deferred, not delivered.
5. **The vcpkg-specific advisory check remains manual.** No decision here changes the
   procedure in the `cpp-msbuild` skill §5 (pin `builtin-baseline`, check the exact port
   version against the GitHub Advisory Database before adding or rolling it). This ADR
   does not claim that procedure is now automated — only that the surrounding "is the
   dependency set what we think it is" question is.

### Reason

- ADR-0007 already predicted this exact regression: "Dependabot does not support vcpkg…
  the vulnerability gate weakens," and recorded evaluating a vcpkg-aware scanner as open
  item 6. Doing that evaluation honestly means accepting a negative result rather than
  reaching for the nearest scanner and calling the item closed.
- `.github/dependabot.yml`'s own header comment already warns against "adding one would
  be silently ineffective and would give a false sense of coverage" — wiring in
  OSV-Scanner as if it covered vcpkg would repeat exactly that mistake one layer up.
- The project's actual dependency risk today is not "an unpatched vcpkg port" (there are
  none) but "a dependency gets added without anyone noticing the Definition-of-Done line
  about vulnerabilities no longer holds." A presence tripwire directly closes that gap;
  a vcpkg vulnerability scanner that doesn't exist cannot.
- GitHub Actions are pinned to commit SHAs by convention already (see every existing
  `.github/workflows/*.yml`), but nothing previously *enforced* the convention or
  constrained which repos may appear at all — a genuine, if narrow, supply-chain gap
  distinct from the vcpkg question.

### Consequences

- `docs/adr.md` open item 6 is marked resolved, pointing here.
- `docs/TODO.md` M0's scanner-evaluation bullet is checked off, worded to the actual
  outcome ("evaluated; no vcpkg-aware scanner exists; a tripwire and Actions pin check
  were wired in instead") rather than "a scanner was wired in."
- Adding any future native dependency now requires a `.github/dependency-inventory.json`
  entry or CI fails; `CONTRIBUTING.md` and the `cpp-msbuild` skill's "before adding a
  dependency" checklist say so.
- **OSV-Scanner forward coverage remains an open follow-up**, not a shipped feature —
  see decision 4. Whoever picks it up next should start by checking the repository's
  Settings → Actions page (and, if applicable, any organization/enterprise Actions
  policy) for a reusable-workflow restriction, rather than re-attempting the same
  workflow-file variants this ADR already ruled out.
- The `inventory-guard` job is not yet in `main`'s required status checks; adding it is a
  branch-protection change only the repository owner can make (recorded as a manual
  follow-up in #164, not something an agent can do from a checkout).
