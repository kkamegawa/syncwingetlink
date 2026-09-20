// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/PathDisplay.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::cli;

// abbreviateKnownFolders() takes its folder table as a parameter precisely so these
// cases can run against a synthetic one: the assertions below hold on any machine,
// regardless of where that machine's real profile actually lives. knownFolderMappings()
// - the one piece that does query the shell - is covered only for shape
// (see KnownFolderMappingsTests at the bottom), since its contents are host-dependent.
namespace syncwingetlink::tests
{
namespace
{
// Mirrors the real table's shape: %LOCALAPPDATA% nested inside %USERPROFILE%, which is
// what makes the longest-match rule load-bearing rather than cosmetic.
[[nodiscard]] std::vector<KnownFolderMapping> makeFolders()
{
    return {
        KnownFolderMapping{L"C:\\Users\\alice\\AppData\\Local", L"%LOCALAPPDATA%"},
        KnownFolderMapping{L"C:\\Users\\alice\\AppData\\Roaming", L"%APPDATA%"},
        KnownFolderMapping{L"C:\\Users\\alice", L"%USERPROFILE%"},
    };
}
} // namespace

TEST_CLASS(AbbreviateKnownFoldersTests)
{
public:
    TEST_METHOD(aPathUnderAKnownFolderIsAbbreviated)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(
            std::wstring(L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Links\\copilot.exe"),
            abbreviateKnownFolders(
                L"C:\\Users\\alice\\AppData\\Local\\Microsoft\\WinGet\\Links\\copilot.exe",
                folders));
    }

    TEST_METHOD(aPathEqualToAKnownFolderBecomesJustTheVariable)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"%USERPROFILE%"),
                         abbreviateKnownFolders(L"C:\\Users\\alice", folders));
    }

    TEST_METHOD(anUnrelatedPathIsReturnedUnchanged)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"D:\\scratch\\links\\tool.exe"),
                         abbreviateKnownFolders(L"D:\\scratch\\links\\tool.exe", folders));
    }

    // The whole point of the boundary check: a sibling profile whose name merely starts
    // with another profile's name must not be rewritten.
    TEST_METHOD(aPrefixThatDoesNotEndOnAComponentBoundaryIsNotAbbreviated)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"C:\\Users\\alice2\\Documents\\tool.exe"),
                         abbreviateKnownFolders(L"C:\\Users\\alice2\\Documents\\tool.exe",
                                                folders));
    }

    // %LOCALAPPDATA% is itself under %USERPROFILE%; scan order alone would produce the
    // less specific (and less useful) of the two.
    TEST_METHOD(theLongestMatchingFolderWins)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"%LOCALAPPDATA%\\Temp"),
                         abbreviateKnownFolders(L"C:\\Users\\alice\\AppData\\Local\\Temp",
                                                folders));
        Assert::AreEqual(std::wstring(L"%APPDATA%\\Code"),
                         abbreviateKnownFolders(L"C:\\Users\\alice\\AppData\\Roaming\\Code",
                                                folders));
    }

    TEST_METHOD(theLongestMatchWinsRegardlessOfTableOrder)
    {
        // Same table, least specific first - the result must not change.
        const std::vector<KnownFolderMapping> reversed = {
            KnownFolderMapping{L"C:\\Users\\alice", L"%USERPROFILE%"},
            KnownFolderMapping{L"C:\\Users\\alice\\AppData\\Local", L"%LOCALAPPDATA%"},
        };
        Assert::AreEqual(std::wstring(L"%LOCALAPPDATA%\\Temp"),
                         abbreviateKnownFolders(L"C:\\Users\\alice\\AppData\\Local\\Temp",
                                                reversed));
    }

    TEST_METHOD(matchingIsOrdinalCaseInsensitive)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"%LOCALAPPDATA%\\Microsoft"),
                         abbreviateKnownFolders(L"c:\\users\\ALICE\\appdata\\local\\Microsoft",
                                                folders));
    }

    TEST_METHOD(aForwardSlashAlsoCountsAsAComponentBoundary)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"%USERPROFILE%/Documents"),
                         abbreviateKnownFolders(L"C:\\Users\\alice/Documents", folders));
    }

    TEST_METHOD(anEmptyFolderTableLeavesEveryPathAlone)
    {
        const std::vector<KnownFolderMapping> none;
        Assert::AreEqual(
            std::wstring(L"C:\\Users\\alice\\AppData\\Local\\tool.exe"),
            abbreviateKnownFolders(L"C:\\Users\\alice\\AppData\\Local\\tool.exe", none));
    }

    // A defensive case, not a reachable one: buildKnownFolderMappings() drops a folder
    // the shell declined to report rather than storing an empty root. An empty prefix
    // would otherwise match everything.
    TEST_METHOD(anEmptyFolderRootNeverMatches)
    {
        const std::vector<KnownFolderMapping> folders = {
            KnownFolderMapping{L"", L"%NOTHING%"},
        };
        Assert::AreEqual(std::wstring(L"C:\\tool.exe"),
                         abbreviateKnownFolders(L"C:\\tool.exe", folders));
    }

    TEST_METHOD(aRelativePathIsReturnedUnchanged)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"tool.exe"),
                         abbreviateKnownFolders(L"tool.exe", folders));
    }

    TEST_METHOD(anEmptyPathIsReturnedUnchanged)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L""), abbreviateKnownFolders(L"", folders));
    }
};

TEST_CLASS(FormatPathForDisplayTests)
{
public:
    TEST_METHOD(theFlagOffLeavesThePathAsSanitizeForDisplayWouldRenderIt)
    {
        const std::filesystem::path path = L"C:\\Packages\\tool\\tool.exe";
        Assert::AreEqual(std::wstring(L"C:\\Packages\\tool\\tool.exe"),
                         formatPathForDisplay(path, PathDisplayOptions{}));
    }

    // sanitizeForDisplay() must remain the *last* step, so an abbreviated path can never
    // smuggle a control sequence into a terminal or a JSON document.
    TEST_METHOD(sanitizationStillAppliesWithTheFlagOff)
    {
        const std::filesystem::path path =
            std::wstring(L"C:\\Packages\\tool") + wchar_t(0x1B) + L"[31m\\tool.exe";
        const std::wstring rendered = formatPathForDisplay(path, PathDisplayOptions{});
        Assert::IsTrue(rendered.find(wchar_t(0x1B)) == std::wstring::npos);
    }

    TEST_METHOD(sanitizationStillAppliesWithTheFlagOn)
    {
        const std::filesystem::path path =
            std::wstring(L"C:\\Packages\\tool") + wchar_t(0x1B) + L"[31m\\tool.exe";
        const std::wstring rendered =
            formatPathForDisplay(path, PathDisplayOptions{/* showSpecialFolders */ true});
        Assert::IsTrue(rendered.find(wchar_t(0x1B)) == std::wstring::npos);
    }

    // A path that matches no known folder is identical either way - which is also what
    // makes this assertion host-independent.
    TEST_METHOD(anUnrelatedPathRendersIdenticallyWithAndWithoutTheFlag)
    {
        const std::filesystem::path path = L"D:\\scratch\\links\\tool.exe";
        Assert::AreEqual(formatPathForDisplay(path, PathDisplayOptions{}),
                         formatPathForDisplay(path, PathDisplayOptions{true}));
    }
};

TEST_CLASS(KnownFolderMappingsTests)
{
public:
    // Contents are host-dependent, so this asserts the invariants the matching rules
    // rely on rather than any particular path: no empty roots, no trailing separators,
    // and a %NAME% spelling for each entry.
    TEST_METHOD(everyMappingHasANonEmptyRootWithoutATrailingSeparator)
    {
        for (const KnownFolderMapping& mapping : knownFolderMappings())
        {
            Assert::IsFalse(mapping.root.empty());
            Assert::IsTrue(mapping.root.back() != L'\\' && mapping.root.back() != L'/');
            Assert::IsFalse(mapping.variable.empty());
            Assert::IsTrue(mapping.variable.front() == L'%');
            Assert::IsTrue(mapping.variable.back() == L'%');
        }
    }

    TEST_METHOD(theTableIsCachedAndStableAcrossCalls)
    {
        const std::span<const KnownFolderMapping> first = knownFolderMappings();
        const std::span<const KnownFolderMapping> second = knownFolderMappings();
        Assert::AreEqual(first.size(), second.size());
        Assert::IsTrue(first.data() == second.data());
    }
};

// Review follow-up on #180: ArgParser accepts an extended-length override verbatim
// (it rejects only "\\.\" device paths) while SHGetKnownFolderPath always reports the
// ordinary form, so a literal comparison left exactly those paths carrying the account
// name under -s.
TEST_CLASS(AbbreviateExtendedLengthPathTests)
{
public:
    TEST_METHOD(anExtendedLengthPathUnderAKnownFolderIsAbbreviated)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(
            std::wstring(L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Links\\tool.exe"),
            abbreviateKnownFolders(
                L"\\\\?\\C:\\Users\\alice\\AppData\\Local\\Microsoft\\WinGet\\Links\\tool.exe",
                folders));
    }

    TEST_METHOD(anExtendedLengthPathEqualToAKnownFolderBecomesJustTheVariable)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"%USERPROFILE%"),
                         abbreviateKnownFolders(L"\\\\?\\C:\\Users\\alice", folders));
    }

    // Normalization is for *matching* only. A path that matches nothing keeps the
    // caller's own spelling, prefix included - stripping it would be this function
    // silently rewriting a path it was not asked to touch.
    TEST_METHOD(anUnmatchedExtendedLengthPathKeepsItsPrefix)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"\\\\?\\D:\\scratch\\links\\tool.exe"),
                         abbreviateKnownFolders(L"\\\\?\\D:\\scratch\\links\\tool.exe", folders));
    }

    // The boundary rule still applies after normalization.
    TEST_METHOD(anExtendedLengthSiblingProfileIsNotAbbreviated)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(
            std::wstring(L"\\\\?\\C:\\Users\\alice2\\Documents\\tool.exe"),
            abbreviateKnownFolders(L"\\\\?\\C:\\Users\\alice2\\Documents\\tool.exe", folders));
    }
};

// Review follow-up on #180: core/ formats its exception messages with a real path
// embedded mid-sentence, so -s abbreviated every ordinary path and then leaked the
// account name the moment anything failed.
TEST_CLASS(AbbreviateKnownFoldersInTextTests)
{
public:
    TEST_METHOD(aPathEmbeddedInADiagnosticIsAbbreviated)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(
            std::wstring(L"CreateSymbolicLinkW failed for "
                         L"'%LOCALAPPDATA%\\Microsoft\\WinGet\\Links\\tool.exe' "
                         L"(Win32 error 5)"),
            abbreviateKnownFoldersInText(
                L"CreateSymbolicLinkW failed for "
                L"'C:\\Users\\alice\\AppData\\Local\\Microsoft\\WinGet\\Links\\tool.exe' "
                L"(Win32 error 5)",
                folders));
    }

    // A quote closes a path just as a separator opens a component - without that, a
    // message naming the folder itself would keep the account name.
    TEST_METHOD(aQuoteCountsAsABoundary)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"could not open '%USERPROFILE%' for reading"),
                         abbreviateKnownFoldersInText(
                             L"could not open 'C:\\Users\\alice' for reading", folders));
    }

    TEST_METHOD(severalOccurrencesAreAllAbbreviated)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(
            std::wstring(L"copy '%LOCALAPPDATA%\\a.exe' to '%APPDATA%\\b.exe'"),
            abbreviateKnownFoldersInText(
                L"copy 'C:\\Users\\alice\\AppData\\Local\\a.exe' to "
                L"'C:\\Users\\alice\\AppData\\Roaming\\b.exe'",
                folders));
    }

    TEST_METHOD(theLongestMatchStillWinsInsideText)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L"at %LOCALAPPDATA%\\Temp"),
                         abbreviateKnownFoldersInText(
                             L"at C:\\Users\\alice\\AppData\\Local\\Temp", folders));
    }

    // The component-boundary guarantee must survive the wider boundary rule.
    TEST_METHOD(aSiblingProfileInsideTextIsNotAbbreviated)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        const std::wstring text = L"could not read 'C:\\Users\\alice2\\notes.txt'";
        Assert::AreEqual(text, abbreviateKnownFoldersInText(text, folders));
    }

    TEST_METHOD(textWithNoPathIsReturnedUnchanged)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        const std::wstring text = L"rules file is not valid JSON at offset 12";
        Assert::AreEqual(text, abbreviateKnownFoldersInText(text, folders));
    }

    TEST_METHOD(anEmptyFolderTableLeavesTextAlone)
    {
        const std::vector<KnownFolderMapping> none;
        const std::wstring text = L"failed for 'C:\\Users\\alice\\AppData\\Local\\a.exe'";
        Assert::AreEqual(text, abbreviateKnownFoldersInText(text, none));
    }

    TEST_METHOD(emptyTextIsReturnedUnchanged)
    {
        const std::vector<KnownFolderMapping> folders = makeFolders();
        Assert::AreEqual(std::wstring(L""), abbreviateKnownFoldersInText(L"", folders));
    }
};

TEST_CLASS(FormatDiagnosticForDisplayTests)
{
public:
    TEST_METHOD(theFlagOffLeavesTheMessageByteForByte)
    {
        const std::wstring text = L"failed for 'C:\\Users\\alice\\AppData\\Local\\a.exe'";
        Assert::AreEqual(text, formatDiagnosticForDisplay(text, PathDisplayOptions{}));
    }

    // Unlike formatPathForDisplay(), this one does not sanitize: its callers are
    // existing Console::writeLine() sites that already sanitize at their own boundary,
    // and a second pass would change what today's non-path diagnostics look like.
    TEST_METHOD(itDoesNotSanitize)
    {
        const std::wstring text = std::wstring(L"failed") + wchar_t(0x1B) + L"[31m";
        Assert::AreEqual(text, formatDiagnosticForDisplay(text, PathDisplayOptions{}));
        Assert::AreEqual(text, formatDiagnosticForDisplay(text, PathDisplayOptions{true}));
    }

    TEST_METHOD(aMessageWithNoKnownFolderIsIdenticalEitherWay)
    {
        const std::wstring text = L"failed for 'D:\\scratch\\a.exe' (Win32 error 5)";
        Assert::AreEqual(formatDiagnosticForDisplay(text, PathDisplayOptions{}),
                         formatDiagnosticForDisplay(text, PathDisplayOptions{true}));
    }
};
} // namespace syncwingetlink::tests
