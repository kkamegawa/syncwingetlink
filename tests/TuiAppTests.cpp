// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <tui/ChecklistModel.h>
#include <tui/TerminalSession.h>
#include <tui/TuiApp.h>

#include <deque>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::tui;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] ChecklistCandidate makeCandidate(const std::wstring& alias)
{
    RepairItem item;
    item.executable.path = L"C:\\Packages\\" + alias;
    item.alias = alias;
    item.linkPath = L"C:\\Links\\" + alias;
    item.status = LinkStatus::Missing;
    return ChecklistCandidate{item};
}

[[nodiscard]] TuiEvent keyEvent(std::uint16_t virtualKeyCode, wchar_t character = 0,
                                bool ctrlPressed = false)
{
    TuiKeyEvent key;
    key.virtualKeyCode = virtualKeyCode;
    key.character = character;
    key.ctrlPressed = ctrlPressed;
    return TuiEvent{key};
}

[[nodiscard]] TuiEvent resizeEvent(std::uint16_t rows, std::uint16_t columns)
{
    TuiResizeEvent resize;
    resize.rows = rows;
    resize.columns = columns;
    return TuiEvent{resize};
}

constexpr std::uint16_t kVkUp = 0x26;
constexpr std::uint16_t kVkDown = 0x28;
constexpr std::uint16_t kVkReturn = 0x0D;
constexpr std::uint16_t kVkEscape = 0x1B;

// A minimal fake TerminalSession backing: scripted events feed runChecklist(), and
// every control write it makes is captured for inspection. Mirrors
// TerminalSessionTests.cpp's FakeTerminal, extended with a scripted event queue.
struct ScriptedTerminal
{
    std::deque<std::optional<TuiEvent>> events;
    std::vector<std::wstring> controlWrites;
};

[[nodiscard]] TerminalOperations makeScriptedOperations(ScriptedTerminal& fake)
{
    TerminalOperations operations;
    operations.getStdinMode = [](std::uint32_t& mode) {
        mode = 0;
        return true;
    };
    operations.setStdinMode = [](std::uint32_t) { return true; };
    operations.writeControl = [&fake](std::wstring_view text) {
        fake.controlWrites.emplace_back(text);
    };
    operations.queryViewport = []() -> std::optional<TuiResizeEvent> {
        TuiResizeEvent event;
        event.rows = 12;
        event.columns = 80;
        return event;
    };
    operations.readEvent = [&fake]() -> std::optional<TuiEvent> {
        if (fake.events.empty())
        {
            return std::nullopt;
        }
        std::optional<TuiEvent> next = fake.events.front();
        fake.events.pop_front();
        return next;
    };
    operations.setCloseHandler = [](bool, std::function<void()>) {};
    return operations;
}

[[nodiscard]] TerminalSession makeSession(ScriptedTerminal& fake)
{
    std::optional<TerminalSession> session =
        TerminalSession::tryCreate(true, true, true, makeScriptedOperations(fake));
    Assert::IsTrue(session.has_value());
    return std::move(*session);
}
} // namespace

TEST_CLASS(RunChecklistTests)
{
public:
    TEST_METHOD(enterWithNoSelectionConfirmsWithEmptyBatch)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(kVkReturn));

        TerminalSession session = makeSession(fake);

        ChecklistModel model({makeCandidate(L"a.exe"), makeCandidate(L"b.exe")});
        ChecklistRunResult result = runChecklist(session, model);

        Assert::IsTrue(result.outcome == ChecklistOutcome::Confirmed);
        Assert::IsTrue(result.selectedCandidates.empty());
        Assert::IsFalse(fake.controlWrites.empty()); // at least the initial render
    }

    TEST_METHOD(spaceThenEnterConfirmsTheToggledCandidate)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(0, L' '));
        fake.events.push_back(keyEvent(kVkReturn));

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"a.exe"), makeCandidate(L"b.exe")});

        ChecklistRunResult result = runChecklist(session, model);

        Assert::IsTrue(result.outcome == ChecklistOutcome::Confirmed);
        Assert::AreEqual(size_t{1}, result.selectedCandidates.size());
        Assert::AreEqual(std::wstring(L"a.exe"), result.selectedCandidates[0].item.alias);
    }

    TEST_METHOD(downThenSpaceThenEnterConfirmsTheSecondCandidate)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(kVkDown));
        fake.events.push_back(keyEvent(0, L' '));
        fake.events.push_back(keyEvent(kVkReturn));

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"a.exe"), makeCandidate(L"b.exe")});

        ChecklistRunResult result = runChecklist(session, model);

        Assert::AreEqual(size_t{1}, result.selectedCandidates.size());
        Assert::AreEqual(std::wstring(L"b.exe"), result.selectedCandidates[0].item.alias);
    }

    TEST_METHOD(escapeCancelsWithNoSelection)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(0, L' ')); // select "a.exe" first
        fake.events.push_back(keyEvent(kVkEscape));

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"a.exe")});

        ChecklistRunResult result = runChecklist(session, model);

        Assert::IsTrue(result.outcome == ChecklistOutcome::Cancelled);
        Assert::IsTrue(result.selectedCandidates.empty());
        Assert::IsTrue(model.wasCancelled());
    }

    TEST_METHOD(qCancelsJustLikeEscape)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(0, L'q'));

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"a.exe")});

        ChecklistRunResult result = runChecklist(session, model);
        Assert::IsTrue(result.outcome == ChecklistOutcome::Cancelled);
    }

    TEST_METHOD(ctrlCCancelsAsAnOrdinaryKeyEvent)
    {
        // D4/ADR-0026: Ctrl+C must be handled here, as an ordinary key, never via a
        // SetConsoleCtrlHandler callback - this test exercises exactly that path.
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(/*virtualKeyCode=*/'C', 0, /*ctrlPressed=*/true));

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"a.exe")});

        ChecklistRunResult result = runChecklist(session, model);
        Assert::IsTrue(result.outcome == ChecklistOutcome::Cancelled);
    }

    TEST_METHOD(upAndDownNavigateBeforeToggling)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(kVkDown));
        fake.events.push_back(keyEvent(kVkDown));
        fake.events.push_back(keyEvent(kVkUp));
        fake.events.push_back(keyEvent(0, L' ')); // toggles index 1
        fake.events.push_back(keyEvent(kVkReturn));

        TerminalSession session = makeSession(fake);
        ChecklistModel model(
            {makeCandidate(L"a.exe"), makeCandidate(L"b.exe"), makeCandidate(L"c.exe")});

        ChecklistRunResult result = runChecklist(session, model);

        Assert::AreEqual(size_t{1}, result.selectedCandidates.size());
        Assert::AreEqual(std::wstring(L"b.exe"), result.selectedCandidates[0].item.alias);
    }

    TEST_METHOD(resizeEventIsAppliedWithoutEndingTheSession)
    {
        ScriptedTerminal fake;
        fake.events.push_back(resizeEvent(20, 100));
        fake.events.push_back(keyEvent(kVkReturn));

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"a.exe")});

        ChecklistRunResult result = runChecklist(session, model);
        Assert::IsTrue(result.outcome == ChecklistOutcome::Confirmed);
    }

    TEST_METHOD(readFailureIsTreatedAsCancellation)
    {
        // No scripted events at all - the first readEvent() call returns nullopt,
        // simulating a dead input source.
        ScriptedTerminal fake;

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"a.exe")});

        ChecklistRunResult result = runChecklist(session, model);
        Assert::IsTrue(result.outcome == ChecklistOutcome::Cancelled);
    }

    TEST_METHOD(renderedFrameContainsSanitizedAliasText)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(kVkReturn));

        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(L"tool.exe")});

        static_cast<void>(runChecklist(session, model));

        Assert::IsFalse(fake.controlWrites.empty());
        bool found = false;
        for (const std::wstring& write : fake.controlWrites)
        {
            if (write.find(L"tool.exe") != std::wstring::npos)
            {
                found = true;
                break;
            }
        }
        Assert::IsTrue(found);
    }

    // The case above alone would still pass if render() stopped sanitizing entirely -
    // "tool.exe" has nothing for cli::sanitizeForDisplay() to strip. This case uses an
    // alias carrying an embedded ESC byte (as an untrusted package/executable name
    // could) and asserts the *sanitized* form is what actually reaches the terminal,
    // while the raw, ESC-carrying form - which could otherwise forge a fake ANSI
    // color/control sequence into the checklist's own intentional output - never does.
    TEST_METHOD(renderedFrameSanitizesCandidateTextBeforeCombiningWithControlSequences)
    {
        ScriptedTerminal fake;
        fake.events.push_back(keyEvent(kVkReturn));

        const std::wstring maliciousAlias =
            std::wstring(L"tool") + wchar_t(0x1B) + L"[31mFAKE.exe";
        TerminalSession session = makeSession(fake);
        ChecklistModel model({makeCandidate(maliciousAlias)});

        static_cast<void>(runChecklist(session, model));

        bool sanitizedFormFound = false;
        bool rawFormFound = false;
        for (const std::wstring& write : fake.controlWrites)
        {
            if (write.find(L"tool[31mFAKE.exe") != std::wstring::npos)
            {
                sanitizedFormFound = true;
            }
            if (write.find(maliciousAlias) != std::wstring::npos)
            {
                rawFormFound = true;
            }
        }
        Assert::IsTrue(sanitizedFormFound);
        Assert::IsFalse(rawFormFound);
    }
};
} // namespace syncwingetlink::tests
