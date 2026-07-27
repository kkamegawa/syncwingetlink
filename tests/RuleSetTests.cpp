// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/ComApartment.h>
#include <rules/RuleSet.h>

#include <memory>
#include <optional>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
// winrt::Windows::Data::Json (used by RuleSet::parse()) requires an initialized
// apartment at runtime - see docs/adr-phase-2.md ADR-0011. No other test in this module
// depends on COM, so this is scoped to this one module via TEST_MODULE_INITIALIZE /
// TEST_MODULE_CLEANUP rather than adding a process-wide apartment for every test.
std::unique_ptr<ComApartment> g_apartment;
} // namespace

TEST_MODULE_INITIALIZE(RuleSetModuleInitialize)
{
    g_apartment = std::make_unique<ComApartment>();
}

TEST_MODULE_CLEANUP(RuleSetModuleCleanup)
{
    g_apartment.reset();
}

TEST_CLASS(IsValidAliasFileNameTests)
{
public:
    TEST_METHOD(ordinaryAliasNamesAreAccepted)
    {
        Assert::IsTrue(isValidAliasFileName(L"codex.exe"));
        Assert::IsTrue(isValidAliasFileName(L"restic.exe"));
    }

    TEST_METHOD(extensionComparisonIsCaseInsensitive)
    {
        Assert::IsTrue(isValidAliasFileName(L"codex.EXE"));
        Assert::IsTrue(isValidAliasFileName(L"codex.Exe"));
    }

    TEST_METHOD(anEmptyStemIsRejected)
    {
        Assert::IsFalse(isValidAliasFileName(L".exe"));
        Assert::IsFalse(isValidAliasFileName(L""));
    }

    TEST_METHOD(missingOrWrongExtensionIsRejected)
    {
        Assert::IsFalse(isValidAliasFileName(L"codex"));
        Assert::IsFalse(isValidAliasFileName(L"codex.dll"));
        Assert::IsFalse(isValidAliasFileName(L"codex.exe.bak"));
    }

    TEST_METHOD(pathSeparatorsAreRejected)
    {
        Assert::IsFalse(isValidAliasFileName(L"sub\\codex.exe"));
        Assert::IsFalse(isValidAliasFileName(L"sub/codex.exe"));
    }

    TEST_METHOD(aStemMadeEntirelyOfDotsIsRejected)
    {
        Assert::IsFalse(isValidAliasFileName(L"..exe"));
        Assert::IsFalse(isValidAliasFileName(L"...exe"));
    }
};

TEST_CLASS(RuleSetConstructionTests)
{
public:
    TEST_METHOD(emptyRuleListIsAccepted)
    {
        const RuleSet rules;
        Assert::IsTrue(rules.isEmpty());
        Assert::AreEqual(static_cast<std::size_t>(0), rules.size());
    }

    TEST_METHOD(anEmptyRuleNameIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            RuleSet(std::vector<AliasRule>{AliasRule{L"", L"^(.+)\\.exe$", L"$1.exe", false}});
        });
    }

    TEST_METHOD(duplicateRuleNamesAreRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            RuleSet(std::vector<AliasRule>{
                AliasRule{L"dup", L"^a\\.exe$", L"a.exe", false},
                AliasRule{L"dup", L"^b\\.exe$", L"b.exe", false},
            });
        });
    }

    TEST_METHOD(anInvalidRegexPatternIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            RuleSet(std::vector<AliasRule>{AliasRule{L"broken", L"^(unterminated", L"$1.exe", false}});
        });
    }

    TEST_METHOD(rejectingARuleReportsTheRightErrorKind)
    {
        try
        {
            RuleSet(std::vector<AliasRule>{AliasRule{L"broken", L"^(unterminated", L"$1.exe", false}});
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError& error)
        {
            Assert::IsTrue(RuleSetErrorKind::InvalidRegex == error.kind());
        }
    }
};

TEST_CLASS(RuleSetResolveTests)
{
public:
    TEST_METHOD(theFirstMatchingRuleWins)
    {
        const RuleSet rules({
            AliasRule{L"specific", L"^codex-x86_64\\.exe$", L"codex-specific.exe", false},
            AliasRule{L"generic", L"^codex-.+\\.exe$", L"codex-generic.exe", false},
        });

        const auto match = rules.resolve(L"codex-x86_64.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"specific"), match->ruleName);
        Assert::AreEqual(std::wstring(L"codex-specific.exe"), match->alias);
    }

    TEST_METHOD(matchingIsWholeStringNotPartial)
    {
        const RuleSet rules({AliasRule{L"exact", L"^codex\\.exe$", L"codex.exe", false}});

        Assert::IsFalse(rules.resolve(L"prefix-codex.exe-suffix").has_value());
        Assert::IsFalse(rules.resolve(L"notcodex.exe").has_value());
        Assert::IsTrue(rules.resolve(L"codex.exe").has_value());
    }

    TEST_METHOD(noMatchReturnsNullopt)
    {
        const RuleSet rules({AliasRule{L"only-codex", L"^codex\\.exe$", L"codex.exe", false}});
        Assert::IsFalse(rules.resolve(L"other.exe").has_value());
    }

    TEST_METHOD(ignoreCaseFlagMakesMatchingCaseInsensitive)
    {
        const RuleSet caseSensitive({AliasRule{L"cs", L"^codex\\.exe$", L"codex.exe", false}});
        Assert::IsFalse(caseSensitive.resolve(L"CODEX.EXE").has_value());

        const RuleSet caseInsensitive({AliasRule{L"ci", L"^codex\\.exe$", L"codex.exe", true}});
        Assert::IsTrue(caseInsensitive.resolve(L"CODEX.EXE").has_value());
    }

    TEST_METHOD(numberedCapturesAreSubstitutedIntoTheReplacement)
    {
        const RuleSet rules({
            AliasRule{L"strip-triple", L"^(.+?)-x86_64-pc-windows-msvc\\.exe$", L"$1.exe", false},
        });

        const auto match = rules.resolve(L"codex-x86_64-pc-windows-msvc.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), match->alias);
    }

    TEST_METHOD(aMatchProducingAnInvalidAliasIsTreatedAsNoMatch)
    {
        // The capture is empty, so the replacement would be just ".exe" - not a valid
        // alias file name. resolve() must not return that, and must not fall through to
        // a later rule either.
        const RuleSet rules({
            AliasRule{L"empty-capture", L"^(.*)\\.exe$", L"$1.exe", false},
            AliasRule{L"fallback", L"^\\.exe$", L"fallback.exe", false},
        });

        Assert::IsFalse(rules.resolve(L".exe").has_value());
    }

    TEST_METHOD(aReplacementNotEndingInExeIsTreatedAsNoMatch)
    {
        const RuleSet rules({AliasRule{L"bad-replacement", L"^codex\\.exe$", L"codex.dll", false}});
        Assert::IsFalse(rules.resolve(L"codex.exe").has_value());
    }
};

TEST_CLASS(RuleSetParseTests)
{
public:
    TEST_METHOD(aWellFormedDocumentParsesSuccessfully)
    {
        const auto rules = RuleSet::parse(LR"({
            "version": 1,
            "rules": [
                {
                    "name": "strip-rust-target-triple",
                    "pattern": "^(.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
                    "replacement": "$1.exe",
                    "flags": ["ignorecase"]
                }
            ]
        })");

        Assert::AreEqual(static_cast<std::size_t>(1), rules.size());
        const auto match = rules.resolve(L"codex-x86_64-pc-windows-msvc.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), match->alias);
    }

    TEST_METHOD(anEmptyRulesArrayParsesToAnEmptyRuleSet)
    {
        const auto rules = RuleSet::parse(LR"({"version": 1, "rules": []})");
        Assert::IsTrue(rules.isEmpty());
    }

    TEST_METHOD(malformedJsonTextIsRejected)
    {
        try
        {
            static_cast<void>(RuleSet::parse(L"{ not json"));
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError& error)
        {
            Assert::IsTrue(RuleSetErrorKind::ParseError == error.kind());
        }
    }

    TEST_METHOD(aRootThatIsNotAnObjectIsRejected)
    {
        Assert::ExpectException<RuleSetError>(
            [] { static_cast<void>(RuleSet::parse(LR"(["not", "an", "object"])")); });
    }

    TEST_METHOD(aMissingVersionFieldIsRejected)
    {
        try
        {
            static_cast<void>(RuleSet::parse(LR"({"rules": []})"));
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError& error)
        {
            Assert::IsTrue(RuleSetErrorKind::UnsupportedVersion == error.kind());
        }
    }

    TEST_METHOD(anUnsupportedVersionValueIsRejected)
    {
        Assert::ExpectException<RuleSetError>(
            [] { static_cast<void>(RuleSet::parse(LR"({"version": 2, "rules": []})")); });
    }

    TEST_METHOD(aMissingRulesFieldIsRejected)
    {
        Assert::ExpectException<RuleSetError>(
            [] { static_cast<void>(RuleSet::parse(LR"({"version": 1})")); });
    }

    TEST_METHOD(aRulesFieldThatIsNotAnArrayIsRejected)
    {
        Assert::ExpectException<RuleSetError>(
            [] { static_cast<void>(RuleSet::parse(LR"({"version": 1, "rules": {}})")); });
    }

    TEST_METHOD(aRuleMissingANameIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            static_cast<void>(RuleSet::parse(
                LR"({"version": 1, "rules": [{"pattern": "^a\\.exe$", "replacement": "a.exe"}]})"));
        });
    }

    TEST_METHOD(aRuleMissingAPatternIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            static_cast<void>(RuleSet::parse(
                LR"({"version": 1, "rules": [{"name": "n", "replacement": "a.exe"}]})"));
        });
    }

    TEST_METHOD(aRuleMissingAReplacementIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            static_cast<void>(
                RuleSet::parse(LR"({"version": 1, "rules": [{"name": "n", "pattern": "^a\\.exe$"}]})"));
        });
    }

    TEST_METHOD(anUnsupportedFlagIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            static_cast<void>(RuleSet::parse(
                LR"({"version": 1, "rules": [{"name": "n", "pattern": "^a\\.exe$", )"
                LR"("replacement": "a.exe", "flags": ["multiline"]}]})"));
        });
    }

    TEST_METHOD(aDuplicateRuleNameAcrossTwoRulesIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            static_cast<void>(RuleSet::parse(
                LR"({"version": 1, "rules": [)"
                LR"({"name": "dup", "pattern": "^a\\.exe$", "replacement": "a.exe"},)"
                LR"({"name": "dup", "pattern": "^b\\.exe$", "replacement": "b.exe"}]})"));
        });
    }

    TEST_METHOD(anInvalidRegexPatternInJsonIsRejected)
    {
        Assert::ExpectException<RuleSetError>([] {
            static_cast<void>(RuleSet::parse(
                LR"({"version": 1, "rules": [{"name": "n", "pattern": "^(unterminated", )"
                LR"("replacement": "a.exe"}]})"));
        });
    }

    TEST_METHOD(nonAsciiTextInTheDocumentIsAccepted)
    {
        const auto rules = RuleSet::parse(
            LR"({"version": 1, "rules": [{"name": "日本語", "pattern": "^a\\.exe$", )"
            LR"("replacement": "a.exe"}]})");

        Assert::AreEqual(static_cast<std::size_t>(1), rules.size());
    }
};
} // namespace syncwingetlink::tests
