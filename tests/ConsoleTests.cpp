// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/Console.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
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
