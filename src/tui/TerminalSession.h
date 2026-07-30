// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>

namespace syncwingetlink::tui
{
// One key press, normalized from a Win32 KEY_EVENT_RECORD (key-down only; key-up
// records are not surfaced). Ctrl+C arrives here like any other key: TerminalSession's
// stdin input mode deliberately clears ENABLE_PROCESSED_INPUT, so Ctrl+C is delivered
// as an ordinary key record read by the same blocking ReadConsoleInputW loop as every
// other key, never as a separate CTRL_C_EVENT fired on its own thread via
// SetConsoleCtrlHandler. That thread split is the right mechanism for the
// non-interactive repair loop (#60), whose main thread is doing work between checks -
// it is the wrong mechanism here, where the main thread is blocked inside
// ReadConsoleInputW and would not observe a flag set by a handler running on another
// thread until the next key was pressed. Callers detect Ctrl+C by checking
// ctrlPressed together with virtualKeyCode == 'C'.
struct TuiKeyEvent
{
    wchar_t character{0}; // 0 if the key produced no printable character
    std::uint16_t virtualKeyCode{0};
    bool ctrlPressed{false};
};

// A console viewport size change. Deliberately derived from
// GetConsoleScreenBufferInfo().srWindow (visible window rows/columns), never from
// WINDOW_BUFFER_SIZE_EVENT.dwSize, which reports the scroll-back screen-buffer size,
// not the visible window - using dwSize directly would size the checklist's viewport
// wrong whenever the buffer and the window differ.
struct TuiResizeEvent
{
    std::uint16_t rows{0};
    std::uint16_t columns{0};
};

using TuiEvent = std::variant<TuiKeyEvent, TuiResizeEvent>;

// The Win32-facing operations TerminalSession drives, mirroring the seam pattern
// cli::ConsoleOperations and SymlinkServiceOperations already establish in this
// codebase: production code wraps the real APIs, tests supply deterministic callbacks.
struct TerminalOperations
{
    // Reads stdin's current console mode into mode; returns false on failure (stdin
    // not a usable handle, or GetConsoleMode failed).
    std::function<bool(std::uint32_t& mode)> getStdinMode;

    // Sets stdin's console mode; returns false on failure.
    std::function<bool(std::uint32_t mode)> setStdinMode;

    // Writes an already-built control/rendering sequence verbatim to stdout, in
    // chunks. Never sanitizes: the caller is responsible for having already run any
    // package-derived text through cli::sanitizeForDisplay() before combining it with
    // an intentional control sequence passed here.
    std::function<void(std::wstring_view text)> writeControl;

    // Blocking read of the next meaningful console input record (a key-down, or a
    // resize), normalized into a TuiEvent. Key-up records, mouse/focus/menu records,
    // and any other event type are silently skipped internally rather than surfaced.
    // Returns nullopt if the underlying read failed (e.g. the handle became invalid).
    std::function<std::optional<TuiEvent>()> readEvent;

    // Queries the current viewport size directly, from
    // GetConsoleScreenBufferInfo().srWindow. Used both by readEvent's internal resize
    // handling and by a caller that wants the viewport without waiting for a resize
    // event (e.g. right after the session starts).
    std::function<std::optional<TuiResizeEvent>()> queryViewport;

    // Installs (install=true) or uninstalls (install=false) the process-wide
    // close/logoff/shutdown restoration handler. When installed, onCloseSignal is
    // invoked if CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT, or CTRL_SHUTDOWN_EVENT fires -
    // these are delivered even when a C++ destructor would not otherwise run (the
    // console window closed via its own close button, a logoff, or a shutdown), unlike
    // Ctrl+C, which this class deliberately never handles via a console-control
    // handler (see TuiKeyEvent's documentation). At most one TerminalSession is active
    // at a time - the TUI is modal - so a single registration slot is sufficient.
    std::function<void(bool install, std::function<void()> onCloseSignal)> setCloseHandler;
};

// An RAII interactive-terminal session for the M7 checklist (docs/adr-phase-6.md
// ADR-0026). Owns exactly three things: stdin's console mode, the alternate-screen
// state, and cursor visibility - never stdout's ENABLE_VIRTUAL_TERMINAL_PROCESSING
// mode, which remains cli::Console's exclusive responsibility (Console enables it once
// at construction and restores it once at destruction; a second, independent
// enable/restore here would capture "original mode" as whatever Console had already
// changed it to, corrupting restoration order).
//
// Initialization order: enter alternate screen -> hide cursor -> change stdin's input
// mode. Teardown reverses this exactly: restore stdin's input mode -> show cursor ->
// leave alternate screen. A failure partway through initialization unwinds only the
// steps that already succeeded, in that reverse order, before tryCreate() reports the
// session as unavailable - it never leaves a partial state.
//
// Stdin input-mode contract while the session is active:
//   set:   ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS
//   clear: ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
//          ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT
// ENABLE_WINDOW_INPUT is required for WINDOW_BUFFER_SIZE_EVENT to be generated at all;
// ENABLE_VIRTUAL_TERMINAL_INPUT must stay clear because enabling it turns input into a
// VT escape-sequence stream and suppresses WINDOW_BUFFER_SIZE_EVENT entirely.
// ENABLE_EXTENDED_FLAGS must be passed in the same SetConsoleMode call that clears
// ENABLE_QUICK_EDIT_MODE, or the quick-edit change is silently ignored.
class TerminalSession
{
public:
    // Attempts to start a session using the real Win32 APIs. stdinInteractive,
    // stdoutInteractive, and vtEnabled are the caller's already-computed
    // cli::Console::stdinInteractive()/stdoutInteractive()/vtEnabled() - this class
    // does not re-derive them and does not touch stdout's console mode itself. Returns
    // nullopt, with nothing changed, if any of the three is false, or if acquiring the
    // session otherwise fails.
    [[nodiscard]] static std::optional<TerminalSession> tryCreate(bool stdinInteractive,
                                                                   bool stdoutInteractive,
                                                                   bool vtEnabled);

    // Deterministic constructor for tests: operations replaces every Win32 call.
    [[nodiscard]] static std::optional<TerminalSession> tryCreate(bool stdinInteractive,
                                                                   bool stdoutInteractive,
                                                                   bool vtEnabled,
                                                                   TerminalOperations operations);

    ~TerminalSession();

    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;
    TerminalSession(TerminalSession&&) noexcept = default;
    TerminalSession& operator=(TerminalSession&&) noexcept = default;

    // Blocking read of the next key or resize event through the session's operations.
    [[nodiscard]] std::optional<TuiEvent> readEvent();

    // Writes an intentional TUI control/rendering sequence verbatim to stdout. See
    // TerminalOperations::writeControl's documentation for the sanitization
    // requirement this places on the caller.
    void writeControl(std::wstring_view text);

    // Restores every change this session made (stdin mode, alternate screen, cursor
    // visibility), in the reverse of the order they were applied, and unregisters the
    // close/logoff/shutdown handler. Idempotent: safe to call more than once, from more
    // than one thread (the close handler may invoke this concurrently with normal
    // destruction), and after a move-from state; only the first call does anything.
    void restore() noexcept;

private:
    struct State;
    explicit TerminalSession(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};
} // namespace syncwingetlink::tui
