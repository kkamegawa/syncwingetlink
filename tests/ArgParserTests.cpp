// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/ArgParser.h>
#include <core/Model.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::cli;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] ArgParseErrorKind expectError(const std::vector<std::wstring>& args)
{
    try
    {
        static_cast<void>(parseArguments(args));
    }
    catch (const ArgParseError& error)
    {
        return error.kind();
    }

    Assert::Fail(L"expected parseArguments() to throw ArgParseError");
}
} // namespace

TEST_CLASS(ArgParserCommandTests)
{
public:
    TEST_METHOD(noArgumentsDefaultsToScan)
    {
        const AppOptions options = parseArguments({});
        Assert::IsTrue(options.command == AppCommand::Scan);
    }

    TEST_METHOD(scanCommandParsesExplicitly)
    {
        const AppOptions options = parseArguments({L"scan"});
        Assert::IsTrue(options.command == AppCommand::Scan);
    }

    TEST_METHOD(fixCommandParses)
    {
        const AppOptions options = parseArguments({L"fix"});
        Assert::IsTrue(options.command == AppCommand::Fix);
    }

    TEST_METHOD(testRuleCommandParsesWithValidName)
    {
        const AppOptions options =
            parseArguments({L"test-rule", L"codex-x86_64-pc-windows-msvc.exe"});
        Assert::IsTrue(options.command == AppCommand::TestRule);
        Assert::IsTrue(options.testRuleName.has_value());
        Assert::AreEqual(std::wstring(L"codex-x86_64-pc-windows-msvc.exe"),
                         *options.testRuleName);
    }

    TEST_METHOD(testRuleWithoutNameThrowsMissingArgument)
    {
        Assert::IsTrue(expectError({L"test-rule"}) == ArgParseErrorKind::MissingArgument);
    }

    TEST_METHOD(testRuleNameWithBackslashIsRejected)
    {
        Assert::IsTrue(expectError({L"test-rule", L"sub\\name.exe"}) ==
                       ArgParseErrorKind::InvalidTestRuleName);
    }

    TEST_METHOD(testRuleNameWithForwardSlashIsRejected)
    {
        Assert::IsTrue(expectError({L"test-rule", L"sub/name.exe"}) ==
                       ArgParseErrorKind::InvalidTestRuleName);
    }

    TEST_METHOD(testRuleNameWithDriveLetterIsRejected)
    {
        Assert::IsTrue(expectError({L"test-rule", L"C:\\name.exe"}) ==
                       ArgParseErrorKind::InvalidTestRuleName);
    }

    TEST_METHOD(unknownCommandIsRejected)
    {
        Assert::IsTrue(expectError({L"resync"}) == ArgParseErrorKind::UnknownCommand);
    }

    TEST_METHOD(unexpectedTrailingArgumentAfterFixIsRejected)
    {
        Assert::IsTrue(expectError({L"fix", L"extra"}) ==
                       ArgParseErrorKind::UnexpectedArgument);
    }
};

TEST_CLASS(ArgParserOptionTests)
{
public:
    TEST_METHOD(sourceOptionParsesEachDocumentedValue)
    {
        Assert::IsTrue(parseArguments({L"--source", L"com"}).source == PackageSource::Com);
        Assert::IsTrue(parseArguments({L"--source", L"fs"}).source ==
                       PackageSource::FileSystem);
        Assert::IsTrue(parseArguments({L"--source", L"auto"}).source == PackageSource::Auto);
    }

    TEST_METHOD(sourceDefaultsToAutoWhenOmitted)
    {
        // Pins the documented default (--help: "default auto: COM first, filesystem
        // scan fallback", docs/adr-phase-2.md ADR-0010) so a future regression here is
        // caught by this test rather than only by re-reading the help text.
        Assert::IsTrue(parseArguments({}).source == PackageSource::Auto);
    }

    TEST_METHOD(invalidSourceValueIsRejected)
    {
        Assert::IsTrue(expectError({L"--source", L"bogus"}) ==
                       ArgParseErrorKind::InvalidOptionValue);
    }

    TEST_METHOD(tuiFlagSetsUseTui)
    {
        // --tui is only valid with fix (see ArgParserTerminatorAndConflictTests's
        // tui* cases below) - a bare "--tui" would default to AppCommand::Scan and
        // now be rejected, so this test names fix explicitly.
        Assert::IsTrue(parseArguments({L"fix", L"--tui"}).useTui);
    }

    TEST_METHOD(dryRunFlagSetsDryRun)
    {
        Assert::IsTrue(parseArguments({L"fix", L"--dry-run"}).dryRun);
    }

    TEST_METHOD(yesFlagAndItsShortFormBothSetAssumeYes)
    {
        Assert::IsTrue(parseArguments({L"fix", L"--yes"}).assumeYes);
        Assert::IsTrue(parseArguments({L"fix", L"-y"}).assumeYes);
    }

    TEST_METHOD(includeAndExcludeAccumulatePatterns)
    {
        const AppOptions options =
            parseArguments({L"--include", L"codex*", L"--include", L"rg*", L"--exclude",
                            L"*beta*"});

        Assert::AreEqual(size_t{2}, options.includePatterns.size());
        Assert::AreEqual(std::wstring(L"codex*"), options.includePatterns[0]);
        Assert::AreEqual(std::wstring(L"rg*"), options.includePatterns[1]);
        Assert::AreEqual(size_t{1}, options.excludePatterns.size());
        Assert::AreEqual(std::wstring(L"*beta*"), options.excludePatterns[0]);
    }

    TEST_METHOD(jsonFlagSetsJsonOutput)
    {
        Assert::IsTrue(parseArguments({L"--json"}).jsonOutput);
    }

    TEST_METHOD(verboseAndQuietSetLogLevel)
    {
        Assert::IsTrue(parseArguments({L"--verbose"}).logLevel == LogLevel::Verbose);
        Assert::IsTrue(parseArguments({L"--quiet"}).logLevel == LogLevel::Quiet);
    }

    // #113 (ADR-0030): repeating --verbose/--quiet is last-wins, in either order -
    // matching every other repeatable ArgParser option (e.g. --source) rather than
    // leaving the interaction an accident of code order.
    TEST_METHOD(verboseThenQuietIsLastWins)
    {
        Assert::IsTrue(parseArguments({L"--verbose", L"--quiet"}).logLevel == LogLevel::Quiet);
    }

    TEST_METHOD(quietThenVerboseIsLastWins)
    {
        Assert::IsTrue(parseArguments({L"--quiet", L"--verbose"}).logLevel == LogLevel::Verbose);
    }

    TEST_METHOD(failOnMissingFlagSets)
    {
        Assert::IsTrue(parseArguments({L"scan", L"--fail-on-missing"}).failOnMissing);
    }

    TEST_METHOD(noColorFlagSets)
    {
        Assert::IsTrue(parseArguments({L"--no-color"}).noColor);
    }

    TEST_METHOD(silentFlagSets)
    {
        Assert::IsTrue(parseArguments({L"fix", L"--silent"}).silent);
    }

    TEST_METHOD(missingOptionValueIsRejected)
    {
        Assert::IsTrue(expectError({L"--source"}) == ArgParseErrorKind::MissingOptionValue);
        Assert::IsTrue(expectError({L"--rules"}) == ArgParseErrorKind::MissingOptionValue);
    }

    TEST_METHOD(unknownOptionIsRejected)
    {
        Assert::IsTrue(expectError({L"--not-a-real-option"}) ==
                       ArgParseErrorKind::UnknownOption);
    }
};

TEST_CLASS(ArgParserPathOverrideTests)
{
public:
    TEST_METHOD(emptyPathOverrideIsRejected)
    {
        Assert::IsTrue(expectError({L"--links-dir", L""}) ==
                       ArgParseErrorKind::InvalidPathOverride);
    }

    TEST_METHOD(deviceOverridePathIsRejected)
    {
        Assert::IsTrue(expectError({L"--packages-dir", LR"(\\.\PhysicalDrive0)"}) ==
                       ArgParseErrorKind::InvalidPathOverride);
    }

    TEST_METHOD(relativePathOverrideBecomesAbsolute)
    {
        const AppOptions options = parseArguments({L"--links-dir", LR"(relative\Links)"});

        Assert::IsTrue(options.linksDirectory.has_value());
        Assert::IsTrue(options.linksDirectory->is_absolute());
    }

    // A path override that does not exist on disk must still parse successfully - an
    // absent Packages directory is a normal, tolerated state elsewhere in the codebase
    // (docs/adr-phase-2.md ADR-0010), and an absent Links directory is the exact
    // condition `fix` exists to correct. See ArgParser.h's parseArguments() docs.
    TEST_METHOD(nonExistentPathOverrideIsAccepted)
    {
        const std::filesystem::path doesNotExist =
            std::filesystem::temp_directory_path() /
            LR"(syncwingetlink-argparser-test-does-not-exist)";

        const AppOptions options =
            parseArguments({L"--packages-dir", doesNotExist.native()});

        Assert::IsTrue(options.packagesDirectory.has_value());
    }

    TEST_METHOD(rulesPathOverrideIsStored)
    {
        const AppOptions options = parseArguments({L"--rules", LR"(C:\config\rules.json)"});

        Assert::IsTrue(options.rulesPath.has_value());
        Assert::IsTrue(options.rulesPath->is_absolute());
    }
};

TEST_CLASS(ArgParserTerminatorAndConflictTests)
{
public:
    TEST_METHOD(doubleDashTreatsFollowingTokensAsPositionalNotOptions)
    {
        // Without "--", a NAME starting with '-' would be misread as an unknown option.
        const AppOptions options =
            parseArguments({L"test-rule", L"--", L"-strange.exe"});

        Assert::IsTrue(options.command == AppCommand::TestRule);
        Assert::IsTrue(options.testRuleName.has_value());
        Assert::AreEqual(std::wstring(L"-strange.exe"), *options.testRuleName);
    }

    TEST_METHOD(jsonWithFixWithoutYesIsRejected)
    {
        Assert::IsTrue(expectError({L"fix", L"--json"}) ==
                       ArgParseErrorKind::ConflictingOptions);
    }

    TEST_METHOD(jsonWithFixAndYesSucceeds)
    {
        const AppOptions options = parseArguments({L"fix", L"--json", L"--yes"});
        Assert::IsTrue(options.jsonOutput);
        Assert::IsTrue(options.assumeYes);
    }

    TEST_METHOD(jsonWithScanNeverConflicts)
    {
        const AppOptions options = parseArguments({L"scan", L"--json"});
        Assert::IsTrue(options.jsonOutput);
    }

    // --tui (M7, issue #59) is only meaningful for an interactive `fix`: scan/test-rule
    // have no repair checklist to show, and --json/--yes both imply an unattended,
    // scriptable invocation - the opposite of an interactive checklist.
    TEST_METHOD(tuiWithScanIsRejected)
    {
        Assert::IsTrue(expectError({L"scan", L"--tui"}) ==
                       ArgParseErrorKind::ConflictingOptions);
    }

    TEST_METHOD(tuiWithTestRuleIsRejected)
    {
        Assert::IsTrue(expectError({L"test-rule", L"foo.exe", L"--tui"}) ==
                       ArgParseErrorKind::ConflictingOptions);
    }

    TEST_METHOD(tuiWithJsonIsRejected)
    {
        Assert::IsTrue(expectError({L"fix", L"--tui", L"--json", L"--yes"}) ==
                       ArgParseErrorKind::ConflictingOptions);
    }

    TEST_METHOD(tuiWithYesIsRejected)
    {
        Assert::IsTrue(expectError({L"fix", L"--tui", L"--yes"}) ==
                       ArgParseErrorKind::ConflictingOptions);
    }

    TEST_METHOD(tuiWithFixAloneSucceeds)
    {
        const AppOptions options = parseArguments({L"fix", L"--tui"});
        Assert::IsTrue(options.useTui);
        Assert::IsTrue(options.command == AppCommand::Fix);
    }

    TEST_METHOD(tuiWithFixAndDryRunAndNoColorSucceeds)
    {
        // Both --dry-run and --no-color remain supported alongside --tui - only
        // scan/test-rule/--json/--yes conflict with it.
        const AppOptions options =
            parseArguments({L"fix", L"--tui", L"--dry-run", L"--no-color"});
        Assert::IsTrue(options.useTui);
        Assert::IsTrue(options.dryRun);
        Assert::IsTrue(options.noColor);
    }

    TEST_METHOD(helpFlagShortCircuitsEvenWithMalformedRemainder)
    {
        const AppOptions options = parseArguments({L"--help", L"--not-a-real-option"});
        Assert::IsTrue(options.command == AppCommand::Help);
    }

    TEST_METHOD(helpShortFormShortCircuits)
    {
        const AppOptions options = parseArguments({L"-h"});
        Assert::IsTrue(options.command == AppCommand::Help);
    }

    TEST_METHOD(versionFlagShortCircuitsEvenWithMalformedRemainder)
    {
        const AppOptions options = parseArguments({L"--version", L"fix", L"extra", L"extra2"});
        Assert::IsTrue(options.command == AppCommand::Version);
    }
};
} // namespace syncwingetlink::tests
