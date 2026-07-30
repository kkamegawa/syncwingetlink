# Architecture Decision Records — M7 Phase

This file continues the chronological record in
[`adr-phase-5.md`](./adr-phase-5.md). Unlike M6, which recorded one ADR per issue in a
single running sequence, M7's three sub-issues (#58, #59, #60) each get their own ADR
number (ADR-0026, ADR-0027, ADR-0028) reserved up front by the pre-implementation
review that revised the Wiki plan `plan/syncwingetlink/m7-interactive-tui` and issues
#9/#58/#59/#60/#113 - avoiding three stacked pull requests editing the same ADR
section, which a single shared "ADR-0026 covers all of M7" plan would have caused.

---

## ADR-0026 — Terminal session ownership, the stdin input-mode contract, and close-event restoration

- **Date**: 2026-07-31
- **Affected**: `tui::TerminalSession` (new), `cli::Console`, `docs/TODO.md` M7, the
  Wiki page `plan/syncwingetlink/m7-interactive-tui`
- **Status**: Accepted

### Decision

1. **stdout's `ENABLE_VIRTUAL_TERMINAL_PROCESSING` mode stays exclusively owned by
   `cli::Console`**, unchanged from M6 (`Console`'s constructor enables it once;
   its destructor restores it once). `TerminalSession` never calls
   `GetConsoleMode`/`SetConsoleMode` on stdout. `Console` gains three read-only
   accessors - `vtEnabled()`, `stdinInteractive()`, `stdoutInteractive()` - so
   `TerminalSession` and the dispatch layer (#59) can query capability without
   re-deriving or re-mutating it. `Console` already computed VT capability internally
   (`tryEnableVirtualTerminal()`) and discarded it into a local variable; this issue
   keeps it in a member instead of adding a parallel capability type.

   A corollary already true before this issue, now covered by an explicit test
   (`TerminalCapabilityTests::vtEnabledReflectsCapabilityRegardlessOfNoColor`):
   `--no-color`/`NO_COLOR` gate `colorEnabled()` only. `Console`'s constructor calls
   `tryEnableVirtualTerminal()` unconditionally and ANDs `noColorRequested`/`NO_COLOR`
   into `m_colorEnabled` alone, so `vtEnabled()` is unaffected by either. This issue
   is exposing existing behavior, not changing it.

2. **`TerminalSession` (new, `src/tui/`) owns exactly three things**: stdin's console
   mode, alternate-screen state, and cursor visibility. Its `tryCreate()` takes the
   caller's already-computed `stdinInteractive`/`stdoutInteractive`/`vtEnabled` values
   (from `Console`) rather than re-probing Win32 itself, and returns `std::nullopt`,
   with nothing changed, if any of the three is false.

3. **Initialization order**: enter alternate screen (`ESC[?1049h`) → hide cursor
   (`ESC[?25l`) → change stdin's input mode. Teardown reverses this exactly: restore
   stdin's input mode → show cursor (`ESC[?25h`) → leave alternate screen
   (`ESC[?1049l`). A failure partway through initialization (only the stdin-mode step
   can fail; the two control-sequence writes are best-effort, matching
   `Console::write`'s existing fire-and-forget style) unwinds only the steps that
   already succeeded, in this reverse order - `TerminalSessionAcquisitionTests`
   covers both the read-mode and write-mode failure points.

   Rationale for putting the stdin-mode change last: the alternate-screen sequence
   only has effect once stdout VT is enabled (already true by construction, since
   `tryCreate` requires `vtEnabled`), so there is no ordering hazard putting it first;
   doing so also means the one step capable of failing (`SetConsoleMode` on stdin) is
   the last one applied, giving the widest possible successfully-unwound state if it
   does.

4. **Stdin input-mode contract while a session is active**:
   - set: `ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS`
   - clear: `ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
     ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT`

   Three points the original M7 plan did not state, found while implementing against
   the real Win32 contract rather than only the issue text:
   - `WINDOW_BUFFER_SIZE_EVENT` is only generated when `ENABLE_WINDOW_INPUT` is set.
   - `ENABLE_VIRTUAL_TERMINAL_INPUT` must stay **clear**: enabling it turns console
     input into a VT escape-sequence byte stream and suppresses
     `WINDOW_BUFFER_SIZE_EVENT` entirely, which would silently break resize detection
     if set for "consistency" with stdout's VT mode.
   - `ENABLE_EXTENDED_FLAGS` must be passed in the *same* `SetConsoleMode` call that
     clears `ENABLE_QUICK_EDIT_MODE`, or the quick-edit change is silently ignored by
     the OS - a mouse drag would otherwise still be able to select text and stall the
     renderer mid-frame.

5. **Ctrl+C is read as an ordinary key event, never delivered via
   `SetConsoleCtrlHandler`, while a `TerminalSession` is active.** Clearing
   `ENABLE_PROCESSED_INPUT` (point 4) is what causes this: with it clear, Ctrl+C
   arrives as a `KEY_EVENT_RECORD` (`wVirtualKeyCode == 'C'`, a Ctrl bit set in
   `dwControlKeyState`) through the same blocking `ReadConsoleInputW` loop used for
   every other key, surfaced by `TerminalSession::readEvent()` as an ordinary
   `TuiKeyEvent` with `ctrlPressed == true`. Had `ENABLE_PROCESSED_INPUT` been left
   set instead, Ctrl+C would fire a `SetConsoleCtrlHandler` callback on a separate
   OS-spawned thread while the main thread stayed blocked inside `ReadConsoleInputW` -
   a flag set by that callback (the pattern `cli::Dispatch`'s existing
   `g_ctrlCRequested` already uses, correctly, for the *non-interactive* repair loop,
   whose main thread is doing work between checks rather than blocked on input) would
   not be observed until another key was pressed. #59 is the consumer of this
   contract for the checklist's own Escape/Q/Ctrl+C cancellation; #60's
   `SetConsoleCtrlHandler`-based repair-loop interruption remains correct as-is and is
   registered only after a `TerminalSession`, if one was used, has already been torn
   down.

6. **`WINDOW_BUFFER_SIZE_EVENT.dwSize` is never used as the viewport size.** It
   reports the scroll-back screen-buffer size, not the visible window. The viewport
   is derived from `GetConsoleScreenBufferInfo().srWindow`
   (`Bottom - Top + 1` rows, `Right - Left + 1` columns) - both when a resize event is
   observed and whenever a caller queries the viewport directly.

7. **A `CTRL_CLOSE_EVENT`/`CTRL_LOGOFF_EVENT`/`CTRL_SHUTDOWN_EVENT` handler restores
   terminal state and always returns `FALSE`.** A C++ destructor does not run when the
   console window is closed via its own close control, at logoff, or at shutdown, so
   relying on `~TerminalSession()` alone would leave the terminal in the alternate
   screen with the cursor hidden after such an event. The handler is distinct from
   Ctrl+C handling (point 5) - it is registered through
   `TerminalOperations::setCloseHandler`, a single process-wide slot (the TUI is
   modal - at most one session is ever active), and returning `FALSE` means default OS
   handling still proceeds after restoration; the handler never decides whether the
   process exits, only what state it leaves behind.

   The restore logic itself is guarded by a `std::atomic<bool>` compare-exchange
   inside a `shared_ptr`-held state object (the same idiom `Console.cpp`'s
   `ProductionOutputModeState` already uses for its VT-restore closures), so it runs
   at most once even if the close-handler thread and the destroying thread race - the
   close-handler lambda captures only a `weak_ptr` to that state, never `this`, so a
   session that has already been destroyed and unregistered cannot be reached by a
   stale callback.

### Reason

- Splitting stdout-VT ownership from the new terminal session (point 1) is the direct
  fix for a defect the pre-implementation review found: an independent second
  enable/restore of stdout's VT mode inside `TerminalSession` would have captured
  "original mode" as whatever `Console` had already changed it to, corrupting
  restoration order on both the way in and the way out.
- The Ctrl+C-as-key decision (point 5) is not a style preference; it is required by
  how `ReadConsoleInputW` and `SetConsoleCtrlHandler` actually interact on a thread
  that is blocked reading console input. Getting this wrong would have made
  "Escape, Q, or Ctrl+C cancels" (the Wiki plan's own checklist contract) simply not
  work for Ctrl+C specifically, in a way that would not have been obvious without
  tracing the actual Win32 threading model.
- Points 4 and 6 close two more previously-unstated Win32 preconditions the checklist
  needs to function at all (resize detection, quick-edit interference); leaving them
  unstated risked an implementation that "worked" against a simple manual test but
  broke resize detection or mouse-drag interaction unpredictably depending on the
  starting console mode.
- Point 7 addresses that this project's existing Ctrl+C mechanism
  (`SetConsoleCtrlHandler` in `cli::Dispatch`) covers only Ctrl+C/Ctrl+Break, not
  window-close/logoff/shutdown - all of which can otherwise strand a real user's
  terminal in the alternate screen with a hidden cursor, a visible defect distinct
  from anything the repair pipeline's own safety contracts (M4/M5) are about.

### Consequences

- `src/tui/TerminalSession.{h,cpp}` (new) implement `TerminalSession`,
  `TerminalOperations`, `TuiKeyEvent`, `TuiResizeEvent`, and `TuiEvent` per the
  description above, added to `syncwingetlink.core.vcxproj`/`.filters` under a new
  `tui` filter.
- `cli/Console.{h,cpp}` gain `vtEnabled()`, `stdinInteractive()`, `stdoutInteractive()`
  and the three backing members that capture what the constructor already computes.
- `tests/TerminalSessionTests.cpp` covers: unavailability for each of the three
  missing-capability cases; successful acquisition's write/mode-change order; the
  installed close handler; partial-initialization failure at both the stdin-mode-read
  and stdin-mode-write steps, each unwinding in reverse order; destructor-driven
  restoration exactly once; an explicit `restore()` call followed by destruction
  (still exactly once); the close-handler callback performing the same restoration;
  move transferring ownership so only the moved-to instance's destructor restores
  anything; event delegation, including a Ctrl+C key event; and control-sequence
  writes bypassing sanitization (as intended - `TerminalOperations::writeControl` is
  for this class's own intentional sequences, not untrusted text).
- `tests/ConsoleTests.cpp` gains `TerminalCapabilityTests`, covering the three new
  accessors independently of `colorEnabled()`, including the `--no-color`
  cross-check.
- `docs/TODO.md` M7's first line is checked off, pointing at #58 and this ADR.
- The Wiki page `plan/syncwingetlink/m7-interactive-tui` and issues #9/#58/#59/#60/
  #113 were corrected in place by the pre-implementation review that also produced
  this ADR's number reservation (ADR-0026/0027/0028, not a single shared ADR-0026;
  #113 moved to ADR-0029) - see each issue's own revision note for what changed and
  why.
- #59 (repair checklist) is responsible for `ChecklistModel`, the thin renderer built
  on `TerminalSession`, the `--tui` command-line conflict checks, and routing a
  `tryCreate()` failure to the existing line-oriented CLI confirmation flow with zero
  TUI escape sequences emitted - none of that is implemented yet.

---

## ADR-0027 — Checklist selection model, `--tui`'s command-line conflicts, and the declined-vs-planned distinction

- **Date**: 2026-07-31
- **Affected**: `tui::ChecklistModel`, `tui::TuiApp` (new), `cli::ArgParser`,
  `cli::Dispatch`, `docs/TODO.md` M7, the Wiki page
  `plan/syncwingetlink/m7-interactive-tui`
- **Status**: Accepted

### Decision

1. **`tui::ChecklistModel` is a pure, Win32-free selection-state model** (cursor,
   per-index selection, scrolling viewport), wrapping `RepairItem` directly rather than
   introducing a parallel display-only type. `tui::TuiApp::runChecklist()` is the thin
   renderer/input loop built on top of it and a `tui::TerminalSession` (#58,
   ADR-0026): it translates key/resize events into model calls and redraws the full
   viewport after each one - a full redraw rather than an incremental diff, since this
   is a modal checklist with at most a few dozen visible rows, not a high-frequency
   renderer.

2. **Ctrl+C, Escape, and `q`/`Q` are all read as ordinary key events inside
   `runChecklist()`**, per ADR-0026's stdin input-mode contract (which clears
   `ENABLE_PROCESSED_INPUT` for exactly this reason) - none of the three goes through
   `SetConsoleCtrlHandler`. A read failure from the session (e.g. the input handle
   became invalid) is also treated as a cancellation rather than an infinite loop or a
   crash: no repair is authorized either way.

3. **`--tui` is rejected at parse time (exit code 3) when combined with `scan`,
   `test-rule`, `--json`, or `--yes`.** The checks live in `cli::ArgParser::
   parseArguments()`, immediately after the pre-existing `--json`-with-`fix`-without-
   `--yes` conflict check, so the `--help`/`--version` short-circuit that runs before
   any conflict validation remains untouched. `scan`/`test-rule` have no repair
   candidates to show a checklist for; `--json`/`--yes` both imply an unattended,
   scriptable invocation, the opposite of an interactive checklist. `--dry-run` and
   `--no-color` remain compatible with `--tui`.

4. **`cli::Dispatch::runFix()` treats the checklist's confirmed selection as consent
   for exactly the items selected, and as an explicit *decline* for every offered
   Missing/Broken item left unchecked** - never silently reusing `RepairMode::DryRun`
   for a declined item, which is the same-named bug ADR-0028 (#60) fixes for the
   pre-existing non-interactive confirmation prompt. A declined item is skipped
   entirely (no `repairLink()` call, no pre-action inspection) and reported to the
   console as `<alias>: declined` - distinct from a `--dry-run` "would create"/"would
   replace" line, and never written to a `--json` document (`--json` and `--tui`
   already conflict at parse time per point 3, so this never actually arises for the
   TUI path, but the console-only wording is chosen now so #60's shared executor does
   not have to retrofit a different label later). Every other candidate (`Ok`,
   `Mismatch`, and anything excluded by a collision) is processed exactly as it would
   be without `--tui` at all - none of those ever needed confirmation in the first
   place.

5. **A `--tui` request with nothing selectable, or with the terminal capability
   unavailable, falls back to the pre-existing line-oriented CLI confirmation flow
   with zero TUI escape sequences emitted**, per a warning on stderr - `cli::Dispatch`
   checks `Console::stdinInteractive()`/`stdoutInteractive()`/`vtEnabled()` (ADR-0026)
   before ever calling `tui::TerminalSession::tryCreate()`, and `tryCreate()` itself
   still returns `nullopt` defensively if any of the three turns out false regardless.

### Reason

- Point 2 is a direct consequence of ADR-0026's Ctrl+C decision - handling it any other
  way inside `runChecklist()` would silently stop working the moment the input mode
  contract from #58 was applied.
- Point 4 avoids introducing a second instance of the same declined/dry-run conflation
  ADR-0028 already has to fix for the pre-existing CLI prompt, rather than shipping it
  here and asking #60 to fix it twice.
- Point 5's fallback ordering (capability checked by `cli::Dispatch` *and* re-checked
  inside `tryCreate()`) is deliberate belt-and-suspenders: `Console`'s capability
  accessors are the single source of truth, but `tryCreate()` never assumes its caller
  checked correctly, matching how `SymlinkService::repairLink()` (M5) never trusts a
  candidate's already-recorded status either.

### Consequences

- `src/tui/ChecklistModel.{h,cpp}` (new) implement `ChecklistModel`/`ChecklistCandidate`.
- `src/tui/TuiApp.{h,cpp}` (new) implement `runChecklist()`, `ChecklistOutcome`, and
  `ChecklistRunResult`.
- `src/tui/TerminalSession.{h,cpp}` (#58) gain a `queryViewport()` accessor so the
  renderer can size itself before any resize event has occurred.
- `src/cli/ArgParser.{h,cpp}` gain the four `--tui` conflict checks described above.
- `src/cli/Dispatch.cpp` gains `runTuiChecklistIfRequested()` and the
  declined-vs-selected branch inside `runFix()`'s repair loop, described above.
- `tests/ChecklistModelTests.cpp` (new) covers initial state, navigation (including
  never wrapping past either end), selection/toggle/confirm/cancel, and viewport
  scrolling in both directions plus resize.
- `tests/TuiAppTests.cpp` (new) covers confirm with/without a selection, cancellation
  via Escape/`q`/Ctrl+C-as-key, navigation before toggling, a resize mid-session, a
  read failure treated as cancellation, and that rendered frames carry sanitized
  candidate text.
- `tests/ArgParserTests.cpp` gains the four `--tui` conflict cases and updates the
  pre-existing `tuiFlagSetsUseTui` case (a bare `--tui` now defaults to `scan` and
  would itself conflict, so the case now names `fix` explicitly).
- `docs/TODO.md` M7's second line is checked off, pointing at #59 and this ADR.
- #60 (progress and results) is responsible for extracting a shared repair-batch
  executor used by both the CLI and TUI paths, formalizing `declined` as one of its
  reported categories, and correcting `runFix()`'s exit-code precedence (interruption
  vs. insufficient permission) - none of that is implemented yet; this issue's
  `runFix()` changes are scoped to routing the checklist's selection into the existing
  loop, not restructuring it.

---

## ADR-0028 — Shared repair-batch executor, the `[current/total]` progress contract, and the corrected exit-code precedence

- **Date**: 2026-07-31
- **Affected**: `core::RepairBatch` (new), `cli::Dispatch`, `docs/PLAN.md` §11,
  `docs/TODO.md` M7, the Wiki page `plan/syncwingetlink/m7-interactive-tui`
- **Status**: Accepted

### Decision

1. **`core::runRepairBatch()` (new, `src/core/RepairBatch.{h,cpp}`) is the single
   implementation both `cli::Dispatch::runFix()`'s non-interactive path and its
   TUI-selection path drive**, replacing the two separate result-accounting/exit-code
   code paths #59 had left in place (the loop itself, and the earlier
   `runTuiChecklistIfRequested()`-only integration). It decides, per candidate: the
   `RepairDisposition` (`Created`/`ReplacedBroken`/`PlannedCreate`/
   `PlannedReplaceBroken`/`Declined`/`SkippedOk`/`RefusedMismatch`/`Failed`/
   `NotAttempted`), the aggregate `RepairBatchSummary` counts, and - via
   `exitCodeFor()` - the final exit code. Consent is pluggable
   (`RepairBatchOptions::consent` for the interactive CLI prompt,
   `preApprovedAliases` for the TUI's already-decided checklist selection,
   `assumeYes` for `--yes`), and `repairLink()` itself is injectable
   (`RepairBatchOptions::repairFunction`, defaulting to the real one) purely so this
   executor's own decision/counting logic is unit-testable without symlink privilege -
   the same seam-injection pattern `SymlinkServiceOperations`/`ConsoleOperations`/
   `TerminalOperations` already establish in this codebase.

2. **`Declined` is a first-class `RepairDisposition`**, not a reuse of
   `PlannedCreate`/`PlannedReplaceBroken`. A declined candidate never reaches
   `repairLink()` at all - #59 already introduced this for the TUI's own path (see
   ADR-0027); this ADR is what makes it also true for the pre-existing, non-interactive
   CLI "Repair X (currently Y)? [y/N]" prompt, which previously answered "no" by
   calling `repairLink(candidate, RepairMode::DryRun)` and reporting the resulting
   `WouldCreate`/`WouldReplaceBroken` outcome - indistinguishable, in a `--dry-run` run
   or in a summary, from an item the tool had genuinely planned to touch. Both
   `RepairDisposition::PlannedCreate`/`PlannedReplaceBroken` (produced only by an
   actual `RepairMode::DryRun` call to `repairLink()`) and `Declined` (produced only by
   a refused consent check, never by inspecting the candidate at all) now exist
   side-by-side, and the shared executor is the only place either is ever assigned.
   `Declined` is a console/summary category only, not a `--json` field - the `--json`
   document (`cli::toJsonFixResult`, ADR-0022) is built only from candidates that
   actually produced a `SymlinkRepairResult`, exactly as before, so its schema is
   unchanged; this holds vacuously anyway since `--json` and `--tui` already conflict
   at parse time (ADR-0027), and `--json`-without-`--tui` always implies `--yes`
   (`cli::ArgParser`), which bypasses consent (and therefore `Declined`) entirely.

3. **Persistent progress lines follow `[current/total] alias: result`**, replacing the
   pre-#60 CLI's bare `alias: result` line, for both the non-interactive and TUI `fix`
   paths - `RepairBatchOptions::onProgress` is called once per candidate immediately
   after its disposition is decided, carrying the 1-based current position and the
   total candidate count alongside the `RepairItemResult`. A `Failed` item is reported
   as its own `[current/total] error: alias - <guidance>` line on stderr (using the
   same `permissionGuidance()` text the pre-#60 code already produced, now driven by
   `RepairItemResult::error` - the caught `SymlinkServiceError`, preserved on the
   result since the executor itself has no `Console` to format guidance text with) -
   always emitted regardless of `--json`, matching the pre-#60 behavior for errors
   specifically (only the *non*-error per-item line was ever gated by `!jsonOutput`).

4. **The exit-code precedence is corrected, for both the CLI and TUI paths, not merely
   documented as unchanged.** `core::exitCodeFor(RepairBatchSummary)` is now the single
   place this decision is made:
   - Insufficient permission -> exit code 2, taking priority over an interruption or
     any other failure in the same batch.
   - Otherwise, an interruption with `summary.remaining > 0`, or any `summary.failed >
     0` -> exit code 10. An interruption observed only after every candidate had
     already been processed (`remaining == 0`) does not, by itself, count as a
     partial failure - there was nothing left it could have prevented. (This exact
     state cannot currently arise from `runRepairBatch()`'s own loop, which only ever
     sets `summary.interrupted` when it breaks out early - i.e., when at least one
     candidate remains; the branch is kept as an explicit, tested part of
     `exitCodeFor()`'s own contract regardless, both because the pre-#60 review
     specified it and as a guard against a future change to the loop's interruption
     check silently changing that invariant.)
   - Otherwise -> exit code 0 - full success, every candidate declined/skipped, an
     empty batch, and a complete (uninterrupted) dry run all included.

   This is a genuine behavior change to `fix`'s pre-#60 CLI path, not a
   clarification: the code this replaces checked `interrupted` *before*
   `anyInsufficientPermission`, so a permission failure followed by Ctrl+C returned
   10, not 2 - the original defect the pre-implementation review that produced this
   ADR's number reservation found (see #9/#58/#59/#60's revision notes).

### Reason

- Extracting one executor (point 1) is the only way to guarantee the CLI and TUI paths
  cannot independently drift on what a given repairLink() outcome means for the
  summary or the exit code - the exact risk two hand-maintained copies of the same
  decision logic would otherwise carry.
- Point 2 exists because shipping the CLI's declined-as-dry-run conflation
  unfixed while fixing it only for the new TUI path (#59) would have been a strange,
  hard-to-explain asymmetry between two paths a shared executor is supposed to unify.
- Point 4's correction, rather than a "no change" note, follows directly from actually
  reading `src/cli/Dispatch.cpp`'s pre-#60 `runFix()` against the documented contract
  before writing this ADR - the two did not match, and restating the mismatched
  contract without fixing the code would have left a real bug undocumented.

### Consequences

- `src/core/RepairBatch.{h,cpp}` (new) implement `RepairDisposition`,
  `RepairItemResult`, `RepairBatchSummary`, `RepairBatchResult`, `RepairBatchOptions`,
  `runRepairBatch()`, `RepairBatchExitCode`, and `exitCodeFor()`.
- `src/cli/Dispatch.cpp`: `runFix()`'s manual loop is replaced by one
  `runRepairBatch()` call; `outcomeDisplayName()` is replaced by
  `repairDispositionDisplayName()` (one additional case each for `Declined`, `Failed`,
  and the unreachable-in-practice `NotAttempted`); a new `toExitCode()` maps
  `RepairBatchExitCode` onto `cli::ExitCode` totally.
- `tests/RepairBatchTests.cpp` (new) covers: full success (created/replaced, and that
  `Ok`/`Mismatch` never call `consent()`); dry-run (planned outcomes, `consent()`
  never called); declined (a refused prompt, `preApprovedAliases` exclusion including
  ordinal case-insensitivity, and `assumeYes` bypassing consent entirely); failure
  (permission vs. non-permission, and permission's precedence over a same-batch
  interruption); interruption (before any item, and midway through, in both cases
  confirming the skipped candidate's `repairFunction` is never called); and
  `exitCodeFor()` exercised directly against hand-built summaries, including the
  `interrupted && remaining == 0` case `runRepairBatch()` itself cannot produce.
- `docs/TODO.md` M7's third line is checked off, pointing at #60 and this ADR - this
  completes M7 in `docs/TODO.md`.
- `docs/PLAN.md` §11's "`--tui` allows interactive checking and batch creation" item is
  checked off, pointing at #58/#59/#60 and ADR-0026 through ADR-0028 - the only M7-
  related Definition-of-Done line this session claims, per the Wiki plan's own
  instruction that #60 alone marks it.
