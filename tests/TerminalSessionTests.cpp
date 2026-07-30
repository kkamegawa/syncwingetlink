// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <tui/TerminalSession.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink::tui;

namespace syncwingetlink::tests
{
namespace
{
// A fake stdin console mode with only ENABLE_PROCESSED_INPUT set (a plausible starting
// mode - real console defaults also include ENABLE_LINE_INPUT/ENABLE_ECHO_INPUT/
// ENABLE_INSERT_MODE/ENABLE_QUICK_EDIT_MODE/ENABLE_EXTENDED_FLAGS, but the exact
// starting bits do not matter for these tests beyond confirming the documented
// set/clear mask is applied and restored around it).
constexpr std::uint32_t kFakeOriginalStdinMode = 0x0001 | 0x0002 | 0x0004 | 0x0040 | 0x0080;

struct FakeTerminal
{
    std::vector<std::wstring> controlWrites;
    std::vector<bool> closeHandlerInstallCalls;
    std::function<void()> registeredCloseCallback;
    std::uint32_t stdinMode{kFakeOriginalStdinMode};
    bool getStdinModeSucceeds{true};
    bool setStdinModeSucceeds{true};
    int setStdinModeCalls{0};
};

[[nodiscard]] TerminalOperations makeFakeOperations(FakeTerminal& fake)
{
    TerminalOperations operations;

    operations.getStdinMode = [&fake](std::uint32_t& mode) {
        if (!fake.getStdinModeSucceeds)
        {
            return false;
        }
        mode = fake.stdinMode;
        return true;
    };

    operations.setStdinMode = [&fake](std::uint32_t mode) {
        ++fake.setStdinModeCalls;
        if (!fake.setStdinModeSucceeds)
        {
            return false;
        }
        fake.stdinMode = mode;
        return true;
    };

    operations.writeControl = [&fake](std::wstring_view text) {
        fake.controlWrites.emplace_back(text);
    };

    operations.queryViewport = []() -> std::optional<TuiResizeEvent> {
        TuiResizeEvent event;
        event.rows = 24;
        event.columns = 80;
        return event;
    };

    operations.readEvent = []() -> std::optional<TuiEvent> { return std::nullopt; };

    operations.setCloseHandler = [&fake](bool install, std::function<void()> onCloseSignal) {
        fake.closeHandlerInstallCalls.push_back(install);
        fake.registeredCloseCallback = install ? std::move(onCloseSignal) : nullptr;
    };

    return operations;
}
} // namespace

TEST_CLASS(TerminalSessionAcquisitionTests)
{
public:
    TEST_METHOD(unavailableWhenStdinNotInteractive)
    {
        FakeTerminal fake;
        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(/*stdinInteractive=*/false, /*stdoutInteractive=*/true,
                                       /*vtEnabled=*/true, makeFakeOperations(fake));

        Assert::IsFalse(session.has_value());
        Assert::IsTrue(fake.controlWrites.empty());
        Assert::AreEqual(0, fake.setStdinModeCalls);
    }

    TEST_METHOD(unavailableWhenStdoutNotInteractive)
    {
        FakeTerminal fake;
        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, /*stdoutInteractive=*/false, true,
                                       makeFakeOperations(fake));

        Assert::IsFalse(session.has_value());
        Assert::IsTrue(fake.controlWrites.empty());
    }

    TEST_METHOD(unavailableWhenVtNotEnabled)
    {
        FakeTerminal fake;
        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, /*vtEnabled=*/false, makeFakeOperations(fake));

        Assert::IsFalse(session.has_value());
        Assert::IsTrue(fake.controlWrites.empty());
    }

    TEST_METHOD(successfulAcquisitionEntersAlternateScreenThenHidesCursorThenChangesStdinMode)
    {
        FakeTerminal fake;
        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));

        Assert::IsTrue(session.has_value());
        Assert::AreEqual(size_t{2}, fake.controlWrites.size());
        Assert::AreEqual(std::wstring(L"\x1b[?1049h"), fake.controlWrites[0]);
        Assert::AreEqual(std::wstring(L"\x1b[?25l"), fake.controlWrites[1]);
        Assert::AreEqual(1, fake.setStdinModeCalls);

        // The applied mode sets ENABLE_WINDOW_INPUT|ENABLE_EXTENDED_FLAGS and clears
        // ENABLE_LINE_INPUT|ENABLE_ECHO_INPUT|ENABLE_PROCESSED_INPUT|
        // ENABLE_VIRTUAL_TERMINAL_INPUT|ENABLE_QUICK_EDIT_MODE|ENABLE_MOUSE_INPUT.
        constexpr std::uint32_t kEnableWindowInput = 0x0008;
        constexpr std::uint32_t kEnableExtendedFlags = 0x0080;
        constexpr std::uint32_t kEnableProcessedInput = 0x0001;
        constexpr std::uint32_t kEnableLineInput = 0x0002;
        constexpr std::uint32_t kEnableEchoInput = 0x0004;
        constexpr std::uint32_t kEnableQuickEditMode = 0x0040;
        Assert::IsTrue((fake.stdinMode & kEnableWindowInput) != 0);
        Assert::IsTrue((fake.stdinMode & kEnableExtendedFlags) != 0);
        Assert::IsFalse((fake.stdinMode & kEnableProcessedInput) != 0);
        Assert::IsFalse((fake.stdinMode & kEnableLineInput) != 0);
        Assert::IsFalse((fake.stdinMode & kEnableEchoInput) != 0);
        Assert::IsFalse((fake.stdinMode & kEnableQuickEditMode) != 0);
    }

    TEST_METHOD(successfulAcquisitionInstallsCloseHandler)
    {
        FakeTerminal fake;
        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));

        Assert::IsTrue(session.has_value());
        Assert::AreEqual(size_t{1}, fake.closeHandlerInstallCalls.size());
        Assert::IsTrue(fake.closeHandlerInstallCalls[0]);
        Assert::IsTrue(static_cast<bool>(fake.registeredCloseCallback));
    }

    TEST_METHOD(partialFailureAtStdinModeReadUnwindsCursorAndAlternateScreenInReverseOrder)
    {
        FakeTerminal fake;
        fake.getStdinModeSucceeds = false;

        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));

        Assert::IsFalse(session.has_value());
        // Enter sequence (2 writes) followed by the reverse-order unwind (show cursor,
        // then leave alternate screen) - 4 total, in this exact order.
        Assert::AreEqual(size_t{4}, fake.controlWrites.size());
        Assert::AreEqual(std::wstring(L"\x1b[?1049h"), fake.controlWrites[0]);
        Assert::AreEqual(std::wstring(L"\x1b[?25l"), fake.controlWrites[1]);
        Assert::AreEqual(std::wstring(L"\x1b[?25h"), fake.controlWrites[2]);
        Assert::AreEqual(std::wstring(L"\x1b[?1049l"), fake.controlWrites[3]);
        // Stdin mode was never read successfully, so it must never be written either.
        Assert::AreEqual(0, fake.setStdinModeCalls);
    }

    TEST_METHOD(partialFailureAtStdinModeWriteUnwindsCursorAndAlternateScreen)
    {
        FakeTerminal fake;
        fake.setStdinModeSucceeds = false;

        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));

        Assert::IsFalse(session.has_value());
        Assert::AreEqual(size_t{4}, fake.controlWrites.size());
        Assert::AreEqual(std::wstring(L"\x1b[?25h"), fake.controlWrites[2]);
        Assert::AreEqual(std::wstring(L"\x1b[?1049l"), fake.controlWrites[3]);
        // The original (unchanged) mode is never reported as applied.
        Assert::AreEqual(kFakeOriginalStdinMode, fake.stdinMode);
    }
};

TEST_CLASS(TerminalSessionRestorationTests)
{
public:
    TEST_METHOD(destructorRestoresInReverseOrderExactlyOnce)
    {
        FakeTerminal fake;
        {
            std::optional<TerminalSession> session =
                TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));
            Assert::IsTrue(session.has_value());
        }

        // 2 enter-sequence writes + 2 teardown writes (show cursor, leave alt screen).
        Assert::AreEqual(size_t{4}, fake.controlWrites.size());
        Assert::AreEqual(std::wstring(L"\x1b[?25h"), fake.controlWrites[2]);
        Assert::AreEqual(std::wstring(L"\x1b[?1049l"), fake.controlWrites[3]);
        // setStdinMode: once to enter the session, once to restore it.
        Assert::AreEqual(2, fake.setStdinModeCalls);
        Assert::AreEqual(kFakeOriginalStdinMode, fake.stdinMode);
        // The close handler is uninstalled on the way out.
        Assert::AreEqual(size_t{2}, fake.closeHandlerInstallCalls.size());
        Assert::IsFalse(fake.closeHandlerInstallCalls[1]);
    }

    TEST_METHOD(explicitRestoreThenDestructorRestoresOnlyOnce)
    {
        FakeTerminal fake;
        {
            std::optional<TerminalSession> session =
                TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));
            Assert::IsTrue(session.has_value());

            session->restore();
            Assert::AreEqual(size_t{4}, fake.controlWrites.size());
            Assert::AreEqual(2, fake.setStdinModeCalls);

            session->restore(); // A second explicit call must be a no-op.
        }

        // Still exactly 4 control writes and 2 setStdinMode calls - the destructor's
        // own restore() call found the session already restored and did nothing.
        Assert::AreEqual(size_t{4}, fake.controlWrites.size());
        Assert::AreEqual(2, fake.setStdinModeCalls);
    }

    TEST_METHOD(closeHandlerCallbackRestoresState)
    {
        FakeTerminal fake;
        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));
        Assert::IsTrue(session.has_value());
        Assert::IsTrue(static_cast<bool>(fake.registeredCloseCallback));

        // Simulate CTRL_CLOSE_EVENT firing the registered callback, as
        // terminalCloseHandler() would from the console-control-handler thread.
        fake.registeredCloseCallback();

        Assert::AreEqual(size_t{4}, fake.controlWrites.size());
        Assert::AreEqual(2, fake.setStdinModeCalls);
        Assert::AreEqual(kFakeOriginalStdinMode, fake.stdinMode);
    }

    TEST_METHOD(moveTransfersOwnershipSoOnlyOneRestoreHappens)
    {
        FakeTerminal fake;
        {
            std::optional<TerminalSession> original =
                TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));
            Assert::IsTrue(original.has_value());

            TerminalSession moved = std::move(*original);
            // original is now moved-from; its destructor (at scope exit) must be a
            // no-op, and moved's destructor must perform the one real restoration.
            (void)moved;
        }

        Assert::AreEqual(size_t{4}, fake.controlWrites.size());
        Assert::AreEqual(2, fake.setStdinModeCalls);
    }
};

TEST_CLASS(TerminalSessionEventTests)
{
public:
    TEST_METHOD(readEventDelegatesToOperations)
    {
        FakeTerminal fake;
        TerminalOperations operations = makeFakeOperations(fake);
        operations.readEvent = []() -> std::optional<TuiEvent> {
            TuiKeyEvent key;
            key.character = L'y';
            key.virtualKeyCode = 'Y';
            return TuiEvent{key};
        };

        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, std::move(operations));
        Assert::IsTrue(session.has_value());

        std::optional<TuiEvent> event = session->readEvent();
        Assert::IsTrue(event.has_value());
        Assert::IsTrue(std::holds_alternative<TuiKeyEvent>(*event));
        Assert::AreEqual(L'y', std::get<TuiKeyEvent>(*event).character);
    }

    TEST_METHOD(ctrlCArrivesAsAnOrdinaryKeyEventNotAControlHandlerCallback)
    {
        // Documents the D4 contract: Ctrl+C is reported as a TuiKeyEvent with
        // ctrlPressed=true and virtualKeyCode=='C', never routed through
        // setCloseHandler (which is close/logoff/shutdown only).
        FakeTerminal fake;
        TerminalOperations operations = makeFakeOperations(fake);
        operations.readEvent = []() -> std::optional<TuiEvent> {
            TuiKeyEvent key;
            key.virtualKeyCode = 'C';
            key.ctrlPressed = true;
            return TuiEvent{key};
        };

        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, std::move(operations));
        Assert::IsTrue(session.has_value());

        std::optional<TuiEvent> event = session->readEvent();
        Assert::IsTrue(event.has_value());
        const TuiKeyEvent& key = std::get<TuiKeyEvent>(*event);
        Assert::IsTrue(key.ctrlPressed);
        Assert::AreEqual(static_cast<std::uint16_t>('C'), key.virtualKeyCode);
    }

    TEST_METHOD(writeControlDelegatesToOperationsWithoutSanitizing)
    {
        FakeTerminal fake;
        std::optional<TerminalSession> session =
            TerminalSession::tryCreate(true, true, true, makeFakeOperations(fake));
        Assert::IsTrue(session.has_value());

        session->writeControl(L"\x1b[2;5H");

        Assert::AreEqual(std::wstring(L"\x1b[2;5H"), fake.controlWrites.back());
    }
};
} // namespace syncwingetlink::tests
