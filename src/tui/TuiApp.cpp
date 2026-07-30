// SPDX-License-Identifier: MIT

#include "TuiApp.h"

// sanitizeForDisplay() is cli::Console's, reused here rather than duplicated -
// AGENTS.md's cli//tui split names both as thin presentation layers over core/; this
// is the one place tui/ depends on cli/ instead of the reverse (Dispatch.cpp, in cli/,
// already depends on core/ extensively). Both live in the same syncwingetlink.core
// static library, so this is an ordinary intra-library include, not a layering
// violation.
#include "cli/Console.h"

#include <Windows.h>

#include <string>
#include <string_view>
#include <variant>

namespace syncwingetlink::tui
{
namespace
{
// Rows the renderer reserves for its own instructions above the candidate list - the
// viewport height ChecklistModel::resize() receives is the console's visible row
// count minus this, never the raw value TuiResizeEvent reports.
constexpr std::size_t kReservedRows = 2;

[[nodiscard]] std::wstring_view statusLabel(LinkStatus status) noexcept
{
    switch (status)
    {
    case LinkStatus::Missing:
        return L"Missing";
    case LinkStatus::Broken:
        return L"Broken";
    case LinkStatus::Ok:
        return L"Ok";
    case LinkStatus::Mismatch:
        return L"Mismatch";
    }
    return L"Unknown";
}

[[nodiscard]] std::size_t usableHeight(std::size_t rows) noexcept
{
    return rows > kReservedRows ? rows - kReservedRows : 0;
}

// Builds and writes one full frame. Redrawing the whole viewport on every state change
// (rather than diffing) is deliberate: this is a modal checklist with at most a few
// dozen visible rows, not a high-frequency renderer, and a full redraw can never leave
// a stale line from a previous, differently-sized frame on screen.
void render(TerminalSession& session, const ChecklistModel& model)
{
    std::wstring frame;
    frame += L"\x1b[2J\x1b[H"; // Clear screen, home cursor.
    frame += L"Space: toggle  Up/Down: move  Enter: repair selected  Esc/Q/Ctrl+C: cancel\r\n";
    frame += L"\r\n";

    const std::size_t start = model.viewportStart();
    const std::size_t count = model.viewportCount();
    for (std::size_t index = start; index < start + count; ++index)
    {
        const ChecklistCandidate& candidate = model.candidates()[index];

        frame += (index == model.cursor()) ? L"> " : L"  ";
        frame += model.isSelected(index) ? L"[x] " : L"[ ] ";
        frame += L"(";
        frame += statusLabel(candidate.item.status);
        frame += L") ";
        frame += cli::sanitizeForDisplay(candidate.item.alias);
        frame += L" -> ";
        frame += cli::sanitizeForDisplay(candidate.item.executable.path.native());
        frame += L"\r\n";
    }

    session.writeControl(frame);
}
} // namespace

ChecklistRunResult runChecklist(TerminalSession& session, ChecklistModel& model)
{
    if (const std::optional<TuiResizeEvent> initial = session.queryViewport();
        initial.has_value())
    {
        model.resize(usableHeight(initial->rows));
    }

    render(session, model);

    ChecklistRunResult result;

    for (;;)
    {
        const std::optional<TuiEvent> event = session.readEvent();
        if (!event.has_value())
        {
            model.cancel();
            result.outcome = ChecklistOutcome::Cancelled;
            return result;
        }

        if (std::holds_alternative<TuiResizeEvent>(*event))
        {
            model.resize(usableHeight(std::get<TuiResizeEvent>(*event).rows));
            render(session, model);
            continue;
        }

        const TuiKeyEvent& key = std::get<TuiKeyEvent>(*event);

        // Ctrl+C: see TuiKeyEvent's documentation - this arrives as an ordinary key
        // event, not a SetConsoleCtrlHandler callback, because the session clears
        // ENABLE_PROCESSED_INPUT for exactly this reason.
        if (key.ctrlPressed && key.virtualKeyCode == 'C')
        {
            model.cancel();
            result.outcome = ChecklistOutcome::Cancelled;
            return result;
        }
        if (key.virtualKeyCode == VK_ESCAPE || key.character == L'q' || key.character == L'Q')
        {
            model.cancel();
            result.outcome = ChecklistOutcome::Cancelled;
            return result;
        }
        if (key.virtualKeyCode == VK_RETURN)
        {
            result.selectedCandidates = model.confirm();
            result.outcome = ChecklistOutcome::Confirmed;
            return result;
        }

        if (key.virtualKeyCode == VK_UP)
        {
            model.moveUp();
        }
        else if (key.virtualKeyCode == VK_DOWN)
        {
            model.moveDown();
        }
        else if (key.character == L' ')
        {
            model.toggleCurrent();
        }
        // Any other key is ignored - the checklist has no other bindings.

        render(session, model);
    }
}
} // namespace syncwingetlink::tui
