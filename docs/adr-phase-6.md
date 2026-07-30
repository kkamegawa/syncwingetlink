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
