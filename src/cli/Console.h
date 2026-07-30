// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace syncwingetlink::cli
{
// Strips characters that could rewrite already-printed terminal lines or disguise
// displayed text: C0 controls (0x00-0x1F, including ESC) and DEL (0x7F), C1 controls
// (0x80-0x9F), and the Unicode bidi override/isolate characters (U+202A-U+202E,
// U+2066-U+2069). Pure and total - safe to call on any untrusted string (a package id,
// executable file name, or alias) before it reaches console or JSON output. Stripping
// rather than escaping is deliberate: the sanitized copy is for display only, never fed
// back into filesystem or comparison logic, so there is no need to preserve a reversible
// representation of what was removed.
[[nodiscard]] std::wstring sanitizeForDisplay(std::wstring_view text) noexcept;

// Interprets one line of prompt input as consent. nullopt (no line could be read - EOF,
// a closed pipe, or a read failure) is refusal. An empty line (bare Enter) is refusal.
// Anything other than "y" or "yes", ordinal and case-insensitive, after trimming
// surrounding spaces/tabs/CR, is refusal. This is the one place the "non-interactive
// stdin is not consent" and "bare Enter is not consent" rules are enforced.
[[nodiscard]] bool isAffirmative(const std::optional<std::wstring>& line) noexcept;

// True when the NO_COLOR convention (https://no-color.org) is in effect: the
// environment variable is set to any value, including empty. Takes the already-looked-up
// value (rather than reading the environment itself) so it stays a pure, testable
// function; Console's production constructor performs the actual lookup.
[[nodiscard]] bool noColorEnvSet(const std::optional<std::wstring>& noColorEnvValue) noexcept;

enum class ConsoleStream
{
    Output, // stdout
    Error,  // stderr
    Input,  // stdin - only meaningful to ConsoleOperations::isConsole, never to write()
};

// The Win32-facing operations Console drives. Production code (Console's single-argument
// constructor) wraps the real APIs; tests supply deterministic callbacks so redirected
// vs. real-console behavior, VT capability, and EOF vs. a real input line are all
// covered without an actual terminal - the same seam pattern SymlinkServiceOperations
// (core/SymlinkService.h) already establishes for this codebase.
struct ConsoleOperations
{
    // True when the given stream's handle is a real console: GetStdHandle succeeded
    // (not NULL/INVALID_HANDLE_VALUE) and GetConsoleMode succeeds on it.
    std::function<bool(ConsoleStream)> isConsole;

    // Writes text verbatim to the given stream (Output or Error; never Input). The
    // caller (Console) has already sanitized/formatted it. Production: WriteConsoleW in
    // chunks when isConsole(stream) is true, else UTF-8 bytes via WriteFile in chunks.
    std::function<void(ConsoleStream, std::wstring_view)> write;

    // Attempts to enable ENABLE_VIRTUAL_TERMINAL_PROCESSING on stdout and reports
    // whether it ends up enabled (already-on or freshly turned on). Never called unless
    // isConsole(Output) is true. If this returns true after actually changing the mode,
    // restoreOutputMode() below is guaranteed to be called exactly once, from
    // ~Console(), to put the original mode back.
    std::function<bool()> tryEnableVirtualTerminal;

    // Restores stdout's console mode to what it was before tryEnableVirtualTerminal()
    // changed it. Called at most once, from ~Console(). A no-op if
    // tryEnableVirtualTerminal() was never called or did not change anything.
    std::function<void()> restoreOutputMode;

    // Reads one line from stdin, self-contained (any console-mode change it makes for
    // line/echo input is saved and restored within this single call, not held across
    // calls). Returns nullopt on EOF, a closed/redirected stream with nothing left to
    // read, or a read failure - never synthesizes an empty line for any of those.
    // Production: ReadConsoleW with ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT when
    // isConsole(Input) is true, else a byte read decoded as UTF-8.
    std::function<std::optional<std::wstring>()> readLine;
};

// Owns console interaction for the process: stream selection, color/VT capability
// detection, sanitized output, and confirmation prompts. See docs/adr-phase-5.md and
// the Wiki page plan/syncwingetlink/m6-command-line-interface for the full security and
// Win32 contract this class implements.
class Console
{
public:
    // Production constructor: wraps the real Win32 APIs. noColorRequested is
    // AppOptions::noColor (--no-color); the NO_COLOR environment variable is also
    // consulted. Attempts to enable virtual terminal processing on stdout exactly once,
    // at construction, and restores the original mode in the destructor.
    explicit Console(bool noColorRequested);

    // Deterministic constructor for tests: operations replaces every Win32 call, and
    // noColorEnvValueOverride replaces a real NO_COLOR environment lookup (defaulted to
    // nullopt - "unset" - so a test is never at the mercy of the real test runner's
    // environment unless it explicitly opts in).
    Console(bool noColorRequested, ConsoleOperations operations,
           std::optional<std::wstring> noColorEnvValueOverride = std::nullopt);

    ~Console();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;
    Console(Console&&) = delete;
    Console& operator=(Console&&) = delete;

    // Whether color/VT output should be produced: stdout is a real console, enabling
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING on it succeeded, --no-color was not passed, and
    // NO_COLOR is not set. M7 is the actual consumer of this for styled output; M6 only
    // computes and exposes it.
    [[nodiscard]] bool colorEnabled() const noexcept
    {
        return m_colorEnabled;
    }

    // Sanitizes text (sanitizeForDisplay()) and writes it followed by a newline to the
    // given stream.
    void writeLine(std::wstring_view text, ConsoleStream stream = ConsoleStream::Output);

    // Writes promptText (sanitized) to stdout, with no trailing newline, then evaluates
    // consent. assumeYes bypasses the prompt entirely: neither promptText nor stdin is
    // touched, and true is returned immediately - callers remain responsible for every
    // other gate a "yes" must still pass (in particular, a candidate belonging to a
    // detected alias collision is excluded before repair regardless of what confirm()
    // returns; that exclusion is dispatch's (#56) responsibility, not this class's).
    // Otherwise, one line is read from stdin and passed through isAffirmative().
    [[nodiscard]] bool confirm(std::wstring_view promptText, bool assumeYes);

private:
    ConsoleOperations m_operations;
    bool m_colorEnabled;
};
} // namespace syncwingetlink::cli
