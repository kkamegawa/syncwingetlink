// SPDX-License-Identifier: MIT

#pragma once

#include "core/Model.h"

#include <cstddef>
#include <vector>

namespace syncwingetlink::tui
{
// A checklist candidate. Wraps RepairItem directly rather than introducing a parallel
// display-only type - the model needs nothing beyond what the existing repair pipeline
// (core/LinkInspector.h, core/SymlinkService.h) already produces for a candidate.
struct ChecklistCandidate
{
    RepairItem item;
};

// Pure, Win32-free selection-state model for the M7 repair checklist. Holds cursor
// position, the set of selected items, and the scrolling viewport. `cli::Dispatch`
// (#59) constructs one from the non-colliding Missing/Broken subset of a scan's
// results and drives it from key/resize events read through a tui::TerminalSession;
// `tui::runChecklist()` (TuiApp.h) is the thin renderer/input loop built on top of both.
// Having no Win32 dependency here keeps every state transition unit-testable without a
// real console.
class ChecklistModel
{
public:
    // Every selectable candidate starts unchecked - no candidate is ever pre-selected.
    // An empty candidates vector is tolerated (every mutating method below becomes a
    // no-op, confirm() returns empty) rather than asserted against, since a defensive
    // caller may construct one before checking whether there was anything to select.
    explicit ChecklistModel(std::vector<ChecklistCandidate> candidates);

    [[nodiscard]] const std::vector<ChecklistCandidate>& candidates() const noexcept
    {
        return m_candidates;
    }

    [[nodiscard]] std::size_t cursor() const noexcept
    {
        return m_cursor;
    }

    [[nodiscard]] bool isSelected(std::size_t index) const noexcept;

    // Move the cursor by one, clamped to [0, candidates().size()-1] - never wraps
    // around either end. No-op when candidates() is empty.
    void moveUp() noexcept;
    void moveDown() noexcept;

    // Toggles the selection state of the candidate currently under the cursor. No-op
    // when candidates() is empty.
    void toggleCurrent() noexcept;

    // Recomputes the scrolling viewport for a new visible height (the number of rows
    // the renderer has available to list candidates in, after reserving space for its
    // own header/footer text) without losing the cursor position or any existing
    // selection - a resize event only ever changes what is visible, never what is
    // selected or where the cursor sits logically. height == 0 is tolerated
    // (viewportCount() becomes 0 - nothing visible - rather than treated as an error).
    void resize(std::size_t height) noexcept;

    // The half-open range [viewportStart(), viewportStart()+viewportCount()) of
    // candidates() indices the renderer should currently draw. This class does no
    // drawing itself.
    [[nodiscard]] std::size_t viewportStart() const noexcept
    {
        return m_viewportStart;
    }
    [[nodiscard]] std::size_t viewportCount() const noexcept;

    // Returns every selected candidate, in ascending index order, and marks the model
    // confirmed. Enter with no selected items is a successful no-op: this returns an
    // empty vector, not an error - the caller performs no repairs for an empty batch,
    // but still reports success.
    [[nodiscard]] std::vector<ChecklistCandidate> confirm();

    // Marks the model cancelled. The caller performs no repairs regardless of what had
    // been selected up to this point - selection state itself is left untouched
    // (inspectable for tests), only wasCancelled() distinguishes this from confirm().
    void cancel() noexcept;

    [[nodiscard]] bool wasCancelled() const noexcept
    {
        return m_cancelled;
    }
    [[nodiscard]] bool wasConfirmed() const noexcept
    {
        return m_confirmed;
    }

private:
    void clampViewportToCursor() noexcept;

    std::vector<ChecklistCandidate> m_candidates;
    std::vector<bool> m_selected;
    std::size_t m_cursor{0};
    std::size_t m_viewportHeight{0};
    std::size_t m_viewportStart{0};
    bool m_confirmed{false};
    bool m_cancelled{false};
};
} // namespace syncwingetlink::tui
