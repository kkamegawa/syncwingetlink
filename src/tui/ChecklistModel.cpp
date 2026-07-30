// SPDX-License-Identifier: MIT

#include "ChecklistModel.h"

#include <algorithm>
#include <utility>

namespace syncwingetlink::tui
{
ChecklistModel::ChecklistModel(std::vector<ChecklistCandidate> candidates)
    : m_candidates(std::move(candidates)), m_selected(m_candidates.size(), false)
{
}

bool ChecklistModel::isSelected(std::size_t index) const noexcept
{
    return index < m_selected.size() && m_selected[index];
}

void ChecklistModel::moveUp() noexcept
{
    if (m_candidates.empty() || m_cursor == 0)
    {
        return;
    }
    --m_cursor;
    clampViewportToCursor();
}

void ChecklistModel::moveDown() noexcept
{
    if (m_candidates.empty() || m_cursor + 1 >= m_candidates.size())
    {
        return;
    }
    ++m_cursor;
    clampViewportToCursor();
}

void ChecklistModel::toggleCurrent() noexcept
{
    if (m_cursor < m_selected.size())
    {
        m_selected[m_cursor] = !m_selected[m_cursor];
    }
}

void ChecklistModel::resize(std::size_t height) noexcept
{
    m_viewportHeight = height;
    clampViewportToCursor();
}

std::size_t ChecklistModel::viewportCount() const noexcept
{
    if (m_viewportHeight == 0 || m_candidates.empty())
    {
        return 0;
    }
    return std::min(m_viewportHeight, m_candidates.size() - m_viewportStart);
}

void ChecklistModel::clampViewportToCursor() noexcept
{
    if (m_viewportHeight == 0 || m_candidates.empty())
    {
        m_viewportStart = 0;
        return;
    }

    // Scroll up if the cursor moved above the current viewport...
    if (m_viewportStart > m_cursor)
    {
        m_viewportStart = m_cursor;
    }
    // ...or down if it moved below it.
    else if (m_cursor >= m_viewportStart + m_viewportHeight)
    {
        m_viewportStart = m_cursor - m_viewportHeight + 1;
    }

    // A resize to a taller viewport (or a shorter candidate list) can leave
    // m_viewportStart further from the end than the remaining candidates justify -
    // pull it back so the list never scrolls past its own last full page, and never
    // scrolls at all once every candidate already fits.
    if (m_candidates.size() <= m_viewportHeight)
    {
        m_viewportStart = 0;
    }
    else if (m_viewportStart > m_candidates.size() - m_viewportHeight)
    {
        m_viewportStart = m_candidates.size() - m_viewportHeight;
    }
}

std::vector<ChecklistCandidate> ChecklistModel::confirm()
{
    m_confirmed = true;

    std::vector<ChecklistCandidate> selected;
    for (std::size_t index = 0; index < m_candidates.size(); ++index)
    {
        if (isSelected(index))
        {
            selected.push_back(m_candidates[index]);
        }
    }
    return selected;
}

void ChecklistModel::cancel() noexcept
{
    m_cancelled = true;
}
} // namespace syncwingetlink::tui
