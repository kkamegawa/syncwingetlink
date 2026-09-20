# Architecture Decision Records — Post-M9 (`--tui` reporting and path display)

This file continues the chronological record in
[`adr-phase-9.md`](./adr-phase-9.md). Both records here belong to unnumbered
follow-up work reported against a shipped build rather than to a milestone in
`docs/TODO.md`: issue #179 (`fix --tui` hides the one candidate that needs attention)
and issue #180 (real user-profile paths make console output hard to share).

---

## ADR-0047 — `Mismatch` candidates appear in the `--tui` checklist as non-selectable rows

- **Date**: 2026-09-20
- **Affected**: `tui::ChecklistModel`, `tui::TuiApp`, `cli::Dispatch`, `docs/PLAN.md`,
  `README.md`, `docs/TODO.md`, the Wiki pages
  `plan/syncwingetlink/m7-interactive-tui` and
  `plan/syncwingetlink/tui-mismatch-and-path-abbreviation`
- **Status**: Accepted
- **Extends**: ADR-0027 (`docs/adr-phase-6.md`) decisions 4 and 5
- **Does not change**: ADR-0014 (`docs/adr-phase-3.md`) and ADR-0016
  (`docs/adr-phase-4.md`) — a `Mismatch` is still never mutated

### Context

`fix --tui` was reported as "the TUI does not appear" on a host whose inventory was 20
`Ok` links and one `Mismatch` (issue #179). That was the specified behavior: ADR-0027
decision 4 routes `Ok` and `Mismatch` through the ordinary path, so
`runTuiChecklistIfRequested()` collected only `Missing`/`Broken`, found nothing, and
returned. Two further defects made it unreadable rather than merely surprising:

1. The empty-selection branch returned **silently**, although ADR-0027 decision 5 states
   that *both* the nothing-selectable case and the terminal-capability case warn on
   stderr. Only the second was ever implemented.
2. `runFix()` suppressed the grouped fix preview whenever `--tui` was *requested*, not
   when the checklist actually ran — so a fallback produced strictly less output than a
   plain `fix`, hiding the `Mismatch` a second time.

### Decision

1. **The checklist lists `Missing`, `Broken`, and `Mismatch`; `Mismatch` rows are not
   selectable.** `tui::ChecklistCandidate` gains a `bool selectable` field, and
   `cli::Dispatch` is the single place that maps a `LinkStatus` onto it:
   `Missing`/`Broken` → selectable, `Mismatch` → not selectable, `Ok` → not listed at
   all. `tui::ChecklistModel` never interprets a `LinkStatus`; it stays a pure selection
   state machine, which is what keeps every transition unit-testable without a console.

2. **`ChecklistModel` enforces non-selectability, rather than trusting the renderer to.**
   `isSelected()` reports false for a non-selectable index regardless of prior toggles,
   and `toggleCurrent()` is a no-op over one. `confirm()` therefore *cannot* return a
   `Mismatch`, so `cli::Dispatch`'s "a `Mismatch` is never consented to" contract holds
   however the model was driven. The cursor still moves onto non-selectable rows — their
   target paths are the reason the rows exist, and a checklist made entirely of them
   would otherwise trap the cursor on its first entry.

3. **`Ok` candidates stay out of the checklist.** There is nothing to decide about them,
   and a healthy inventory (20 of 21 entries, in the reported case) would bury the rows
   that need attention.

4. **A `Mismatch` is still never repaired.** No `--force` or `--replace-mismatch` option
   is introduced. The entry under `Links\` is a real file, a non-symlink reparse point,
   or a symlink to a different existing file; replacing it is a destructive act this tool
   does not perform on the user's behalf. The row is informational: `[-]` instead of a
   checkbox, and a trailing `[cannot repair]` marker.

5. **With nothing selectable, the key hints change.** `Space: toggle` and
   `Enter: repair selected` would both promise something no key press can deliver when
   every row is informational, so the header becomes
   `Up/Down: move  Enter: continue  Esc/Q/Ctrl+C: cancel`. `kReservedRows` stays 2: the
   marker is a row suffix, not a legend line, so no viewport arithmetic changes.

6. **The genuinely-empty case warns instead of returning silently**, implementing what
   ADR-0027 decision 5 already specified:
   `warning: --tui has nothing to show - no Missing, Broken, or Mismatch candidates;
   falling back to the line-oriented confirmation flow`.

7. **The fix preview is suppressed only when the checklist actually ran.** The preview
   block moves *after* `runTuiChecklistIfRequested()` and is gated on
   `TuiRunOutcome::NotRun`. The checklist restores the terminal before returning, so
   writing there lands on the normal screen, never the alternate one.

### Reason

- Point 1 keeps the status→role policy in exactly one place. Pushing it into
  `ChecklistModel` would have made the model interpret domain state it otherwise never
  touches, and would have duplicated the judgement `cli::Dispatch` already makes when it
  builds the candidate set.
- Point 2 is the same belt-and-suspenders reasoning ADR-0027 decision 5 applies to
  `TerminalSession::tryCreate()`, and that `SymlinkService::repairLink()` applies to a
  candidate's recorded status: the invariant is enforced where it is defined, not only
  where it is currently honored.
- Point 4 preserves ADR-0014's refusal rule. Showing a row is not the same as offering to
  act on it, so surfacing a `Mismatch` costs nothing in safety.
- Point 7 fixes an asymmetry, not a design: ADR-0027's intent was that the checklist
  *supersedes* the preview, which says nothing about a run where no checklist appeared.

### Consequences

- `src/tui/ChecklistModel.{h,cpp}`: `ChecklistCandidate::selectable`,
  `ChecklistModel::hasSelectable()`, and the `isSelected()`/`toggleCurrent()` guards.
  The defaulted field keeps existing `ChecklistCandidate{item}` aggregate initialization
  compiling.
- `src/tui/TuiApp.cpp`: three-state checkbox rendering, the `[cannot repair]` suffix, and
  the conditional key-hint line. `statusLabel()` already covered `Mismatch`.
- `src/cli/Dispatch.cpp`: `runTuiChecklistIfRequested()`'s collection loop becomes a total
  `switch` over `LinkStatus`; the empty case warns; `runFix()`'s preview moves after the
  checklist attempt and is gated on `NotRun`.
- `tests/ChecklistModelTests.cpp`: a new `ChecklistModelUnselectableCandidateTests` class
  covering `hasSelectable()`, the toggle no-op, `confirm()`'s exclusion, cursor movement
  over non-selectable rows, and unchanged viewport behavior in a mixed list.
- `tests/TuiAppTests.cpp`: a new `RunChecklistUnselectableCandidateTests` class covering
  the `[-]`/`[cannot repair]` rendering, the absence of those markers on a selectable row,
  both key-hint variants, Space being inert, and confirm/cancel over a `Mismatch`-only
  checklist.
- `docs/PLAN.md` §`--tui` and `README.md` §`--tui` both gain an explicit statement that
  `fix` never repairs a `Mismatch` — the behavior is easy to forget, and issue #179 is
  what it looks like when it is.
- `cli::Dispatch`'s TUI wiring remains outside unit-test reach (`tests/DispatchTests.cpp`
  documents why); it is covered by the manual scratch-tree checks recorded in
  `docs/task.md`.

---

## ADR-0048 — `--showspecialfolder`: abbreviating user folders in console *and* `--json` output

- **Date**: 2026-09-20
- **Affected**: `cli::PathDisplay` (new), `cli::ArgParser`, `cli::Json`,
  `cli::ScanReport`, `cli::Dispatch`, `tui::TuiApp`, `core::AppOptions`,
  `docs/PLAN.md`, `README.md`, `docs/TODO.md`, the Wiki page
  `plan/syncwingetlink/tui-mismatch-and-path-abbreviation`
- **Status**: Accepted

### Context

Every path this tool prints contains the real user-profile directory, so a screenshot or
a pasted log leaks the account name and has to be redacted by hand before it can be
shared in an issue (#180).

### Decision

1. **`--showspecialfolder`, short form `-s`.** When set, a path whose prefix is a known
   user folder is printed with that folder's environment-variable spelling.

2. **Exactly three folders: `%LOCALAPPDATA%`, `%APPDATA%`, `%USERPROFILE%`.** These are
   the folders whose real form carries the account name. `%PROGRAMFILES%`,
   `%PROGRAMDATA%` and `%SYSTEMROOT%` identify nobody, so abbreviating them would shorten
   output without serving the purpose the option exists for, and each extra entry is one
   more prefix every printed path is tested against.

3. **Longest match wins, and only on a component boundary.** `%LOCALAPPDATA%` is itself
   under `%USERPROFILE%`, so scan order alone would yield the less specific (and less
   informative) of the two. The character after a matched prefix must be a separator or
   the end of the string, so `C:\Users\bob` never rewrites `C:\Users\bobby\...`.
   Comparison is ordinal and case-insensitive, matching every other path comparison in
   this codebase.

4. **`--json` documents are abbreviated too.** Being able to paste a document verbatim
   was judged worth more than keeping every path in the document directly openable; a
   consumer that needs the real path expands the environment variable itself. The
   behavior is documented in `docs/PLAN.md` §8 alongside the schema. Without the flag the
   document is byte-for-byte what it always was.

5. **The setting is a parameter, never a global.** `PathDisplayOptions` is threaded
   through `formatGroupedReport()`, the `cli::Json` serializers, and
   `tui::runChecklist()` as a defaulted trailing argument. A process-wide flag would have
   been a smaller diff but would make these otherwise-pure functions order-dependent
   under the unit tests, and `cli::Dispatch` is already the single place that turns
   `AppOptions` into presentation decisions.

6. **Sorting is unaffected.** `formatGroupedReport()` still orders rows by the real
   executable path (`lessByAliasThenPath()`), so the flag can never reorder a report.
   Column widths *are* measured on the rendered text, so an abbreviated table still lines
   up.

7. **`abbreviateKnownFolders()` takes its folder table as a parameter**, and
   `knownFolderMappings()` is the separate, cached accessor that queries
   `SHGetKnownFolderPath`. That split is what makes the matching rules testable against a
   synthetic table on any host, rather than against wherever the build machine's profile
   happens to live.

8. **A folder the shell will not report is simply absent from the table.** Unlike
   `paths::getLocalAppDataDirectory()`, which cannot proceed without its answer, failing
   to abbreviate is cosmetic - so `queryKnownFolder()` returns an empty string instead of
   throwing, and the entry is dropped.

9. **No conflicts with any other option.** Unlike `--tui` (ADR-0027 decision 3), this one
   only changes how a path is rendered, which is meaningful for every command and for
   `--json` alike.

### Consequences

- `src/cli/PathDisplay.{h,cpp}` (new): `PathDisplayOptions`, `KnownFolderMapping`,
  `abbreviateKnownFolders()`, `knownFolderMappings()`, `formatPathForDisplay()`.
  Registered in both `syncwingetlink.core.vcxproj` and its `.filters`.
- `formatPathForDisplay()` becomes the single entry point for printing a path:
  abbreviate (when asked), then `sanitizeForDisplay()`. Sanitization stays last, so an
  abbreviated path can never smuggle a control sequence into a terminal or a document.
- `core::AppOptions::showSpecialFolders`; `cli::ArgParser` accepts
  `--showspecialfolder`/`-s`; `cli::Dispatch::pathDisplayOptionsFor()` is the one place
  the two are joined.
- Five call sites now print through it: the grouped report's target column, the `--tui`
  checklist row, the two `--verbose` directory diagnostics, and the
  "could not derive a valid alias" warning. `toJsonPathString()` covers every path in
  both JSON documents.
- `tests/PathDisplayTests.cpp` (new, registered in the test `.vcxproj` and `.filters`)
  covers the matching rules against a synthetic table, the sanitize-last ordering, and
  the invariants `knownFolderMappings()` must satisfy on any host.
- `tests/ArgParserTests.cpp`, `tests/JsonTests.cpp`, `tests/ScanReportTests.cpp` and
  `tests/TuiAppTests.cpp` each gain cases for the flag; the report case also asserts that
  row order is identical with and without it.
