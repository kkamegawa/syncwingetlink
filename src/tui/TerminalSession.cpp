// SPDX-License-Identifier: MIT

#include "TerminalSession.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

namespace syncwingetlink::tui
{
namespace
{
// The alternate-screen enter/leave and cursor show/hide VT sequences. Written verbatim
// via TerminalOperations::writeControl - never through cli::Console::writeLine(),
// which would sanitize them away (they are exactly the C0 ESC-prefixed sequences
// sanitizeForDisplay() is designed to strip from untrusted text).
constexpr std::wstring_view kEnterAlternateScreen = L"\x1b[?1049h";
constexpr std::wstring_view kLeaveAlternateScreen = L"\x1b[?1049l";
constexpr std::wstring_view kHideCursor = L"\x1b[?25l";
constexpr std::wstring_view kShowCursor = L"\x1b[?25h";

// Process-wide slot for the close/logoff/shutdown handler. At most one TerminalSession
// is active at a time (the TUI is modal), so a single slot is sufficient - mirroring
// the single g_ctrlCRequested flag cli::Dispatch already uses for the unrelated,
// non-interactive repair loop's own Ctrl+C handling. Guarded by a mutex because the
// registered handler runs on a separate OS-provided thread, concurrently with whichever
// thread installs/uninstalls it.
std::mutex g_closeHandlerMutex;
std::function<void()> g_activeCloseCallback;
bool g_closeHandlerInstalled = false;

BOOL WINAPI terminalCloseHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_LOGOFF_EVENT ||
        ctrlType == CTRL_SHUTDOWN_EVENT)
    {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(g_closeHandlerMutex);
            callback = g_activeCloseCallback;
        }
        if (callback)
        {
            callback();
        }
    }
    // Never suppress default handling (which would be TRUE) - this handler only
    // restores terminal state before the process goes away; it does not decide
    // whether the process goes away.
    return FALSE;
}

[[nodiscard]] HANDLE stdinHandle() noexcept
{
    return ::GetStdHandle(STD_INPUT_HANDLE);
}

[[nodiscard]] HANDLE stdoutHandle() noexcept
{
    return ::GetStdHandle(STD_OUTPUT_HANDLE);
}

[[nodiscard]] bool isUsableHandle(HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

void writeControlChunked(HANDLE handle, std::wstring_view text) noexcept
{
    if (!isUsableHandle(handle))
    {
        return;
    }
    constexpr std::size_t kMaxChunkChars = 4096;
    std::size_t offset = 0;
    while (offset < text.size())
    {
        const std::size_t chunkSize = std::min(kMaxChunkChars, text.size() - offset);
        DWORD written = 0;
        if (!::WriteConsoleW(handle, text.data() + offset, static_cast<DWORD>(chunkSize),
                             &written, nullptr) ||
            written == 0)
        {
            return;
        }
        offset += written;
    }
}

[[nodiscard]] std::optional<TuiResizeEvent> queryViewportFromConsole() noexcept
{
    const HANDLE handle = stdoutHandle();
    if (!isUsableHandle(handle))
    {
        return std::nullopt;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!::GetConsoleScreenBufferInfo(handle, &info))
    {
        return std::nullopt;
    }
    const SHORT rows = static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1);
    const SHORT columns = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
    if (rows <= 0 || columns <= 0)
    {
        return std::nullopt;
    }
    TuiResizeEvent event;
    event.rows = static_cast<std::uint16_t>(rows);
    event.columns = static_cast<std::uint16_t>(columns);
    return event;
}

[[nodiscard]] TerminalOperations makeProductionTerminalOperations()
{
    TerminalOperations operations;

    operations.getStdinMode = [](std::uint32_t& mode) {
        const HANDLE handle = stdinHandle();
        if (!isUsableHandle(handle))
        {
            return false;
        }
        DWORD raw = 0;
        if (!::GetConsoleMode(handle, &raw))
        {
            return false;
        }
        mode = static_cast<std::uint32_t>(raw);
        return true;
    };

    operations.setStdinMode = [](std::uint32_t mode) {
        const HANDLE handle = stdinHandle();
        if (!isUsableHandle(handle))
        {
            return false;
        }
        return ::SetConsoleMode(handle, static_cast<DWORD>(mode)) != FALSE;
    };

    operations.writeControl = [](std::wstring_view text) {
        writeControlChunked(stdoutHandle(), text);
    };

    operations.queryViewport = []() { return queryViewportFromConsole(); };

    // readEvent loops internally (never returning key-up, mouse, focus, or menu
    // records to the caller) so ChecklistModel/TuiApp (#59) only ever have to handle
    // the two variants TuiEvent actually names.
    operations.readEvent = []() -> std::optional<TuiEvent> {
        const HANDLE handle = stdinHandle();
        if (!isUsableHandle(handle))
        {
            return std::nullopt;
        }

        for (;;)
        {
            INPUT_RECORD record{};
            DWORD recordsRead = 0;
            if (!::ReadConsoleInputW(handle, &record, 1, &recordsRead) || recordsRead == 0)
            {
                return std::nullopt;
            }

            if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown)
            {
                TuiKeyEvent key;
                key.character = record.Event.KeyEvent.uChar.UnicodeChar;
                key.virtualKeyCode =
                    static_cast<std::uint16_t>(record.Event.KeyEvent.wVirtualKeyCode);
                key.ctrlPressed = (record.Event.KeyEvent.dwControlKeyState &
                                   (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                return TuiEvent{key};
            }

            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT)
            {
                // Deliberately ignore record.Event.WindowBufferSizeEvent.dwSize (the
                // scroll-back buffer size) and re-derive the visible viewport from
                // srWindow instead - see TuiResizeEvent's documentation.
                std::optional<TuiResizeEvent> viewport = queryViewportFromConsole();
                if (viewport.has_value())
                {
                    return TuiEvent{*viewport};
                }
                continue;
            }

            // Any other record type (key-up, mouse, focus, menu) is not meaningful to
            // the checklist and is silently skipped.
        }
    };

    operations.setCloseHandler = [](bool install, std::function<void()> onCloseSignal) {
        std::lock_guard<std::mutex> lock(g_closeHandlerMutex);
        if (install)
        {
            g_activeCloseCallback = std::move(onCloseSignal);
            if (!g_closeHandlerInstalled)
            {
                g_closeHandlerInstalled =
                    ::SetConsoleCtrlHandler(&terminalCloseHandler, TRUE) != FALSE;
            }
        }
        else
        {
            g_activeCloseCallback = nullptr;
            if (g_closeHandlerInstalled)
            {
                ::SetConsoleCtrlHandler(&terminalCloseHandler, FALSE);
                g_closeHandlerInstalled = false;
            }
        }
    };

    return operations;
}

// The stdin input-mode contract documented on TerminalSession: bits set/cleared while
// the session is active.
constexpr std::uint32_t kInputModeSetMask = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
constexpr std::uint32_t kInputModeClearMask =
    ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
    ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT;
} // namespace

// Holds every piece of mutable session state behind a shared_ptr (the same idiom
// cli::Console.cpp's ProductionOutputModeState uses for its VT-restore closures) so the
// close/logoff/shutdown callback can safely call restore() regardless of whether the
// owning TerminalSession has since been moved or destroyed - it captures only a
// weak_ptr to this struct, never `this`.
struct TerminalSession::State
{
    TerminalOperations operations;
    std::uint32_t originalStdinMode{0};
    bool stdinModeChanged{false};
    bool alternateScreenEntered{false};
    bool cursorHidden{false};
    // false once a real acquisition succeeds; restore() flips it to true exactly once,
    // from whichever thread gets there first (normal teardown or the close handler).
    std::atomic<bool> restored{true};

    void restore() noexcept
    {
        bool expected = false;
        if (!restored.compare_exchange_strong(expected, true))
        {
            return; // Already restored, or never successfully acquired.
        }

        if (stdinModeChanged && operations.setStdinMode)
        {
            operations.setStdinMode(originalStdinMode);
        }
        if (cursorHidden && operations.writeControl)
        {
            operations.writeControl(kShowCursor);
        }
        if (alternateScreenEntered && operations.writeControl)
        {
            operations.writeControl(kLeaveAlternateScreen);
        }
        if (operations.setCloseHandler)
        {
            operations.setCloseHandler(false, {});
        }
    }
};

TerminalSession::TerminalSession(std::shared_ptr<State> state) noexcept
    : m_state(std::move(state))
{
}

TerminalSession::~TerminalSession()
{
    if (m_state)
    {
        m_state->restore();
    }
}

std::optional<TerminalSession> TerminalSession::tryCreate(bool stdinInteractive,
                                                          bool stdoutInteractive,
                                                          bool vtEnabled)
{
    return tryCreate(stdinInteractive, stdoutInteractive, vtEnabled,
                     makeProductionTerminalOperations());
}

std::optional<TerminalSession> TerminalSession::tryCreate(bool stdinInteractive,
                                                          bool stdoutInteractive,
                                                          bool vtEnabled,
                                                          TerminalOperations operations)
{
    if (!stdinInteractive || !stdoutInteractive || !vtEnabled)
    {
        // Nothing has been touched yet - report unavailable so the caller (#59) falls
        // back to the existing line-oriented CLI confirmation flow with zero TUI
        // escape sequences emitted.
        return std::nullopt;
    }

    auto state = std::make_shared<State>();
    state->operations = std::move(operations);
    state->restored.store(false);

    // Initialization order: alternate screen -> hide cursor -> stdin input mode.
    if (state->operations.writeControl)
    {
        state->operations.writeControl(kEnterAlternateScreen);
    }
    state->alternateScreenEntered = true;

    if (state->operations.writeControl)
    {
        state->operations.writeControl(kHideCursor);
    }
    state->cursorHidden = true;

    std::uint32_t originalMode = 0;
    if (!state->operations.getStdinMode || !state->operations.getStdinMode(originalMode))
    {
        state->restore(); // Unwinds cursor + alternate screen, in reverse order.
        return std::nullopt;
    }
    state->originalStdinMode = originalMode;

    const std::uint32_t desiredMode =
        (originalMode & ~kInputModeClearMask) | kInputModeSetMask;
    if (desiredMode != originalMode)
    {
        if (!state->operations.setStdinMode || !state->operations.setStdinMode(desiredMode))
        {
            state->restore();
            return std::nullopt;
        }
        state->stdinModeChanged = true;
    }

    if (state->operations.setCloseHandler)
    {
        std::weak_ptr<State> weak = state;
        state->operations.setCloseHandler(true, [weak]() {
            if (std::shared_ptr<State> locked = weak.lock())
            {
                locked->restore();
            }
        });
    }

    return TerminalSession(std::move(state));
}

std::optional<TuiEvent> TerminalSession::readEvent()
{
    if (!m_state || !m_state->operations.readEvent)
    {
        return std::nullopt;
    }
    return m_state->operations.readEvent();
}

void TerminalSession::writeControl(std::wstring_view text)
{
    if (m_state && m_state->operations.writeControl)
    {
        m_state->operations.writeControl(text);
    }
}

void TerminalSession::restore() noexcept
{
    if (m_state)
    {
        m_state->restore();
    }
}
} // namespace syncwingetlink::tui
