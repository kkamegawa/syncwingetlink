// SPDX-License-Identifier: MIT

#include "Console.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>

namespace syncwingetlink::cli
{
namespace
{
[[nodiscard]] bool isDisplayBlocked(wchar_t ch) noexcept
{
    if (ch <= 0x1F || ch == 0x7F)
    {
        return true; // C0 controls, including ESC, and DEL
    }
    if (ch >= 0x80 && ch <= 0x9F)
    {
        return true; // C1 controls
    }
    if ((ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2066 && ch <= 0x2069))
    {
        return true; // bidi override/isolate characters
    }
    return false;
}

[[nodiscard]] bool equalsOrdinalIgnoreCase(std::wstring_view left,
                                          std::wstring_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::wstring_view trimAsciiWhitespace(std::wstring_view text) noexcept
{
    const auto isAsciiSpace = [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    };

    std::size_t begin = 0;
    while (begin < text.size() && isAsciiSpace(text[begin]))
    {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && isAsciiSpace(text[end - 1]))
    {
        --end;
    }

    return text.substr(begin, end - begin);
}

// Part of the redirected-stdout/stderr encoding boundary (writeFileChunked() below), so
// a conversion failure must still produce something visible rather than silently
// writing an empty or partial buffer.
[[nodiscard]] std::string toUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                static_cast<int>(text.size()), nullptr, 0,
                                                nullptr, nullptr);
    if (required <= 0)
    {
        return "<unrepresentable>";
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int written =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                             result.data(), required, nullptr, nullptr);
    if (written <= 0)
    {
        return "<unrepresentable>";
    }

    return result;
}

// Lenient by design: input read from a redirected stdin is a confirmation-prompt
// response, not a security boundary the way an untrusted rules file is. An invalid byte
// sequence becomes U+FFFD rather than a hard failure - worst case, an unparseable
// response is treated as non-affirmative, which is the safe direction (see
// isAffirmative()).
[[nodiscard]] std::wstring utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int required =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                         required);
    return result;
}

[[nodiscard]] HANDLE handleForStream(ConsoleStream stream) noexcept
{
    switch (stream)
    {
    case ConsoleStream::Output:
        return ::GetStdHandle(STD_OUTPUT_HANDLE);
    case ConsoleStream::Error:
        return ::GetStdHandle(STD_ERROR_HANDLE);
    case ConsoleStream::Input:
        return ::GetStdHandle(STD_INPUT_HANDLE);
    }
    return INVALID_HANDLE_VALUE;
}

[[nodiscard]] bool isUsableHandle(HANDLE handle) noexcept
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

[[nodiscard]] bool isConsoleHandle(HANDLE handle) noexcept
{
    if (!isUsableHandle(handle))
    {
        return false;
    }
    DWORD mode = 0;
    return ::GetConsoleMode(handle, &mode) != FALSE;
}

// WriteConsoleW/WriteFile are chunked rather than called once with the whole buffer:
// WriteConsoleW in particular can fail with ERROR_NOT_ENOUGH_MEMORY on a very large
// single call. Chunk sizes are generous for an interactive CLI's actual output volume,
// not tuned for throughput.
constexpr std::size_t kMaxConsoleWriteChars = 8192;
constexpr std::size_t kMaxFileWriteBytes = 65536;

void writeConsoleChunked(HANDLE handle, std::wstring_view text) noexcept
{
    std::size_t offset = 0;
    while (offset < text.size())
    {
        const std::size_t chunkSize = std::min(kMaxConsoleWriteChars, text.size() - offset);
        DWORD written = 0;
        if (!::WriteConsoleW(handle, text.data() + offset, static_cast<DWORD>(chunkSize),
                             &written, nullptr) ||
            written == 0)
        {
            return; // Nothing more we can do about a write failure mid-stream here.
        }
        offset += written;
    }
}

void writeFileChunked(HANDLE handle, const std::string& bytes) noexcept
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const std::size_t chunkSize = std::min(kMaxFileWriteBytes, bytes.size() - offset);
        DWORD written = 0;
        if (!::WriteFile(handle, bytes.data() + offset, static_cast<DWORD>(chunkSize), &written,
                        nullptr) ||
            written == 0)
        {
            return;
        }
        offset += written;
    }
}

[[nodiscard]] std::optional<std::wstring> readLineFromConsole(HANDLE handle)
{
    DWORD originalMode = 0;
    const bool hadMode = ::GetConsoleMode(handle, &originalMode) != FALSE;
    bool modeChanged = false;

    if (hadMode)
    {
        const DWORD desiredMode =
            originalMode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
        if (desiredMode != originalMode && ::SetConsoleMode(handle, desiredMode))
        {
            modeChanged = true;
        }
    }

    std::wstring buffer(1024, L'\0');
    DWORD charsRead = 0;
    const BOOL ok =
        ::ReadConsoleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &charsRead,
                      nullptr);

    if (!ok || charsRead == 0)
    {
        if (modeChanged)
        {
            ::SetConsoleMode(handle, originalMode);
        }
        return std::nullopt;
    }

    // ReadConsoleW with ENABLE_LINE_INPUT normally returns once a full line (ending in
    // "\r\n") has been entered. If the buffer filled completely without its last
    // character being the newline, the typed line is longer than the buffer, and the
    // remainder - including the newline that would otherwise end it - is still
    // sitting in the console's own input queue. Left alone, that leftover would be
    // consumed by the *next* confirmation prompt instead of fresh user input,
    // potentially changing its answer without the user having typed anything for it.
    const bool truncated =
        charsRead == static_cast<DWORD>(buffer.size()) && buffer[charsRead - 1] != L'\n';

    if (truncated)
    {
        std::wstring drainBuffer(1024, L'\0');
        for (;;)
        {
            DWORD drainCharsRead = 0;
            const BOOL drainOk =
                ::ReadConsoleW(handle, drainBuffer.data(),
                               static_cast<DWORD>(drainBuffer.size()), &drainCharsRead,
                               nullptr);
            if (!drainOk || drainCharsRead == 0)
            {
                break; // Nothing more to drain - EOF/error on the input handle.
            }
            if (drainBuffer[drainCharsRead - 1] == L'\n')
            {
                break; // Reached the newline that ends the overlong line.
            }
        }
    }

    if (modeChanged)
    {
        ::SetConsoleMode(handle, originalMode);
    }

    if (truncated)
    {
        // A line this long can never be a real "y"/"yes" answer - isAffirmative()
        // would refuse it anyway - so refusal is reported directly rather than a
        // truncated, misleading fragment. The queue has already been drained above,
        // so a later prompt starts cleanly at the next line rather than consuming
        // this one's leftover tail.
        return std::wstring();
    }

    buffer.resize(charsRead);
    while (!buffer.empty() && (buffer.back() == L'\n' || buffer.back() == L'\r'))
    {
        buffer.pop_back();
    }
    return buffer;
}

// A confirmation-prompt answer is expected to be a handful of characters ("y", "yes",
// "no"); nothing legitimate is anywhere near this long. Capping what is retained keeps
// a hostile or merely misbehaving redirected stdin (an arbitrarily long or
// never-terminated line) from growing this buffer without bound.
constexpr std::size_t kMaxRedirectedLineBytes = 4096;

[[nodiscard]] std::optional<std::wstring> readLineFromRedirectedStream(HANDLE handle)
{
    std::string bytes;
    bool anyByteRead = false;
    bool truncated = false;

    char byte = 0;
    DWORD bytesRead = 0;
    while (::ReadFile(handle, &byte, 1, &bytesRead, nullptr) && bytesRead == 1)
    {
        anyByteRead = true;
        if (byte == '\n')
        {
            break;
        }
        if (bytes.size() < kMaxRedirectedLineBytes)
        {
            bytes.push_back(byte);
        }
        else
        {
            // Keep draining to the newline (or EOF) so a subsequent read starts at the
            // next line, but stop retaining bytes - the cap is on memory growth, not on
            // how much of the offending line gets consumed.
            truncated = true;
        }
    }

    if (!anyByteRead)
    {
        return std::nullopt; // Immediate EOF/closed pipe: nothing to read at all.
    }

    if (truncated)
    {
        // A line this long can never be a real "y"/"yes" answer - isAffirmative()
        // would refuse it anyway - so refusal is reported directly rather than handing
        // back a misleadingly truncated fragment.
        return std::wstring();
    }

    if (!bytes.empty() && bytes.back() == '\r')
    {
        bytes.pop_back();
    }
    return utf8ToWide(bytes);
}

// Shared, per-Console-instance state the production tryEnableVirtualTerminal/
// restoreOutputMode closures need: the mode SetConsoleMode observed before this process
// changed it, and whether this process actually changed it (vs. it already being on).
struct ProductionOutputModeState
{
    DWORD originalMode{0};
    bool changed{false};
};

[[nodiscard]] ConsoleOperations makeProductionConsoleOperations()
{
    ConsoleOperations operations;
    const auto modeState = std::make_shared<ProductionOutputModeState>();

    operations.isConsole = [](ConsoleStream stream) {
        return isConsoleHandle(handleForStream(stream));
    };

    operations.write = [](ConsoleStream stream, std::wstring_view text) {
        const HANDLE handle = handleForStream(stream);
        if (!isUsableHandle(handle))
        {
            return;
        }
        if (isConsoleHandle(handle))
        {
            writeConsoleChunked(handle, text);
        }
        else
        {
            writeFileChunked(handle, toUtf8(text));
        }
    };

    operations.tryEnableVirtualTerminal = [modeState]() {
        const HANDLE handle = handleForStream(ConsoleStream::Output);
        if (!isConsoleHandle(handle))
        {
            return false;
        }

        DWORD originalMode = 0;
        if (!::GetConsoleMode(handle, &originalMode))
        {
            return false;
        }

        const DWORD desiredMode = originalMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (desiredMode == originalMode)
        {
            modeState->changed = false;
            return true; // Already enabled - nothing to restore later.
        }

        if (!::SetConsoleMode(handle, desiredMode))
        {
            return false;
        }

        modeState->originalMode = originalMode;
        modeState->changed = true;
        return true;
    };

    operations.restoreOutputMode = [modeState]() {
        if (!modeState->changed)
        {
            return;
        }
        const HANDLE handle = handleForStream(ConsoleStream::Output);
        if (isUsableHandle(handle))
        {
            ::SetConsoleMode(handle, modeState->originalMode);
        }
        modeState->changed = false;
    };

    operations.readLine = []() -> std::optional<std::wstring> {
        const HANDLE handle = handleForStream(ConsoleStream::Input);
        if (!isUsableHandle(handle))
        {
            return std::nullopt;
        }
        return isConsoleHandle(handle) ? readLineFromConsole(handle)
                                       : readLineFromRedirectedStream(handle);
    };

    return operations;
}

[[nodiscard]] std::optional<std::wstring> readNoColorEnvironmentVariable()
{
    // 203L, per WinError.h - not pulled in as a named macro under this project's
    // WIN32_LEAN_AND_MEAN configuration, so the numeric value is used directly.
    constexpr DWORD kErrorEnvironmentVariableNotFound = 203L;

    wchar_t probe[8]{};
    const DWORD result =
        ::GetEnvironmentVariableW(L"NO_COLOR", probe, static_cast<DWORD>(std::size(probe)));
    if (result == 0 && ::GetLastError() == kErrorEnvironmentVariableNotFound)
    {
        return std::nullopt;
    }
    // Presence is all that matters (see noColorEnvSet()); the exact value, and whether
    // it was truncated by the small probe buffer, are both irrelevant.
    return std::wstring(L"set");
}
} // namespace

std::wstring sanitizeForDisplay(std::wstring_view text) noexcept
{
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t ch : text)
    {
        if (!isDisplayBlocked(ch))
        {
            result.push_back(ch);
        }
    }
    return result;
}

bool isAffirmative(const std::optional<std::wstring>& line) noexcept
{
    if (!line.has_value())
    {
        return false;
    }

    const std::wstring_view trimmed = trimAsciiWhitespace(*line);
    if (trimmed.empty())
    {
        return false;
    }

    return equalsOrdinalIgnoreCase(trimmed, L"y") || equalsOrdinalIgnoreCase(trimmed, L"yes");
}

bool noColorEnvSet(const std::optional<std::wstring>& noColorEnvValue) noexcept
{
    return noColorEnvValue.has_value();
}

Console::Console(bool noColorRequested)
    : Console(noColorRequested, makeProductionConsoleOperations(),
             readNoColorEnvironmentVariable())
{
}

Console::Console(bool noColorRequested, ConsoleOperations operations,
                 std::optional<std::wstring> noColorEnvValueOverride)
    : m_operations(std::move(operations)), m_colorEnabled(false)
{
    m_stdoutInteractive =
        m_operations.isConsole && m_operations.isConsole(ConsoleStream::Output);
    m_stdinInteractive =
        m_operations.isConsole && m_operations.isConsole(ConsoleStream::Input);

    bool vtCapable = false;
    if (m_stdoutInteractive && m_operations.tryEnableVirtualTerminal)
    {
        vtCapable = m_operations.tryEnableVirtualTerminal();
    }
    m_vtEnabled = vtCapable;

    m_colorEnabled =
        vtCapable && !noColorRequested && !noColorEnvSet(noColorEnvValueOverride);
}

Console::~Console()
{
    if (m_operations.restoreOutputMode)
    {
        m_operations.restoreOutputMode();
    }
}

void Console::writeLine(std::wstring_view text, ConsoleStream stream)
{
    if (!m_operations.write)
    {
        return;
    }
    m_operations.write(stream, sanitizeForDisplay(text) + L"\n");
}

bool Console::confirm(std::wstring_view promptText, bool assumeYes)
{
    if (assumeYes)
    {
        return true;
    }

    if (m_operations.write)
    {
        m_operations.write(ConsoleStream::Output, sanitizeForDisplay(promptText));
    }

    const std::optional<std::wstring> line =
        m_operations.readLine ? m_operations.readLine() : std::nullopt;
    return isAffirmative(line);
}
} // namespace syncwingetlink::cli
