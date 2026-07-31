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

---

## ADR-0029 — Release-binary hardening flags, and why `/CETCOMPAT` alone is not enough

- **Date**: 2026-07-31
- **Affected**: `props/syncwingetlink.common.props`, `docs/TODO.md` M8, the Wiki page
  `plan/syncwingetlink/m8-quality-polish-and-release`
- **Status**: Accepted

### Decision

1. **`ControlFlowGuard=Guard` (`/guard:cf`) and `FunctionLevelLinking=true` (`/Gy`) apply
   to every configuration and platform** (`Debug`/`Release` × `x64`/`ARM64`) in
   `props/syncwingetlink.common.props`'s shared `ClCompile` `ItemDefinitionGroup`. Neither
   flag has a known compatibility issue for this codebase (pure Win32/C++/WinRT, no
   inline assembly, no third-party binaries), and both apply uniformly to
   `syncwingetlink.core`, `syncwingetlink`, and `syncwingetlink.tests` - the shared props
   did **not** need to be narrowed to the executable project alone (see point 3 below for
   the one flag that is scoped).

2. **`GuardEHContMetadata=true` (`/guard:ehcont`) and `CETCompat=true` (`/CETCOMPAT`) are
   both scoped to `'$(Platform)' == 'x64'` only**, never `ARM64`. Microsoft's own
   reference for each option documents this restriction independently: `/guard:ehcont`
   is supported for 64-bit processes on a 64-bit OS, and `/CETCOMPAT` is "currently only
   applicable to the x64 architecture." Applying either unconditionally would not be a
   silent no-op on `ARM64` - it risks build breakage or, worse, an image flag with no
   corresponding runtime behavior on that platform.

3. **`/CETCOMPAT` alone does not protect what CET shadow stacks exist to protect** on the
   SEH-unwind path, and shipping it without `/guard:ehcont` would have been misleading.
   `/CETCOMPAT` only marks the image as CET Shadow Stack-compatible; the actual defense
   against an attacker corrupting the instruction pointer inside a `CONTEXT` structure
   passed to `NtContinue`/`RtlRestoreContext`/`SetThreadContext` - the documented reason
   CET-aware binaries need this at all - comes from the EH Continuation (EHCONT) table
   `/guard:ehcont` generates. A binary with no EHCONT data is treated by `NtContinue` as
   legacy-compatible and allows *any* address inside it as a valid continuation target,
   which defeats the purpose of claiming CET compatibility in the first place. This issue
   (#106) originally proposed `/CETCOMPAT` alone; the pre-implementation review that
   produced this ADR corrected that before implementation, not after.

4. **`/guard:ehcont` requires `/Gy` (function-level linking / COMDATs) to link at all.**
   Enabling `GuardEHContMetadata` without `FunctionLevelLinking` on code that uses C++
   exceptions produces a hard linker failure (`LNK2046`/`LNK2047`: "module contains C++ EH
   or complex EH metadata but was not compiled with /guard:ehcont"). `/Gy` is therefore
   not optional polish alongside `/guard:ehcont` - it is a hard prerequisite, and is set
   project-wide (point 1) rather than only on `x64`, since it has no known downside on
   `ARM64` either.

5. **`ControlFlowGuard` cannot be combined with `/ZI` (Edit and Continue), and this
   codebase's Debug configuration used `/ZI` implicitly.** Enabling `/guard:cf` produced
   an immediate, verified build failure: `cl : command line error D8016: '/ZI' and
   '/guard:cf' command-line options are incompatible`. Neither project file nor the
   shared props previously set `DebugInformationFormat` explicitly, so MSBuild's own
   default for a `Debug` configuration (`EditAndContinue`, `/ZI`) was silently in effect.
   `DebugInformationFormat` is now pinned to `ProgramDatabase` (`/Zi`) for every
   configuration in the shared props, trading away in-IDE Edit and Continue for `Debug`
   builds. This project has no CI (`#21` is open) and does not otherwise depend on Edit
   and Continue; `/Zi`'s `.pdb` is what `vstest.console.exe` and `dumpbin` already
   consume. This was found by actually building with the new flags, not by reading
   documentation in isolation - the incompatibility is not mentioned on either flag's own
   MS Learn reference page.

6. **The shared `props/syncwingetlink.common.props` did not need to be narrowed to the
   executable project.** #106's own text anticipated a risk that enabling
   `/guard:ehcont`/`/Gy` project-wide might break `syncwingetlink.tests` (the MSTest DLL,
   which links the prebuilt CppUnitTestFramework static library) with `LNK2046`/
   `LNK2047`/`LNK4291`. In practice, all four configurations - including
   `syncwingetlink.tests` - built and linked cleanly with every flag applied uniformly;
   no fallback to executable-only scoping was necessary.

### Verification

Confirmed by an actual build and `dumpbin /headers /loadconfig` reading on this session's
machine (`ADR-0001`'s verified environment: Visual Studio 18 Enterprise, toolset `v145`,
MSVC 14.51.36231, Windows SDK 10.0.26100.0), not assumed from the flags' documentation
alone:

| Configuration | Guard CF instrumented | EH Continuation table present | CET compatible |
|---|---|---|---|
| `Debug\|x64` | yes | yes | yes |
| `Release\|x64` | yes | yes | yes |
| `Release\|x64` (`-p:StaticRuntime=true`) | yes | yes | yes, and `dumpbin /dependents` confirms no `MSVCP140.dll`/`VCRUNTIME140.dll` dependency |
| `Debug\|ARM64` (cross-built, not run) | yes | no (x64-only, as designed) | no (x64-only, as designed) |
| `Release\|ARM64` (cross-built, not run) | yes | no (x64-only, as designed) | no (x64-only, as designed) |

`Debug|x64` and `Release|x64` each ran their full `vstest.console.exe` suite (386 tests)
green after the change. `ARM64` was cross-built only, per `docs/adr.md` open item 3 - not
executed on this x64 host.

### Consequences

- `props/syncwingetlink.common.props` gains `ControlFlowGuard`, `FunctionLevelLinking`,
  `GuardEHContMetadata` (x64-only), `CETCompat` (x64-only), and an explicit
  `DebugInformationFormat=ProgramDatabase` for every configuration.
- Edit and Continue is no longer available for `Debug` builds in Visual Studio, in
  exchange for `/guard:cf` working at all. Not expected to matter given this project's
  build/test workflow (`msbuild` + `vstest.console.exe`, no CI yet).
- `docs/TODO.md` M8 gains a checked hardening line pointing at #106 and this ADR.
- #65 (the M8 pre-release) can cite this ADR and the table above as its hardening
  evidence, rather than re-deriving it.

## ADR-0030 — `--verbose`/`--quiet` wired up via `Console::MessageImportance`

- **Date**: 2026-07-31
- **Affected**: `src/cli/Console.h`/`.cpp`, `src/cli/Dispatch.cpp`, `docs/PLAN.md` §8,
  `docs/TODO.md` M8
- **Status**: Accepted

### Decision

1. **`Console` gains a `MessageImportance { Supplementary, Normal, Diagnostic }` enum**
   and holds the active `LogLevel` (`AppOptions::logLevel`) passed in at construction.
   `writeLine()` takes a third, defaulted (`Normal`) `importance` parameter and drops the
   line before it ever reaches `sanitizeForDisplay()`/`ConsoleOperations::write` if the
   active level does not clear the bar for that importance. Both constructors add
   `LogLevel logLevel = LogLevel::Normal` as a trailing defaulted parameter (production:
   right after `noColorRequested`; the deterministic test constructor: after the existing
   `noColorEnvValueOverride`), so every call site that predates this issue keeps compiling
   and behaving exactly as before.
2. **The three levels form a monotonic chain, each a strict superset of the one before -
   not "Quiet suppresses Supplementary, independently of Diagnostic."** `Quiet` shows
   `Normal`-importance lines only; `Normal` (the default) additionally shows
   `Supplementary`; `Verbose` additionally shows `Diagnostic` on top of that. The
   alternative reading of the original issue text - "Quiet suppresses `Supplementary`
   only, so `Diagnostic` still gets through" - was considered and rejected: it is
   unobservable in production either way, because `Diagnostic`-importance lines are only
   ever produced by the new verbose-reporting function below, which itself only runs when
   `logLevel == Verbose`. The monotonic chain is the more coherent contract for
   `Console`'s own unit tests (which exercise all nine `(LogLevel, MessageImportance)`
   combinations directly, without relying on `Dispatch` only calling `Diagnostic` from
   one branch) and matches the conventional quiet < normal < verbose ordering.
3. **Warnings/errors keep their existing call sites unchanged**, at the default `Normal`
   importance, on `ConsoleStream::Error`. `Normal` is shown at every level, so `--quiet`
   never suppresses them, and the `--json` document (also written at the default `Normal`
   importance via `writeJsonDocument()`) is likewise never suppressed at any level -
   `--json --quiet` still emits exactly the JSON document, unaffected by log level.
4. **`Supplementary` is applied only to genuinely skippable lines**: `scan`'s per-item
   line when `LinkStatus::Ok` (not `Missing`/`Broken`/`Mismatch`, which remain actionable
   and `Normal`), `fix`'s per-item `[current/total] alias: disposition` progress line, and
   all three lines of the final batch summary. Collision warnings and every error path are
   untouched (`Normal`, `ConsoleStream::Error`).
5. **`--verbose`'s additional reporting is a new `reportVerboseDiagnostics()` function**
   called once from `buildRepairCandidates()` (shared by `scan` and `fix`), gated on
   `options.logLevel == LogLevel::Verbose` purely as a work-avoidance optimization - the
   lines it writes are `Diagnostic` importance, which `Console` already drops at every
   other level regardless. It reports, all on `ConsoleStream::Error` (never stdout,
   regardless of `--json` - ADR-0022's stdout-purity rule is unaffected by log level):
   - the resolved effective `Links`/`Packages` directories
     (`paths::getLinksDirectory`/`getPackagesDirectory`);
   - the package source actually used, built from `options.source` plus a local flag the
     existing `onDegrade` callback now also sets - **not a downcast on
     `AutoPackageSource`**, which the caller cannot see through the returned
     `unique_ptr<IPackageSource>` (`src/core/PackageSourceFactory.h`);
   - which rule tier was selected (`--rules`, the user rules file, or the embedded
     defaults), determined by mirroring `RuleSetSelector.cpp`'s own explicit/user-file/
     defaults priority with a local `std::filesystem::exists()` check, rather than adding
     a new return value to `selectRuleSet()`. This is the same "build the report from
     information already available or cheaply re-derived, don't change the core
     interface" approach the package-source report above uses.
6. **`--verbose --quiet` (either order) is last-wins**, requiring no new code:
   `ArgParser::handleOption()` already assigns `options.logLevel` directly on each
   occurrence, so a later flag naturally overwrites an earlier one - the same behavior
   every other repeatable option (`--source`) already has. Tests cover both orders.
7. **`reportVerboseDiagnostics()`'s two Win32-backed lookups are individually
   try/catch-guarded, not left to propagate.** `paths::getPackagesDirectory()` (via
   `getLocalAppDataDirectory()`/`SHGetKnownFolderPath`) and, in the "no `--rules`"
   branch, `paths::getUserRulesFilePath()` (same underlying call) can both throw. Found
   during review: `--source com` never otherwise calls `getPackagesDirectory()` - only
   the filesystem source's own construction does, and `createPackageSource()` skips that
   entirely for `--source com` - so an unguarded call here would have added a failure
   mode that only exists because `--verbose` happened to be on, turning an
   otherwise-successful `--source com --verbose` run into a failure. Diagnostics must be
   best-effort; each lookup is now its own `try`/`catch (const std::exception&)`,
   reporting "could not be determined" on failure rather than aborting the whole report
   (or worse, the whole command). The rule-source lookup is guarded for the same reason
   even though the current call order in `buildRepairCandidates()` (which calls
   `selectRuleSet()`, itself calling the same function, before `reportVerboseDiagnostics()`)
   happens to make it unreachable today - relying on that ordering elsewhere in the file
   to keep this function safe was judged too fragile to leave as the only safeguard.

### Verification

- New `MessageImportanceTests` in `tests/ConsoleTests.cpp` exercise all nine
  `(LogLevel, MessageImportance)` combinations against `Console::writeLine()` directly,
  plus a regression test confirming the pre-#113 two-argument constructor call still
  defaults to `Normal`, and one confirming `Quiet` never drops an `Error`-stream `Normal`
  line.
- `ArgParserTests.cpp` gained `verboseThenQuietIsLastWins`/`quietThenVerboseIsLastWins`.
- `cli::Dispatch`'s own helpers (`buildRepairCandidates`, `reportVerboseDiagnostics`,
  `printScanItem`, `printBatchSummary`) remain file-local and are not unit-tested here,
  consistent with `DispatchTests.cpp`'s existing header comment explaining why `cli::run()`
  itself is verified by hand rather than mocked.
- `Debug|Release` × `x64|ARM64` all build clean at `/W4 /WX`; `vstest.console.exe` reports
  393/393 green for `Debug|x64` and `Release|x64`; `ARM64` is cross-built only, per
  `docs/adr.md` open item 3.

### Consequences

- `Console`'s public constructor signatures each gain one trailing defaulted parameter;
  no existing call site needed to change.
- `docs/PLAN.md` §8 documents the log-level table, the `Supplementary`/`Diagnostic`
  content it gates, and the last-wins rule.
- `docs/TODO.md` M8 gains a checked `--verbose`/`--quiet` line pointing at #113 and this
  ADR.

## ADR-0031 — Diagnostic messages are English-only for the first release

- **Date**: 2026-07-31
- **Affected**: `AGENTS.md` §5, `README.md`, `docs/TODO.md` M8
- **Status**: Accepted

### Decision

**Every runtime diagnostic (warnings, errors, `--help` text) is English-only for the
first release.** Japanese is served entirely by this repository's own documentation
(`README_ja.md`, `docs/*_ja.md`), never by a runtime message lookup. This is a decision
about message *language* only - it says nothing about non-ASCII **data**. Paths, package
names, and file names must still round-trip correctly through the whole pipeline
regardless of script; that is #62's scope, not this ADR's.

This issue's original wording ("record and apply the English and Japanese error-message
policy") read as though the tool ships Japanese diagnostics at runtime. It does not, and
never did:

- every diagnostic string in `src/` (`PackageSourceError`, `RuleSetError`,
  `SymlinkServiceError`, `ArgParseError`, and every `printHelp()`/`writeLine()` literal in
  `src/cli/`) is English, with no exception;
- the UTF-8 `std::string` these error types build, and `Console`'s `CP_UTF8` decoding of
  it (`docs/adr-phase-5.md` ADR-0021), is an **encoding** contract - it exists so
  non-ASCII *data* embedded in a message (e.g. a path) displays correctly - not a
  **localization** one;
- there is no message table, resource script, `.mui` file, or lookup layer anywhere in
  this codebase; and
- `AGENTS.md` §8/§9 already make English the canonical documentation language, with
  `*_ja.md` files documented as human-facing translations that are never a source of
  truth - extending that same rule to runtime diagnostics is consistent, not a new
  policy invented for this issue.

### Reasoning

- **The machine-readable contract for scripted callers is the exit-code table plus
  `--json`'s schema, never message text** (`docs/PLAN.md` §8). A script that branches on
  diagnostic wording would already be fragile regardless of language, so message
  language carries no compatibility weight.
- **No message-table/resource/MUI infrastructure exists**, and building one is out of
  `docs/PLAN.md`'s scope for this release. Adding partial localization now, without that
  infrastructure, would mean scattering translated string literals through `src/cli/` and
  `src/core/` by hand.
- **A half-translated diagnostic set reads worse than a consistent English one.** Every
  diagnostic a user might see - `ArgParseError`, `PackageSourceError`,
  `SymlinkServiceError`'s permission guidance, `--help` - either stays English together
  or the inconsistency itself becomes a bug report.

### What would have to change to revisit this

Not scattered translated literals inline. A real localization decision would need: a
message-table or resource (`.rc` string table, or a `.mui`/satellite-resource layout)
keyed by a stable message ID; a lookup layer `Console` or the error types call through
instead of formatting English text directly; and a decision on how a user selects or the
process detects the active language (a flag, the system locale, or both). None of that
exists today, and none of it is in scope for the first release.

### Consequences

- `AGENTS.md` §5 gains a one-sentence diagnostic-language rule citing this ADR.
- `README.md` gains a one-sentence note, next to the existing `README_ja.md` pointer,
  making the same distinction (documentation is bilingual; runtime diagnostics are not).
- `docs/TODO.md` M8's diagnostic-localization line is rewritten from "decide
  English/Japanese" to the resolved English-only decision, pointing at this ADR.
- No code changed. This issue is a documentation/decision record only.

## ADR-0032 — A single version property, a real VS_VERSION_INFO resource, and a build-time manifest check

- **Date**: 2026-08-01
- **Affected**: `Directory.Build.props`, `props/syncwingetlink.common.props`,
  `src/cli/Version.h`, `src/syncwingetlink.rc` (new), `src/syncwingetlink.vcxproj`/
  `.filters`, `docs/TODO.md` M8
- **Status**: Accepted

### Decision

1. **One new MSBuild property, `ProductVersion` (`Directory.Build.props`), is the
   single source of truth** - following the `StaticRuntime` precedent (a
   Condition-defaulted property, overridable via `-p:ProductVersion=...`). It holds
   the three-part dotted string (`"0.1.0"`), matching `cli::kVersion`'s existing
   format. Two derived properties, `ProductVersionMajor`/`Minor`/`Patch`
   (`$(ProductVersion.Split('.')[N])`), exist purely to feed the numeric
   `FILEVERSION`/`PRODUCTVERSION` fields a `.rc` file and a C++ preprocessor
   `#define` both need but a dotted string cannot directly supply.
2. **`props/syncwingetlink.common.props` defines `SYNCWINGETLINK_VER_MAJOR`/`MINOR`/
   `PATCH` project-wide** (in the shared `ClCompile` `PreprocessorDefinitions`, not
   only on the executable project) because `src/cli/Version.h` is compiled into
   `syncwingetlink.core`, the executable, and the test DLL alike.
3. **`src/cli/Version.h`'s `kVersion` is generated from those three macros** via a
   stringize-and-widen macro chain
   (`SYNCWINGETLINK_STRINGIZE`/`SYNCWINGETLINK_WIDEN`/`SYNCWINGETLINK_WSTRINGIZE`,
   `#undef`-ed at the end of the header so they don't leak into every translation
   unit that includes it), rather than a hand-written literal - this is the first of
   the two options this issue's acceptance criteria offered ("a preprocessor define
   driven by the same property"), chosen over cross-checking a still-hand-written
   literal because it removes the literal (and the drift it could have) entirely,
   not just detects it after the fact. `#ifndef` fallbacks (`0`/`1`/`0`) exist only
   for a tool that parses this header outside a full MSBuild invocation (a
   standalone clang-tidy run, IDE Intellisense) - an actual build always has the
   real macros defined by `props/syncwingetlink.common.props`.
4. **A new `src/syncwingetlink.rc`, in the executable project only** (matching how
   `app.manifest` is already scoped, not `syncwingetlink.core` or
   `syncwingetlink.tests`), defines a `1 VERSIONINFO` (`VS_VERSION_INFO`'s
   well-known resource ID) resource. `FILEVERSION`/`PRODUCTVERSION` use
   `SYNCWINGETLINK_VER_MAJOR,MINOR,PATCH,0` (the `.rc` grammar's own comma syntax
   substitutes the macros textually before parsing); the `FileVersion`/
   `ProductVersion` string values reuse the same stringize trick to build
   `"0.1.0"` from the three macros without a second literal. The `.rc` file
   deliberately does not `#include <windows.h>`/`<winresrc.h>` for the
   `FILEOS`/`FILETYPE`/`FILESUBTYPE` symbolic constants - their numeric values
   (`VOS_NT_WINDOWS32`, `VFT_APP`, `VFT2_UNKNOWN`) are stable, documented Win32 ABI
   constants, spelled out with a comment naming each, avoiding a resource-compiler
   include-path dependency for a resource this small. `syncwingetlink.vcxproj`'s
   `ResourceCompile` item definition sets the same three preprocessor definitions
   the `.rc` file consumes, sourced from the same `ProductVersionMajor`/`Minor`/
   `Patch` properties `kVersion` derives from.
5. **`src/app.manifest`'s `assemblyIdentity` version stays hand-maintained** - per
   ADR-0025, generating it would need a manifest-authoring transform step this
   project does not have, and this issue does not revisit that decision. Instead,
   `syncwingetlink.vcxproj` gains a `VerifyManifestVersionMatchesProductVersion`
   target (`BeforeTargets="Build"`) that reads `app.manifest`'s text, extracts the
   version attribute with a regex anchored to start searching only after the
   literal `assemblyIdentity` (so it can never match the unrelated
   `manifestVersion="1.0"` attribute on the root `<assembly>` element earlier in
   the same file - found and fixed during this issue's own verification, not
   assumed correct), and fails the **build itself** with `<Error>` if it does not
   equal `$(ProductVersion).0`. A build-time MSBuild target was chosen over an
   MSTest case (the issue's other offered option) because a test would need to
   locate `app.manifest` at runtime relative to the test binary's own directory -
   fragile relative-path reasoning a build-time check entirely avoids, and it
   fails earlier (before anything even attempts to link) rather than only at test
   time.

### Verification

- Confirmed the check actually fires: temporarily edited `app.manifest`'s version
  to `0.2.0.0` and rebuilt - the build failed with exactly the intended message,
  naming both the drifted and expected values. Reverted before committing.
- `(Get-Item .\syncwingetlink.exe).VersionInfo` on the built `Release|x64`
  executable shows `FileVersion`/`ProductVersion` `0.1.0` (`0.1.0.0` raw),
  `FileDescription`, `CompanyName`, and `LegalCopyright` all populated - the same
  data Explorer's Details tab reads.
- The same check was repeated on the cross-built `ARM64` executable (`VersionInfo`
  populated identically) - `VS_VERSION_INFO` is a plain resource, not
  architecture-specific code, so reading it needs no execution, unlike running the
  binary itself.
- `Debug|Release` × `x64|ARM64` all build clean at `/W4 /WX`; `vstest.console.exe`
  reports 405/405 for `Debug|x64`/`Release|x64` (unchanged from before this issue -
  no test behavior changed); `ARM64` is cross-built, not run, per `docs/adr.md`
  open item 3.

### Consequences

- `Directory.Build.props` gains `ProductVersion` and its three derived properties.
- `props/syncwingetlink.common.props` gains three project-wide preprocessor
  definitions.
- `src/cli/Version.h`'s `kVersion` is generated, not a literal; its value is
  unchanged (`"0.1.0"`), so `tests/DispatchTests.cpp`'s pre-existing
  `VersionTests` needed no change.
- `src/syncwingetlink.rc` (new) and its `ResourceCompile`/`.filters` registration,
  executable project only.
- `src/syncwingetlink.vcxproj` gains the `VerifyManifestVersionMatchesProductVersion`
  build-time gate. A version bump is now: change `ProductVersion` in
  `Directory.Build.props`, then update `app.manifest`'s version attribute to match
  - the second step is no longer purely a checklist item a human can silently skip;
  the build refuses to proceed if it's missed.
- `docs/TODO.md` M8 gains a checked version-resource line pointing at #118 and this
  ADR. #65 (the M8 pre-release) can now cite a real `VS_VERSION_INFO` resource as
  part of its release evidence, and its own tag-matches-`kVersion` checklist step
  has this ADR's manifest check as a partial mechanical backstop.
