// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/ScanReport.h>

#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::cli;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] RepairItem makeItem(std::wstring packageId, LinkStatus status, std::wstring alias,
                                  std::wstring executablePath)
{
    RepairItem item;
    item.executable.path = std::move(executablePath);
    item.alias = std::move(alias);
    item.status = status;
    item.packageId = std::move(packageId);
    return item;
}

[[nodiscard]] std::wstring joinLines(const std::vector<ReportLine>& lines)
{
    std::wstring joined;
    for (const ReportLine& line : lines)
    {
        joined += line.text;
        joined += L'\n';
    }
    return joined;
}

[[nodiscard]] bool isSeparatorLine(const std::wstring& text) noexcept
{
    return !text.empty() && text.find_first_not_of(L'-') == std::wstring::npos;
}

// The real %LOCALAPPDATA% root on whatever host is running these tests, or an empty
// string if the shell declined to report it. Building the fixture from the live table
// keeps the --showspecialfolder assertions below host-independent without hard-coding
// any profile path.
[[nodiscard]] std::wstring liveLocalAppDataRoot()
{
    for (const KnownFolderMapping& mapping : knownFolderMappings())
    {
        if (mapping.variable == L"%LOCALAPPDATA%")
        {
            return mapping.root;
        }
    }
    return {};
}
} // namespace

TEST_CLASS(DisplayWidthTests)
{
public:
    TEST_METHOD(emptyStringIsZero)
    {
        Assert::AreEqual(std::size_t{0}, displayWidth(L""));
    }

    TEST_METHOD(asciiCountsOnePerCharacter)
    {
        Assert::AreEqual(std::size_t{5}, displayWidth(L"abcde"));
    }

    TEST_METHOD(katakanaCountsTwoPerCharacter)
    {
        // U+30A2 U+30A4 U+30A6 (katakana A I U) - each 2 columns wide -> 6.
        Assert::AreEqual(std::size_t{6}, displayWidth(L"\u30A2\u30A4\u30A6"));
    }

    TEST_METHOD(supplementaryPlaneSurrogatePairCountsAsOneWideCodePoint)
    {
        // U+1F600 (grinning face) as a surrogate pair: D83D DE00. It falls in the emoji
        // wide range, so it counts as 2 columns for one code point, not 2 (one per unit).
        const std::wstring input(1, static_cast<wchar_t>(0xD83D));
        const std::wstring withLowSurrogate = input + static_cast<wchar_t>(0xDE00);
        Assert::AreEqual(std::size_t{2}, displayWidth(withLowSurrogate));
    }

    TEST_METHOD(unpairedSurrogateCountsAsOneColumn)
    {
        const std::wstring input(1, static_cast<wchar_t>(0xD83D)); // no low surrogate follows
        Assert::AreEqual(std::size_t{1}, displayWidth(input));
    }
};

TEST_CLASS(GroupedReportTests)
{
public:
    TEST_METHOD(ngGroupPrecedesOkGroup)
    {
        const std::vector<RepairItem> items{
            makeItem(L"Pkg.Ok", LinkStatus::Ok, L"ok.exe", LR"(C:\pkg\ok.exe)"),
            makeItem(L"Pkg.Missing", LinkStatus::Missing, L"missing.exe", LR"(C:\pkg\missing.exe)"),
        };

        const std::wstring joined = joinLines(formatGroupedReport(items, ReportMode::Scan));

        const std::size_t ngPos = joined.find(L"NG");
        const std::size_t okPos = joined.find(L"OK");
        Assert::IsTrue(ngPos != std::wstring::npos);
        Assert::IsTrue(okPos != std::wstring::npos);
        Assert::IsTrue(ngPos < okPos);
    }

    TEST_METHOD(brokenAndMismatchLandInNgOnlyOkLandsInOk)
    {
        const std::vector<RepairItem> items{
            makeItem(L"Pkg.Broken", LinkStatus::Broken, L"broken.exe", LR"(C:\pkg\broken.exe)"),
            makeItem(L"Pkg.Mismatch", LinkStatus::Mismatch, L"mismatch.exe",
                     LR"(C:\pkg\mismatch.exe)"),
            makeItem(L"Pkg.Ok", LinkStatus::Ok, L"ok.exe", LR"(C:\pkg\ok.exe)"),
        };

        const std::wstring joined = joinLines(formatGroupedReport(items, ReportMode::Scan));

        const std::size_t okHeader = joined.find(L"OK");
        Assert::IsTrue(joined.find(L"broken.exe") < okHeader);
        Assert::IsTrue(joined.find(L"mismatch.exe") < okHeader);
        Assert::IsTrue(joined.find(L"ok.exe") > okHeader);
    }

    TEST_METHOD(zeroNgItemsRendersNothingForNgSection)
    {
        const std::vector<RepairItem> items{
            makeItem(L"Pkg.Ok", LinkStatus::Ok, L"ok.exe", LR"(C:\pkg\ok.exe)"),
        };

        const std::vector<ReportLine> lines = formatGroupedReport(items, ReportMode::Scan);

        Assert::AreEqual(std::wstring(L"NG"), lines.at(0).text);
        Assert::AreEqual(std::wstring(L"nothing"), lines.at(1).text);
    }

    TEST_METHOD(emptyInputRendersNothingForBothGroups)
    {
        const std::vector<RepairItem> items;

        const std::wstring joined = joinLines(formatGroupedReport(items, ReportMode::Scan));

        Assert::AreEqual(std::wstring(L"NG\nnothing\n\nOK\nnothing\n"), joined);
    }

    TEST_METHOD(withinGroupSortedByAliasOrdinalCaseInsensitiveAscending)
    {
        const std::vector<RepairItem> items{
            makeItem(L"Pkg.B", LinkStatus::Missing, L"Bravo.exe", LR"(C:\pkg\bravo.exe)"),
            makeItem(L"Pkg.A", LinkStatus::Missing, L"alpha.exe", LR"(C:\pkg\alpha.exe)"),
            makeItem(L"Pkg.C", LinkStatus::Missing, L"CHARLIE.exe", LR"(C:\pkg\charlie.exe)"),
        };

        const std::wstring joined = joinLines(formatGroupedReport(items, ReportMode::Scan));

        const std::size_t alphaPos = joined.find(L"alpha.exe");
        const std::size_t bravoPos = joined.find(L"Bravo.exe");
        const std::size_t charliePos = joined.find(L"CHARLIE.exe");
        Assert::IsTrue(alphaPos < bravoPos);
        Assert::IsTrue(bravoPos < charliePos);
    }

    TEST_METHOD(columnWidthsMatchTheLongestCellAcrossBothGroups)
    {
        const std::vector<RepairItem> items{
            makeItem(L"Short", LinkStatus::Missing, L"a.exe", LR"(C:\short.exe)"),
            makeItem(L"A.Very.Long.Package.Identifier", LinkStatus::Ok, L"b.exe",
                     LR"(C:\long.exe)"),
        };

        const std::vector<ReportLine> lines = formatGroupedReport(items, ReportMode::Scan);

        std::vector<std::wstring> separators;
        for (const ReportLine& line : lines)
        {
            if (isSeparatorLine(line.text))
            {
                separators.push_back(line.text);
            }
        }

        Assert::IsTrue(separators.size() >= 2);
        for (const std::wstring& separator : separators)
        {
            Assert::AreEqual(separators.front().size(), separator.size());
        }
    }

    TEST_METHOD(scanModeMarksNgRowsNormalAndOkRowsSupplementary)
    {
        const std::vector<RepairItem> items{
            makeItem(L"Pkg.Missing", LinkStatus::Missing, L"missing.exe", LR"(C:\pkg\missing.exe)"),
            makeItem(L"Pkg.Ok", LinkStatus::Ok, L"ok.exe", LR"(C:\pkg\ok.exe)"),
        };

        const std::vector<ReportLine> lines = formatGroupedReport(items, ReportMode::Scan);

        bool foundNgRow = false;
        bool foundOkRow = false;
        for (const ReportLine& line : lines)
        {
            if (line.text.find(L"missing.exe") != std::wstring::npos)
            {
                Assert::IsTrue(line.importance == MessageImportance::Normal);
                foundNgRow = true;
            }
            if (line.text.find(L"ok.exe") != std::wstring::npos)
            {
                Assert::IsTrue(line.importance == MessageImportance::Supplementary);
                foundOkRow = true;
            }
        }
        Assert::IsTrue(foundNgRow);
        Assert::IsTrue(foundOkRow);
    }

    TEST_METHOD(fixPreviewModeMarksEveryLineSupplementary)
    {
        const std::vector<RepairItem> items{
            makeItem(L"Pkg.Missing", LinkStatus::Missing, L"missing.exe", LR"(C:\pkg\missing.exe)"),
            makeItem(L"Pkg.Ok", LinkStatus::Ok, L"ok.exe", LR"(C:\pkg\ok.exe)"),
        };

        const std::vector<ReportLine> lines = formatGroupedReport(items, ReportMode::FixPreview);

        for (const ReportLine& line : lines)
        {
            Assert::IsTrue(line.importance == MessageImportance::Supplementary);
        }
    }

    TEST_METHOD(nonAsciiPackageIdAndAliasAlignByDisplayWidth)
    {
        const std::vector<RepairItem> items{
            makeItem(L"\u30D1\u30C3\u30B1\u30FC\u30B8", LinkStatus::Missing, L"a.exe",
                     LR"(C:\a.exe)"),
            makeItem(L"Pkg", LinkStatus::Missing, L"bb.exe", LR"(C:\bb.exe)"),
        };

        const std::vector<ReportLine> lines = formatGroupedReport(items, ReportMode::Scan);

        std::size_t separatorWidth = 0;
        for (const ReportLine& line : lines)
        {
            if (isSeparatorLine(line.text))
            {
                separatorWidth = displayWidth(line.text);
                break;
            }
        }
        Assert::IsTrue(separatorWidth > 0);

        // The target column is unpadded, so a data row's rendered width can be shorter
        // than the separator, but column alignment (measured via displayWidth(), not
        // raw code-unit length) must never let it exceed the separator.
        for (const ReportLine& line : lines)
        {
            if (line.text.find(L"a.exe") != std::wstring::npos ||
                line.text.find(L"bb.exe") != std::wstring::npos)
            {
                Assert::IsTrue(displayWidth(line.text) <= separatorWidth);
            }
        }
    }

    TEST_METHOD(emptyPackageIdRendersAsDash)
    {
        const std::vector<RepairItem> items{
            makeItem(L"", LinkStatus::Missing, L"noPackage.exe", LR"(C:\noPackage.exe)"),
        };

        const std::wstring joined = joinLines(formatGroupedReport(items, ReportMode::Scan));

        Assert::IsTrue(joined.find(L"- ") != std::wstring::npos);
    }
};

// docs/adr-phase-10.md ADR-0048: --showspecialfolder rewrites the target column only.
TEST_CLASS(GroupedReportPathDisplayTests)
{
public:
    TEST_METHOD(theTargetColumnIsAbbreviatedWhenTheOptionIsSet)
    {
        const std::wstring root = liveLocalAppDataRoot();
        if (root.empty())
        {
            return; // Nothing to abbreviate against on this host.
        }

        const std::wstring target = root + LR"(\Microsoft\WinGet\Packages\t\tool.exe)";
        const std::vector<RepairItem> items = {
            makeItem(L"Pkg.Tool", LinkStatus::Missing, L"tool.exe", target),
        };

        const std::wstring joined = joinLines(
            formatGroupedReport(items, ReportMode::Scan, PathDisplayOptions{true}));

        Assert::IsTrue(joined.find(L"%LOCALAPPDATA%") != std::wstring::npos);
        Assert::IsTrue(joined.find(root) == std::wstring::npos);
    }

    TEST_METHOD(theTargetColumnKeepsTheRealPathByDefault)
    {
        const std::wstring root = liveLocalAppDataRoot();
        if (root.empty())
        {
            return;
        }

        const std::wstring target = root + LR"(\Microsoft\WinGet\Packages\t\tool.exe)";
        const std::vector<RepairItem> items = {
            makeItem(L"Pkg.Tool", LinkStatus::Missing, L"tool.exe", target),
        };

        const std::wstring joined = joinLines(formatGroupedReport(items, ReportMode::Scan));

        Assert::IsTrue(joined.find(root) != std::wstring::npos);
        Assert::IsTrue(joined.find(L"%LOCALAPPDATA%") == std::wstring::npos);
    }

    // Sorting uses the real executable path, so abbreviating can never reorder a report.
    TEST_METHOD(rowOrderIsIdenticalWithAndWithoutTheOption)
    {
        const std::vector<RepairItem> items = {
            makeItem(L"Pkg.C", LinkStatus::Missing, L"c.exe", LR"(C:\pkg\c.exe)"),
            makeItem(L"Pkg.A", LinkStatus::Broken, L"a.exe", LR"(C:\pkg\a.exe)"),
            makeItem(L"Pkg.B", LinkStatus::Mismatch, L"b.exe", LR"(C:\pkg\b.exe)"),
            makeItem(L"Pkg.D", LinkStatus::Ok, L"d.exe", LR"(C:\pkg\d.exe)"),
        };

        const std::vector<ReportLine> plain = formatGroupedReport(items, ReportMode::Scan);
        const std::vector<ReportLine> abbreviated =
            formatGroupedReport(items, ReportMode::Scan, PathDisplayOptions{true});

        Assert::AreEqual(plain.size(), abbreviated.size());
        // None of these paths lives under a known folder, so the rendered tables must be
        // identical - order included.
        Assert::AreEqual(joinLines(plain), joinLines(abbreviated));
    }
};
} // namespace syncwingetlink::tests
