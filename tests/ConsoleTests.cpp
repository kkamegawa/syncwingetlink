// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/Console.h>

#include <Windows.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::cli;

namespace syncwingetlink::tests
{
TEST_CLASS(SanitizeForDisplayTests)
{
public:
    TEST_METHOD(ordinaryTextPassesThroughUnchanged)
    {
        Assert::AreEqual(std::wstring(L"codex-x86_64-pc-windows-msvc.exe"),
                         sanitizeForDisplay(L"codex-x86_64-pc-windows-msvc.exe"));
    }

    TEST_METHOD(escapeCharacterIsStripped)
    {
        // "\x1B" "after" as separate literals - a hex escape is greedy and would
        // otherwise swallow "af" from "after" as extra hex digits (\x1Baf is one
        // character, U+1BAF), which is not what this test means to construct.
        const std::wstring withEscape = L"before" L"\x1B" L"after";
        Assert::AreEqual(std::wstring(L"beforeafter"), sanitizeForDisplay(withEscape));
    }

    TEST_METHOD(everyC0ControlIsStripped)
    {
        for (wchar_t ch = 0x00; ch <= 0x1F; ++ch)
        {
            const std::wstring withControl = std::wstring(L"a") + ch + L"b";
            Assert::AreEqual(std::wstring(L"ab"), sanitizeForDisplay(withControl));
        }
    }

    TEST_METHOD(delAndC1ControlsAreStripped)
    {
        Assert::AreEqual(std::wstring(L"ab"),
                         sanitizeForDisplay(std::wstring(L"a") + wchar_t(0x7F) + L"b"));
        Assert::AreEqual(std::wstring(L"ab"),
                         sanitizeForDisplay(std::wstring(L"a") + wchar_t(0x9B) + L"b"));
    }

    TEST_METHOD(bidiOverrideAndIsolateCharactersAreStripped)
    {
        Assert::AreEqual(std::wstring(L"ab"),
                         sanitizeForDisplay(std::wstring(L"a") + wchar_t(0x202E) + L"b"));
        Assert::AreEqual(std::wstring(L"ab"),
                         sanitizeForDisplay(std::wstring(L"a") + wchar_t(0x2066) + L"b"));
    }

    TEST_METHOD(carriageReturnAndNewlineAreStripped)
    {
        Assert::AreEqual(std::wstring(L"fakeline"),
                         sanitizeForDisplay(L"fake\r\nline"));
    }

    TEST_METHOD(emptyInputProducesEmptyOutput)
    {
        Assert::AreEqual(std::wstring(L""), sanitizeForDisplay(L""));
    }
};

TEST_CLASS(IsAffirmativeTests)
{
public:
    TEST_METHOD(noLineAtAllIsRefusal)
    {
        Assert::IsFalse(isAffirmative(std::nullopt));
    }

    TEST_METHOD(emptyLineIsRefusal)
    {
        Assert::IsFalse(isAffirmative(std::wstring(L"")));
    }

    TEST_METHOD(whitespaceOnlyLineIsRefusal)
    {
        Assert::IsFalse(isAffirmative(std::wstring(L"   \t")));
    }

    TEST_METHOD(yAndYesAreAffirmativeCaseInsensitively)
    {
        Assert::IsTrue(isAffirmative(std::wstring(L"y")));
        Assert::IsTrue(isAffirmative(std::wstring(L"Y")));
        Assert::IsTrue(isAffirmative(std::wstring(L"yes")));
        Assert::IsTrue(isAffirmative(std::wstring(L"YES")));
        Assert::IsTrue(isAffirmative(std::wstring(L"Yes")));
    }

    TEST_METHOD(surroundingWhitespaceIsTrimmedBeforeComparison)
    {
        Assert::IsTrue(isAffirmative(std::wstring(L"  yes  ")));
    }

    TEST_METHOD(anythingElseIsRefusal)
    {
        Assert::IsFalse(isAffirmative(std::wstring(L"n")));
        Assert::IsFalse(isAffirmative(std::wstring(L"no")));
        Assert::IsFalse(isAffirmative(std::wstring(L"yeah")));
        Assert::IsFalse(isAffirmative(std::wstring(L"maybe")));
    }
};

TEST_CLASS(NoColorEnvSetTests)
{
public:
    TEST_METHOD(absentValueIsNotSet)
    {
        Assert::IsFalse(noColorEnvSet(std::nullopt));
    }

    TEST_METHOD(anyPresentValueIsSetIncludingEmpty)
    {
        Assert::IsTrue(noColorEnvSet(std::wstring(L"")));
        Assert::IsTrue(noColorEnvSet(std::wstring(L"1")));
        Assert::IsTrue(noColorEnvSet(std::wstring(L"anything")));
    }
};

namespace
{
[[nodiscard]] ConsoleOperations makeFakeOperations(bool outputIsConsole,
                                                   bool virtualTerminalSucceeds,
                                                   std::vector<std::wstring>& writes,
                                                   std::vector<std::optional<std::wstring>>
                                                       linesToReturn)
{
    ConsoleOperations operations;
    auto remainingLines =
        std::make_shared<std::vector<std::optional<std::wstring>>>(std::move(linesToReturn));
    auto restoreCount = std::make_shared<int>(0);
    auto vtSucceeds = virtualTerminalSucceeds;

    operations.isConsole = [outputIsConsole](ConsoleStream stream) {
        return stream == ConsoleStream::Output ? outputIsConsole : false;
    };
    operations.write = [&writes](ConsoleStream, std::wstring_view text) {
        writes.emplace_back(text);
    };
    operations.tryEnableVirtualTerminal = [vtSucceeds]() { return vtSucceeds; };
    operations.restoreOutputMode = [restoreCount]() { ++*restoreCount; };
    operations.readLine = [remainingLines]() -> std::optional<std::wstring> {
        if (remainingLines->empty())
        {
            return std::nullopt;
        }
        std::optional<std::wstring> next = remainingLines->front();
        remainingLines->erase(remainingLines->begin());
        return next;
    };

    return operations;
}
} // namespace

TEST_CLASS(ConsoleTests)
{
public:
    TEST_METHOD(colorIsEnabledOnlyWhenConsoleAndVtSucceedsAndNotSuppressed)
    {
        std::vector<std::wstring> writes;
        Console console(/*noColorRequested=*/false,
                        makeFakeOperations(/*outputIsConsole=*/true,
                                          /*virtualTerminalSucceeds=*/true, writes, {}));
        Assert::IsTrue(console.colorEnabled());
    }

    TEST_METHOD(colorIsDisabledWhenRedirected)
    {
        std::vector<std::wstring> writes;
        Console console(/*noColorRequested=*/false,
                        makeFakeOperations(/*outputIsConsole=*/false,
                                          /*virtualTerminalSucceeds=*/true, writes, {}));
        Assert::IsFalse(console.colorEnabled());
    }

    TEST_METHOD(colorIsDisabledByNoColorFlag)
    {
        std::vector<std::wstring> writes;
        Console console(/*noColorRequested=*/true,
                        makeFakeOperations(/*outputIsConsole=*/true,
                                          /*virtualTerminalSucceeds=*/true, writes, {}));
        Assert::IsFalse(console.colorEnabled());
    }

    TEST_METHOD(colorIsDisabledByNoColorEnvironmentVariable)
    {
        std::vector<std::wstring> writes;
        Console console(/*noColorRequested=*/false,
                        makeFakeOperations(/*outputIsConsole=*/true,
                                          /*virtualTerminalSucceeds=*/true, writes, {}),
                        std::wstring(L""));
        Assert::IsFalse(console.colorEnabled());
    }

    TEST_METHOD(colorIsDisabledWhenVirtualTerminalCannotBeEnabled)
    {
        std::vector<std::wstring> writes;
        Console console(/*noColorRequested=*/false,
                        makeFakeOperations(/*outputIsConsole=*/true,
                                          /*virtualTerminalSucceeds=*/false, writes, {}));
        Assert::IsFalse(console.colorEnabled());
    }

    TEST_METHOD(destructorRestoresOutputModeExactlyOnce)
    {
        std::vector<std::wstring> writes;
        int restoreCalls = 0;
        {
            ConsoleOperations operations =
                makeFakeOperations(true, true, writes, {});
            operations.restoreOutputMode = [&restoreCalls]() { ++restoreCalls; };
            Console console(false, std::move(operations));
        }
        Assert::AreEqual(1, restoreCalls);
    }

    TEST_METHOD(writeLineSanitizesAndAppendsNewline)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {}));

        console.writeLine(L"tool\x1B.exe");

        Assert::AreEqual(size_t{1}, writes.size());
        Assert::AreEqual(std::wstring(L"tool.exe\n"), writes[0]);
    }

    TEST_METHOD(confirmWithAssumeYesNeverTouchesPromptOrStdin)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {L"should not be read"}));

        Assert::IsTrue(console.confirm(L"Proceed?", /*assumeYes=*/true));
        Assert::IsTrue(writes.empty());
    }

    TEST_METHOD(confirmReadsAndEvaluatesOneLine)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {std::wstring(L"yes")}));

        Assert::IsTrue(console.confirm(L"Proceed?", /*assumeYes=*/false));
        Assert::AreEqual(size_t{1}, writes.size());
        Assert::AreEqual(std::wstring(L"Proceed?"), writes[0]);
    }

    TEST_METHOD(confirmTreatsEofAsRefusal)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {std::nullopt}));

        Assert::IsFalse(console.confirm(L"Proceed?", /*assumeYes=*/false));
    }

    TEST_METHOD(confirmTreatsBareEnterAsRefusal)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {std::wstring(L"")}));

        Assert::IsFalse(console.confirm(L"Proceed?", /*assumeYes=*/false));
    }

    TEST_METHOD(confirmTreatsNoAsRefusal)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {std::wstring(L"no")}));

        Assert::IsFalse(console.confirm(L"Proceed?", /*assumeYes=*/false));
    }
};

// #113 (ADR-0030): a table-driven test over every (LogLevel, MessageImportance)
// combination, so a future regression back to "--verbose/--quiet parsed but ignored"
// fails here rather than passing silently. Console::writeLine() is the one place that
// gates emission, so this is exercised directly rather than through cli::Dispatch (whose
// internal helpers are file-local and untestable, per DispatchTests.cpp's header
// comment).
TEST_CLASS(MessageImportanceTests)
{
public:
    TEST_METHOD(quietShowsNormalOnly)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {}), std::nullopt,
                        LogLevel::Quiet);

        console.writeLine(L"supplementary", ConsoleStream::Output,
                          MessageImportance::Supplementary);
        console.writeLine(L"normal", ConsoleStream::Output, MessageImportance::Normal);
        console.writeLine(L"diagnostic", ConsoleStream::Output, MessageImportance::Diagnostic);

        Assert::AreEqual(size_t{1}, writes.size());
        Assert::AreEqual(std::wstring(L"normal\n"), writes[0]);
    }

    TEST_METHOD(normalShowsSupplementaryAndNormalButNotDiagnostic)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {}), std::nullopt,
                        LogLevel::Normal);

        console.writeLine(L"supplementary", ConsoleStream::Output,
                          MessageImportance::Supplementary);
        console.writeLine(L"normal", ConsoleStream::Output, MessageImportance::Normal);
        console.writeLine(L"diagnostic", ConsoleStream::Output, MessageImportance::Diagnostic);

        Assert::AreEqual(size_t{2}, writes.size());
        Assert::AreEqual(std::wstring(L"supplementary\n"), writes[0]);
        Assert::AreEqual(std::wstring(L"normal\n"), writes[1]);
    }

    TEST_METHOD(verboseShowsEverything)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {}), std::nullopt,
                        LogLevel::Verbose);

        console.writeLine(L"supplementary", ConsoleStream::Output,
                          MessageImportance::Supplementary);
        console.writeLine(L"normal", ConsoleStream::Output, MessageImportance::Normal);
        console.writeLine(L"diagnostic", ConsoleStream::Output, MessageImportance::Diagnostic);

        Assert::AreEqual(size_t{3}, writes.size());
        Assert::AreEqual(std::wstring(L"supplementary\n"), writes[0]);
        Assert::AreEqual(std::wstring(L"normal\n"), writes[1]);
        Assert::AreEqual(std::wstring(L"diagnostic\n"), writes[2]);
    }

    TEST_METHOD(defaultConstructorLogLevelIsNormal)
    {
        // Every pre-#113 call site (Console(false, operations)) keeps compiling and
        // behaving as Normal, matching the "existing call sites keep working" design
        // requirement.
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {}));

        console.writeLine(L"diagnostic", ConsoleStream::Output, MessageImportance::Diagnostic);
        console.writeLine(L"normal", ConsoleStream::Output, MessageImportance::Normal);

        Assert::AreEqual(size_t{1}, writes.size());
        Assert::AreEqual(std::wstring(L"normal\n"), writes[0]);
    }

    TEST_METHOD(quietNeverSuppressesErrorStreamNormalImportanceLines)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(true, true, writes, {}), std::nullopt,
                        LogLevel::Quiet);

        console.writeLine(L"warning: something", ConsoleStream::Error);

        Assert::AreEqual(size_t{1}, writes.size());
    }
};

// #62: Console display fidelity for non-ASCII text (Japanese katakana - no case
// distinction - plus U+1F600, a non-BMP character also with no case distinction; see
// SmokeTests.cpp's matching round-trip tests for why that matters). Written with
// \uXXXX/\UXXXXXXXX escapes only; no raw non-ASCII bytes in this file.
//
// Console::writeLine() computes sanitizeForDisplay(text) + L"\n" once and hands it to
// ConsoleOperations::write - the WriteConsoleW-vs-WriteFile(UTF-8) branch (the "real
// console" vs "redirected" distinction) lives entirely inside the production
// implementation (makeProductionConsoleOperations() in Console.cpp), which is not part
// of this seam. So the two tests below prove the thing this seam *can* prove: Console
// hands the identical, uncorrupted wide text to whichever write implementation is
// installed, regardless of whether isConsole reports true or false. The third test
// separately confirms, via a real WideCharToMultiByte(CP_UTF8) call on this exact
// fixture, that the redirected path's encoding step (which Console.cpp's private
// toUtf8() also calls) produces the correct UTF-8 bytes for this text - the standard
// Win32 conversion API's correctness for well-formed UTF-16 input is not re-implemented
// or re-tested here, only exercised against this specific fixture.
TEST_CLASS(NonAsciiDisplayTests)
{
public:
    TEST_METHOD(nonAsciiTextReachesWriteUnmodifiedWhenStdoutIsARealConsole)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(/*outputIsConsole=*/true, true, writes, {}));
        const std::wstring text = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600";

        console.writeLine(text);

        Assert::AreEqual(size_t{1}, writes.size());
        Assert::AreEqual(text + L"\n", writes[0]);
    }

    TEST_METHOD(nonAsciiTextReachesWriteUnmodifiedWhenStdoutIsRedirected)
    {
        std::vector<std::wstring> writes;
        Console console(false, makeFakeOperations(/*outputIsConsole=*/false, true, writes, {}));
        const std::wstring text = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600";

        console.writeLine(text);

        Assert::AreEqual(size_t{1}, writes.size());
        Assert::AreEqual(text + L"\n", writes[0]);
    }

    TEST_METHOD(redirectedPathEncodingProducesCorrectUtf8ForTheFixture)
    {
        const std::wstring text = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600";

        const int required = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                    static_cast<int>(text.size()), nullptr, 0,
                                                    nullptr, nullptr);
        Assert::IsTrue(required > 0);
        std::string utf8(static_cast<std::size_t>(required), '\0');
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                   static_cast<int>(text.size()), utf8.data(),
                                                   required, nullptr, nullptr);
        Assert::IsTrue(written == required);

        // 6 katakana characters x 3 UTF-8 bytes each, plus U+1F600's 4-byte encoding.
        Assert::AreEqual(std::size_t{6 * 3 + 4}, utf8.size());
        // U+1F600 GRINNING FACE, UTF-8: F0 9F 98 80 - the last 4 bytes.
        const std::size_t tail = utf8.size() - 4;
        Assert::AreEqual(static_cast<unsigned char>(0xF0),
                         static_cast<unsigned char>(utf8[tail + 0]));
        Assert::AreEqual(static_cast<unsigned char>(0x9F),
                         static_cast<unsigned char>(utf8[tail + 1]));
        Assert::AreEqual(static_cast<unsigned char>(0x98),
                         static_cast<unsigned char>(utf8[tail + 2]));
        Assert::AreEqual(static_cast<unsigned char>(0x80),
                         static_cast<unsigned char>(utf8[tail + 3]));
    }
};

namespace
{
// Unlike makeFakeOperations() above (which only ever reports Output as a console, since
// no M6 test needed stdin's console-ness observed independently), these TUI capability
// tests (#58) need isConsole to answer differently for Output and Input.
[[nodiscard]] ConsoleOperations makeFakeOperationsWithStreams(bool outputIsConsole,
                                                              bool inputIsConsole,
                                                              bool virtualTerminalSucceeds)
{
    ConsoleOperations operations;
    operations.isConsole = [outputIsConsole, inputIsConsole](ConsoleStream stream) {
        switch (stream)
        {
        case ConsoleStream::Output:
            return outputIsConsole;
        case ConsoleStream::Input:
            return inputIsConsole;
        case ConsoleStream::Error:
            return false;
        }
        return false;
    };
    operations.write = [](ConsoleStream, std::wstring_view) {};
    operations.tryEnableVirtualTerminal = [virtualTerminalSucceeds]() {
        return virtualTerminalSucceeds;
    };
    operations.restoreOutputMode = []() {};
    operations.readLine = []() -> std::optional<std::wstring> { return std::nullopt; };
    return operations;
}
} // namespace

// #58: Console exposes stdin/stdout/VT capability independently of colorEnabled(), so
// the M7 TUI can gate on interactivity and VT without color being part of the decision.
TEST_CLASS(TerminalCapabilityTests)
{
public:
    TEST_METHOD(vtEnabledReflectsCapabilityRegardlessOfNoColor)
    {
        Console withColor(/*noColorRequested=*/false,
                          makeFakeOperationsWithStreams(true, true, true));
        Console withoutColor(/*noColorRequested=*/true,
                             makeFakeOperationsWithStreams(true, true, true));

        Assert::IsTrue(withColor.vtEnabled());
        Assert::IsTrue(withoutColor.vtEnabled());
        // colorEnabled() differs even though vtEnabled() does not - --no-color gates
        // color only, never VT capability.
        Assert::IsTrue(withColor.colorEnabled());
        Assert::IsFalse(withoutColor.colorEnabled());
    }

    TEST_METHOD(vtEnabledFalseWhenActivationFails)
    {
        Console console(false, makeFakeOperationsWithStreams(true, true, false));
        Assert::IsFalse(console.vtEnabled());
    }

    TEST_METHOD(stdinAndStdoutInteractiveTrueWhenBothAreConsoles)
    {
        Console console(false, makeFakeOperationsWithStreams(true, true, true));
        Assert::IsTrue(console.stdinInteractive());
        Assert::IsTrue(console.stdoutInteractive());
    }

    TEST_METHOD(stdinInteractiveFalseWhenInputIsRedirected)
    {
        Console console(false, makeFakeOperationsWithStreams(true, false, true));
        Assert::IsTrue(console.stdoutInteractive());
        Assert::IsFalse(console.stdinInteractive());
    }

    TEST_METHOD(stdoutInteractiveFalseWhenOutputIsRedirected)
    {
        Console console(false, makeFakeOperationsWithStreams(false, true, true));
        Assert::IsFalse(console.stdoutInteractive());
        // VT can never be enabled when stdout itself is not a console.
        Assert::IsFalse(console.vtEnabled());
    }
};
} // namespace syncwingetlink::tests
