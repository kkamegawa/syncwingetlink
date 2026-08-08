# Architecture Decision Records — Post-M9 (scan/fix result presentation)

This file continues the chronological record in
[`adr-phase-8.md`](./adr-phase-8.md). M9 (documentation) closed issue #11; this ADR
belongs to a new, unnumbered milestone tracked as issue #145 (four stacked sub-issues:
#146-#149), since it does not fit any milestone recorded in `docs/TODO.md`.

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
