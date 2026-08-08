// SPDX-License-Identifier: MIT

#include "ScanReport.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>, which is where CompareStringOrdinal lives via <winnls.h>
// (pulled in transitively) - see core/LinkInspector.cpp's compareOrdinalCaseInsensitive
// for the same pattern.
#include <Windows.h>

#include <algorithm>
#include <format>
#include <utility>

namespace syncwingetlink::cli
{
namespace
{
constexpr std::wstring_view kPackageHeader = L"package";
constexpr std::wstring_view kStatusHeader = L"status";
constexpr std::wstring_view kAliasHeader = L"alias";
constexpr std::wstring_view kTargetHeader = L"target";
constexpr std::wstring_view kEmptyPackageId = L"-";

// Duplicated from cli::Dispatch.cpp's file-local function of the same name (and
// behavior) rather than shared, to keep this translation unit free of a dependency on
// Dispatch.cpp's anonymous namespace. Both are internal linkage, so there is no ODR
// conflict.
[[nodiscard]] std::wstring_view linkStatusDisplayName(LinkStatus status) noexcept
{
    switch (status)
    {
    case LinkStatus::Ok:
        return L"Ok";
    case LinkStatus::Missing:
        return L"Missing";
    case LinkStatus::Broken:
        return L"Broken";
    case LinkStatus::Mismatch:
        return L"Mismatch";
    }
    return L"Unknown";
}

[[nodiscard]] bool isNgStatus(LinkStatus status) noexcept
{
    return status != LinkStatus::Ok;
}

// Best-effort East Asian Wide/Fullwidth code point ranges - not exhaustive (see
// displayWidth()'s doc comment in ScanReport.h). Modeled after the commonly used East
// Asian Width "W"/"F" categories: Hangul Jamo, CJK radicals/punctuation/ideographs,
// Yi, Hangul syllables, CJK compatibility ideographs/forms, fullwidth forms/signs, and
// the common emoji and CJK-extension supplementary-plane ranges.
[[nodiscard]] bool isWideCodePoint(char32_t codePoint) noexcept
{
    return (codePoint >= 0x1100 && codePoint <= 0x115F) ||
           (codePoint >= 0x2E80 && codePoint <= 0x303E) ||
           (codePoint >= 0x3041 && codePoint <= 0x33FF) ||
           (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
           (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
           (codePoint >= 0xA000 && codePoint <= 0xA4CF) ||
           (codePoint >= 0xAC00 && codePoint <= 0xD7A3) ||
           (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
           (codePoint >= 0xFE30 && codePoint <= 0xFE4F) ||
           (codePoint >= 0xFF00 && codePoint <= 0xFF60) ||
           (codePoint >= 0xFFE0 && codePoint <= 0xFFE6) ||
           (codePoint >= 0x1F300 && codePoint <= 0x1FAFF) ||
           (codePoint >= 0x20000 && codePoint <= 0x3FFFD);
}

[[nodiscard]] int compareOrdinalCaseInsensitive(std::wstring_view a, std::wstring_view b) noexcept
{
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                  static_cast<int>(b.size()), TRUE);
}

[[nodiscard]] std::wstring padToWidth(std::wstring_view text, std::size_t width)
{
    std::wstring result(text);
    const std::size_t currentWidth = displayWidth(text);
    if (currentWidth < width)
    {
        result.append(width - currentWidth, L' ');
    }
    return result;
}

// One row's already-sanitized cell text, ready for measurement and rendering.
struct ReportRow
{
    std::wstring packageId;
    std::wstring status;
    std::wstring alias;
    std::wstring target;
};

[[nodiscard]] ReportRow toReportRow(const RepairItem& item)
{
    std::wstring packageId = sanitizeForDisplay(item.packageId);
    if (packageId.empty())
    {
        packageId = std::wstring(kEmptyPackageId);
    }
    return ReportRow{
        std::move(packageId),
        std::wstring(linkStatusDisplayName(item.status)),
        sanitizeForDisplay(item.alias),
        sanitizeForDisplay(item.executable.path.native()),
    };
}

// Ordinal case-insensitive by alias, tie-broken by executable path (same comparison) -
// a stable, deterministic ordering even for candidates that share an alias (an alias
// collision, core/LinkInspector.h detectAliasCollisions()).
[[nodiscard]] bool lessByAliasThenPath(const RepairItem& a, const RepairItem& b) noexcept
{
    const int aliasComparison = compareOrdinalCaseInsensitive(a.alias, b.alias);
    if (aliasComparison != CSTR_EQUAL)
    {
        return aliasComparison == CSTR_LESS_THAN;
    }
    return compareOrdinalCaseInsensitive(a.executable.path.native(),
                                         b.executable.path.native()) == CSTR_LESS_THAN;
}

struct ColumnWidths
{
    std::size_t packageWidth;
    std::size_t statusWidth;
    std::size_t aliasWidth;
    std::size_t targetWidth;
};

// The rendered width of one "package | status | alias | target" line: each of the
// three " | " separators is 3 columns wide.
[[nodiscard]] std::size_t totalTableWidth(const ColumnWidths& widths) noexcept
{
    return widths.packageWidth + widths.statusWidth + widths.aliasWidth + widths.targetWidth +
           (3 * 3);
}

void appendTableHeader(std::vector<ReportLine>& lines, const ColumnWidths& widths,
                       MessageImportance importance)
{
    const std::wstring separator(totalTableWidth(widths), L'-');
    lines.push_back(ReportLine{separator, importance});
    lines.push_back(ReportLine{
        std::format(L"{} | {} | {} | {}", padToWidth(kPackageHeader, widths.packageWidth),
                    padToWidth(kStatusHeader, widths.statusWidth),
                    padToWidth(kAliasHeader, widths.aliasWidth), kTargetHeader),
        importance});
    lines.push_back(ReportLine{separator, importance});
}

void appendTableRows(std::vector<ReportLine>& lines, const std::vector<ReportRow>& rows,
                     const ColumnWidths& widths, MessageImportance importance)
{
    for (const ReportRow& row : rows)
    {
        lines.push_back(ReportLine{
            std::format(L"{} | {} | {} | {}", padToWidth(row.packageId, widths.packageWidth),
                        padToWidth(row.status, widths.statusWidth),
                        padToWidth(row.alias, widths.aliasWidth), row.target),
            importance});
    }
    lines.push_back(ReportLine{std::wstring(totalTableWidth(widths), L'-'), importance});
}

void appendGroup(std::vector<ReportLine>& lines, std::wstring_view heading,
                 const std::vector<ReportRow>& rows, const ColumnWidths& widths,
                 MessageImportance headingImportance, MessageImportance bodyImportance)
{
    lines.push_back(ReportLine{std::wstring(heading), headingImportance});
    if (rows.empty())
    {
        lines.push_back(ReportLine{L"nothing", headingImportance});
        return;
    }

    appendTableHeader(lines, widths, bodyImportance);
    appendTableRows(lines, rows, widths, bodyImportance);
}
} // namespace

std::size_t displayWidth(std::wstring_view text) noexcept
{
    std::size_t width = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        const wchar_t unit = text[index];
        char32_t codePoint = unit;
        std::size_t advance = 1;
        if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < text.size())
        {
            const wchar_t low = text[index + 1];
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                codePoint = 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) +
                            (static_cast<char32_t>(low) - 0xDC00);
                advance = 2;
            }
        }
        width += isWideCodePoint(codePoint) ? 2 : 1;
        index += advance;
    }
    return width;
}

std::vector<ReportLine> formatGroupedReport(std::span<const RepairItem> items, ReportMode mode)
{
    std::vector<ReportRow> ngRows;
    std::vector<ReportRow> okRows;

    {
        std::vector<const RepairItem*> ngItems;
        std::vector<const RepairItem*> okItems;
        for (const RepairItem& item : items)
        {
            (isNgStatus(item.status) ? ngItems : okItems).push_back(&item);
        }

        const auto byAliasThenPath = [](const RepairItem* a, const RepairItem* b) {
            return lessByAliasThenPath(*a, *b);
        };
        std::sort(ngItems.begin(), ngItems.end(), byAliasThenPath);
        std::sort(okItems.begin(), okItems.end(), byAliasThenPath);

        ngRows.reserve(ngItems.size());
        for (const RepairItem* item : ngItems)
        {
            ngRows.push_back(toReportRow(*item));
        }
        okRows.reserve(okItems.size());
        for (const RepairItem* item : okItems)
        {
            okRows.push_back(toReportRow(*item));
        }
    }

    ColumnWidths widths{displayWidth(kPackageHeader), displayWidth(kStatusHeader),
                        displayWidth(kAliasHeader), displayWidth(kTargetHeader)};
    for (const std::vector<ReportRow>* group : {&ngRows, &okRows})
    {
        for (const ReportRow& row : *group)
        {
            widths.packageWidth = std::max(widths.packageWidth, displayWidth(row.packageId));
            widths.statusWidth = std::max(widths.statusWidth, displayWidth(row.status));
            widths.aliasWidth = std::max(widths.aliasWidth, displayWidth(row.alias));
            widths.targetWidth = std::max(widths.targetWidth, displayWidth(row.target));
        }
    }

    // scan: NG is actionable, so its heading/table stay Normal whenever at least one NG
    // item exists; a zero-NG "NG"/"nothing" pair is routine confirmation, same as OK,
    // and drops to Supplementary. fix's preview is informational end to end, regardless
    // of NG/OK (docs/adr-phase-6.md ADR-0030's Supplementary/Normal split is a scan-only
    // concept; the preview never gates anything a user must act on before fix's own
    // per-item consent prompts do).
    const MessageImportance ngHeadingImportance = (mode == ReportMode::Scan && !ngRows.empty())
                                                       ? MessageImportance::Normal
                                                       : MessageImportance::Supplementary;
    const MessageImportance ngBodyImportance =
        mode == ReportMode::Scan ? MessageImportance::Normal : MessageImportance::Supplementary;

    std::vector<ReportLine> lines;
    appendGroup(lines, L"NG", ngRows, widths, ngHeadingImportance, ngBodyImportance);
    lines.push_back(ReportLine{L"", MessageImportance::Supplementary});
    appendGroup(lines, L"OK", okRows, widths, MessageImportance::Supplementary,
               MessageImportance::Supplementary);

    return lines;
}
} // namespace syncwingetlink::cli
