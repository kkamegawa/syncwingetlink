// SPDX-License-Identifier: MIT

#pragma once

#include "Console.h"
#include "core/Model.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace syncwingetlink::cli
{
// One already-formatted output line plus the importance Console::writeLine must use to
// emit it (see MessageImportance, Console.h).
struct ReportLine
{
    std::wstring text;
    MessageImportance importance{MessageImportance::Normal};
};

// Which command is asking for the grouped report - controls how load-bearing its lines
// are under --quiet (docs/adr-phase-6.md ADR-0030).
enum class ReportMode
{
    // scan's own result report: NG (Missing/Broken/Mismatch) rows are the actionable
    // output and stay Normal (when at least one exists); the OK table, and the NG
    // section when there are zero NG items, are Supplementary.
    Scan,
    // fix's pre-batch preview of what it is about to consider: informational only, so
    // every line is Supplementary regardless of NG/OK.
    FixPreview,
};

// Display text for a LinkStatus: "Ok"/"Missing"/"Broken"/"Mismatch", or "Unknown" for
// any future enumerator this switch hasn't been updated to cover. The single shared
// definition for both the grouped report's status column and Dispatch.cpp's fix consent
// prompt, so the two can never silently diverge on wording.
[[nodiscard]] std::wstring_view linkStatusDisplayName(LinkStatus status) noexcept;

// Terminal column width of already-sanitized text (call sanitizeForDisplay() first - this
// function does not sanitize). A UTF-16 surrogate pair counts as one code point; a code
// point in a commonly wide East Asian block (CJK ideographs, Hangul syllables, fullwidth
// forms, common emoji ranges, ...) counts as 2 columns, everything else as 1. Best-effort
// and deliberately incomplete - combining marks, emoji ZWJ sequences, and variation
// selectors are not modeled - a miscount only misaligns a report column; it never affects
// the data actually displayed (sanitizeForDisplay() already handles display *safety*
// separately).
[[nodiscard]] std::size_t displayWidth(std::wstring_view text) noexcept;

// Splits items into the NG group (LinkStatus::Missing/Broken/Mismatch) and the OK group
// (LinkStatus::Ok), and renders both as an aligned "package | status | alias | target"
// table, NG always first. Within a group, rows are sorted by alias (ordinal
// case-insensitive ascending), tie-broken by executable path (same comparison) for
// alias-collision candidates that share an alias. Column widths (package/status/alias;
// target is the last column and is never padded) are computed once across every row in
// both groups, so the NG and OK tables always line up. A group with zero items renders
// as just its heading followed by a bare "nothing" line instead of an empty table. Every
// cell is passed through sanitizeForDisplay() before measurement and rendering; an empty
// packageId renders as "-".
[[nodiscard]] std::vector<ReportLine> formatGroupedReport(std::span<const RepairItem> items,
                                                           ReportMode mode);
} // namespace syncwingetlink::cli
