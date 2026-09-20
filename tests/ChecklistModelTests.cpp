// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <tui/ChecklistModel.h>

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

// A Mismatch row: shown so the user knows it is there, never selectable, because
// repairLink() always refuses a Mismatch (docs/adr-phase-10.md ADR-0047).
[[nodiscard]] ChecklistCandidate makeUnselectableCandidate(const std::wstring& alias)
{
    ChecklistCandidate candidate = makeCandidate(alias);
    candidate.item.status = LinkStatus::Mismatch;
    candidate.selectable = false;
    return candidate;
}

[[nodiscard]] std::vector<ChecklistCandidate> makeCandidates(std::size_t count)
{
    std::vector<ChecklistCandidate> candidates;
    candidates.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        candidates.push_back(makeCandidate(L"tool" + std::to_wstring(i) + L".exe"));
    }
    return candidates;
}
} // namespace

TEST_CLASS(ChecklistModelInitialStateTests)
{
public:
    TEST_METHOD(everyCandidateStartsUnselected)
    {
        ChecklistModel model(makeCandidates(3));
        Assert::IsFalse(model.isSelected(0));
        Assert::IsFalse(model.isSelected(1));
        Assert::IsFalse(model.isSelected(2));
    }

    TEST_METHOD(cursorStartsAtZero)
    {
        ChecklistModel model(makeCandidates(3));
        Assert::AreEqual(size_t{0}, model.cursor());
    }

    TEST_METHOD(neitherConfirmedNorCancelledInitially)
    {
        ChecklistModel model(makeCandidates(1));
        Assert::IsFalse(model.wasConfirmed());
        Assert::IsFalse(model.wasCancelled());
    }

    TEST_METHOD(emptyCandidatesIsTolerated)
    {
        ChecklistModel model(std::vector<ChecklistCandidate>{});
        model.moveUp();
        model.moveDown();
        model.toggleCurrent();
        Assert::AreEqual(size_t{0}, model.cursor());
        Assert::IsTrue(model.confirm().empty());
    }
};

TEST_CLASS(ChecklistModelNavigationTests)
{
public:
    TEST_METHOD(moveDownAdvancesCursor)
    {
        ChecklistModel model(makeCandidates(3));
        model.moveDown();
        Assert::AreEqual(size_t{1}, model.cursor());
    }

    TEST_METHOD(moveDownNeverWrapsPastLastCandidate)
    {
        ChecklistModel model(makeCandidates(2));
        model.moveDown();
        model.moveDown();
        model.moveDown();
        Assert::AreEqual(size_t{1}, model.cursor());
    }

    TEST_METHOD(moveUpNeverWrapsPastFirstCandidate)
    {
        ChecklistModel model(makeCandidates(3));
        model.moveUp();
        model.moveUp();
        Assert::AreEqual(size_t{0}, model.cursor());
    }

    TEST_METHOD(moveUpAndDownRoundTrip)
    {
        ChecklistModel model(makeCandidates(3));
        model.moveDown();
        model.moveDown();
        model.moveUp();
        Assert::AreEqual(size_t{1}, model.cursor());
    }
};

TEST_CLASS(ChecklistModelSelectionTests)
{
public:
    TEST_METHOD(toggleCurrentSelectsThenDeselects)
    {
        ChecklistModel model(makeCandidates(2));
        model.toggleCurrent();
        Assert::IsTrue(model.isSelected(0));
        model.toggleCurrent();
        Assert::IsFalse(model.isSelected(0));
    }

    TEST_METHOD(toggleOnlyAffectsCandidateUnderCursor)
    {
        ChecklistModel model(makeCandidates(3));
        model.moveDown();
        model.toggleCurrent();
        Assert::IsFalse(model.isSelected(0));
        Assert::IsTrue(model.isSelected(1));
        Assert::IsFalse(model.isSelected(2));
    }

    TEST_METHOD(confirmReturnsOnlySelectedCandidatesInAscendingOrder)
    {
        ChecklistModel model(makeCandidates(4));
        model.moveDown();
        model.toggleCurrent(); // select index 1
        model.moveDown();
        model.moveDown();
        model.toggleCurrent(); // select index 3

        const std::vector<ChecklistCandidate> selected = model.confirm();

        Assert::AreEqual(size_t{2}, selected.size());
        Assert::AreEqual(std::wstring(L"tool1.exe"), selected[0].item.alias);
        Assert::AreEqual(std::wstring(L"tool3.exe"), selected[1].item.alias);
        Assert::IsTrue(model.wasConfirmed());
    }

    TEST_METHOD(confirmWithNoSelectionReturnsEmptyButStillConfirmed)
    {
        ChecklistModel model(makeCandidates(3));
        const std::vector<ChecklistCandidate> selected = model.confirm();
        Assert::IsTrue(selected.empty());
        Assert::IsTrue(model.wasConfirmed());
    }

    TEST_METHOD(cancelLeavesSelectionStateInspectableButMarksCancelled)
    {
        ChecklistModel model(makeCandidates(2));
        model.toggleCurrent();
        model.cancel();

        Assert::IsTrue(model.wasCancelled());
        Assert::IsFalse(model.wasConfirmed());
        // Selection state itself is untouched by cancel() - only the caller's decision
        // to discard it (never calling confirm()) is what actually prevents a repair.
        Assert::IsTrue(model.isSelected(0));
    }
};

TEST_CLASS(ChecklistModelViewportTests)
{
public:
    TEST_METHOD(viewportFitsEntireListWhenTallEnough)
    {
        ChecklistModel model(makeCandidates(3));
        model.resize(10);
        Assert::AreEqual(size_t{0}, model.viewportStart());
        Assert::AreEqual(size_t{3}, model.viewportCount());
    }

    TEST_METHOD(zeroHeightShowsNothingWithoutLosingState)
    {
        ChecklistModel model(makeCandidates(3));
        model.moveDown();
        model.toggleCurrent();
        model.resize(0);

        Assert::AreEqual(size_t{0}, model.viewportCount());
        Assert::AreEqual(size_t{1}, model.cursor());
        Assert::IsTrue(model.isSelected(1));
    }

    TEST_METHOD(scrollingDownFollowsCursorPastTheBottomOfTheViewport)
    {
        ChecklistModel model(makeCandidates(5));
        model.resize(2);
        Assert::AreEqual(size_t{0}, model.viewportStart());

        model.moveDown();
        model.moveDown();
        // Cursor is now at index 2, past the initial [0,2) viewport - it must have
        // scrolled to keep the cursor visible.
        Assert::AreEqual(size_t{2}, model.cursor());
        Assert::IsTrue(model.cursor() >= model.viewportStart());
        Assert::IsTrue(model.cursor() < model.viewportStart() + model.viewportCount());
    }

    TEST_METHOD(scrollingUpFollowsCursorAboveTheTopOfTheViewport)
    {
        ChecklistModel model(makeCandidates(5));
        model.resize(2);
        model.moveDown();
        model.moveDown();
        model.moveDown();
        model.moveDown(); // cursor == 4, viewport scrolled down to keep it visible

        model.moveUp();
        model.moveUp();
        model.moveUp();
        model.moveUp(); // cursor == 0 again

        Assert::AreEqual(size_t{0}, model.cursor());
        Assert::AreEqual(size_t{0}, model.viewportStart());
    }

    TEST_METHOD(resizeRecomputesViewportWithoutMovingCursorOrLosingSelection)
    {
        ChecklistModel model(makeCandidates(6));
        model.resize(2);
        model.moveDown();
        model.moveDown();
        model.moveDown(); // cursor == 3, scrolled
        model.toggleCurrent();

        model.resize(4); // grow the viewport - selection/cursor must survive

        Assert::AreEqual(size_t{3}, model.cursor());
        Assert::IsTrue(model.isSelected(3));
        Assert::IsTrue(model.cursor() >= model.viewportStart());
        Assert::IsTrue(model.cursor() < model.viewportStart() + model.viewportCount());
    }

    TEST_METHOD(viewportNeverScrollsPastTheLastFullPage)
    {
        ChecklistModel model(makeCandidates(5));
        model.resize(3);
        model.moveDown();
        model.moveDown();
        model.moveDown();
        model.moveDown(); // cursor == 4 (last candidate)

        // With 5 candidates and a 3-row viewport, the last valid viewportStart is 2
        // (rows 2,3,4) - it must never scroll to e.g. 4, which would show fewer rows
        // than the viewport actually has room for.
        Assert::AreEqual(size_t{2}, model.viewportStart());
        Assert::AreEqual(size_t{3}, model.viewportCount());
    }
};

// docs/adr-phase-10.md ADR-0047: a Mismatch candidate is listed for information only.
TEST_CLASS(ChecklistModelUnselectableCandidateTests)
{
public:
    TEST_METHOD(hasSelectableIsFalseWhenEveryCandidateIsUnselectable)
    {
        std::vector<ChecklistCandidate> candidates;
        candidates.push_back(makeUnselectableCandidate(L"copilot.exe"));
        candidates.push_back(makeUnselectableCandidate(L"gh.exe"));
        ChecklistModel model(std::move(candidates));

        Assert::IsFalse(model.hasSelectable());
    }

    TEST_METHOD(hasSelectableIsTrueWhenAtLeastOneCandidateIsSelectable)
    {
        std::vector<ChecklistCandidate> candidates;
        candidates.push_back(makeUnselectableCandidate(L"copilot.exe"));
        candidates.push_back(makeCandidate(L"codex.exe"));
        ChecklistModel model(std::move(candidates));

        Assert::IsTrue(model.hasSelectable());
    }

    TEST_METHOD(hasSelectableIsFalseForAnEmptyModel)
    {
        ChecklistModel model({});
        Assert::IsFalse(model.hasSelectable());
    }

    TEST_METHOD(togglingAnUnselectableCandidateIsANoOp)
    {
        std::vector<ChecklistCandidate> candidates;
        candidates.push_back(makeUnselectableCandidate(L"copilot.exe"));
        ChecklistModel model(std::move(candidates));

        model.toggleCurrent();
        Assert::IsFalse(model.isSelected(0));

        // Repeated presses must not flip it either - the guard is on the candidate, not
        // on some "already toggled once" state.
        model.toggleCurrent();
        Assert::IsFalse(model.isSelected(0));
    }

    TEST_METHOD(confirmNeverReturnsAnUnselectableCandidate)
    {
        std::vector<ChecklistCandidate> candidates;
        candidates.push_back(makeUnselectableCandidate(L"copilot.exe"));
        candidates.push_back(makeCandidate(L"codex.exe"));
        ChecklistModel model(std::move(candidates));

        model.toggleCurrent(); // no-op on the Mismatch row
        model.moveDown();
        model.toggleCurrent(); // selects codex.exe

        const std::vector<ChecklistCandidate> selected = model.confirm();
        Assert::AreEqual(size_t{1}, selected.size());
        Assert::AreEqual(std::wstring(L"codex.exe"), selected[0].item.alias);
    }

    TEST_METHOD(confirmIsEmptyWhenNothingIsSelectable)
    {
        std::vector<ChecklistCandidate> candidates;
        candidates.push_back(makeUnselectableCandidate(L"copilot.exe"));
        ChecklistModel model(std::move(candidates));

        model.toggleCurrent();
        const std::vector<ChecklistCandidate> selected = model.confirm();

        Assert::IsTrue(selected.empty());
        Assert::IsTrue(model.wasConfirmed());
    }

    TEST_METHOD(theCursorStillRestsOnAnUnselectableCandidate)
    {
        // Non-selectable rows are not skipped: the cursor must be able to sit on one so
        // its target path can be read.
        std::vector<ChecklistCandidate> candidates;
        candidates.push_back(makeCandidate(L"codex.exe"));
        candidates.push_back(makeUnselectableCandidate(L"copilot.exe"));
        candidates.push_back(makeCandidate(L"uv.exe"));
        ChecklistModel model(std::move(candidates));

        model.moveDown();
        Assert::AreEqual(size_t{1}, model.cursor());
        model.moveDown();
        Assert::AreEqual(size_t{2}, model.cursor());
    }

    TEST_METHOD(unselectableCandidatesStillOccupyTheViewport)
    {
        std::vector<ChecklistCandidate> candidates;
        candidates.push_back(makeCandidate(L"codex.exe"));
        candidates.push_back(makeUnselectableCandidate(L"copilot.exe"));
        candidates.push_back(makeUnselectableCandidate(L"gh.exe"));
        candidates.push_back(makeCandidate(L"uv.exe"));
        ChecklistModel model(std::move(candidates));
        model.resize(2);

        model.moveDown();
        model.moveDown();
        model.moveDown(); // cursor == 3

        Assert::AreEqual(size_t{2}, model.viewportStart());
        Assert::AreEqual(size_t{2}, model.viewportCount());
    }
};
} // namespace syncwingetlink::tests
