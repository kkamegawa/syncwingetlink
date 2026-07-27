// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/AliasResolver.h>
#include <rules/DefaultRules.h>

#include <filesystem>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
TEST_CLASS(AliasResolverTests)
{
public:
    TEST_METHOD(aMatchingRuleWinsOverTheRawFileName)
    {
        const RuleSet rules({AliasRule{L"strip-triple", L"^(.+?)-x86_64\\.exe$", L"$1.exe", false}});

        const auto resolution =
            resolveAlias(std::filesystem::path(LR"(C:\pkg\codex-x86_64.exe)"), rules);

        Assert::IsTrue(resolution.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), resolution->alias);
        Assert::IsTrue(resolution->matchedRuleName.has_value());
        Assert::AreEqual(std::wstring(L"strip-triple"), *resolution->matchedRuleName);
    }

    TEST_METHOD(theCodexRustTargetTripleResolvesThroughTheEmbeddedDefaults)
    {
        const RuleSet rules = defaultRules();

        const auto resolution = resolveAlias(
            std::filesystem::path(LR"(C:\Users\me\AppData\Local\Microsoft\WinGet\Packages\)"
                                  LR"(OpenAI.Codex\codex-x86_64-pc-windows-msvc.exe)"),
            rules);

        Assert::IsTrue(resolution.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), resolution->alias);
        Assert::IsTrue(resolution->matchedRuleName.has_value());
        Assert::AreEqual(std::wstring(L"strip-rust-target-triple"), *resolution->matchedRuleName);
    }

    TEST_METHOD(noMatchingRuleFallsBackToTheRawFileName)
    {
        const RuleSet rules({AliasRule{L"only-x86_64", L"^codex-x86_64\\.exe$", L"codex.exe", false}});

        const auto resolution = resolveAlias(std::filesystem::path(LR"(C:\pkg\jq.exe)"), rules);

        Assert::IsTrue(resolution.has_value());
        Assert::AreEqual(std::wstring(L"jq.exe"), resolution->alias);
        Assert::IsFalse(resolution->matchedRuleName.has_value());
    }

    TEST_METHOD(anEmptyRuleSetAlwaysFallsBackToTheRawFileName)
    {
        const RuleSet rules;
        const auto resolution = resolveAlias(std::filesystem::path(LR"(C:\pkg\anything.exe)"), rules);

        Assert::IsTrue(resolution.has_value());
        Assert::AreEqual(std::wstring(L"anything.exe"), resolution->alias);
        Assert::IsFalse(resolution->matchedRuleName.has_value());
    }

    TEST_METHOD(aMatchProducingAnInvalidAliasFallsBackToTheRawFileNameItself)
    {
        // The rule matches "myapp.exe" (an otherwise perfectly valid raw file name) but
        // its capture is empty, producing just ".exe" - invalid. resolveAlias must fall
        // back to the raw file name "myapp.exe", not propagate the invalid alias.
        const RuleSet rules({AliasRule{L"empty-capture", L"^myapp(.*)\\.exe$", L"$1.exe", false}});

        const auto resolution = resolveAlias(std::filesystem::path(LR"(C:\pkg\myapp.exe)"), rules);

        Assert::IsTrue(resolution.has_value());
        Assert::AreEqual(std::wstring(L"myapp.exe"), resolution->alias);
        Assert::IsFalse(resolution->matchedRuleName.has_value());
    }

    TEST_METHOD(aRawFileNameThatIsNotWellFormedIsRejectedEntirely)
    {
        // ".exe" alone (empty stem) is not a well-formed alias file name, and no rule
        // matches it either, so resolution must fail outright rather than propagating an
        // unusable alias.
        const RuleSet rules;
        const auto resolution = resolveAlias(std::filesystem::path(LR"(C:\pkg\.exe)"), rules);

        Assert::IsFalse(resolution.has_value());
    }

    TEST_METHOD(aRawFileNameThatIsAReservedDeviceNameIsRejectedEntirely)
    {
        // "CON.exe" is syntactically a well-formed *.exe name (non-empty stem, correct
        // extension, no path separators) but Windows reserves "CON" as a device name for
        // every directory regardless of extension - CreateFileW would refuse to create
        // this symlink later. Neither tier produces a usable alias, so resolution must
        // fail outright here too, not just for the empty-stem case above.
        const RuleSet rules;
        const auto resolution = resolveAlias(std::filesystem::path(LR"(C:\pkg\CON.exe)"), rules);

        Assert::IsFalse(resolution.has_value());
    }

    TEST_METHOD(aRuleProducingAReservedDeviceNameFallsBackToTheValidRawFileName)
    {
        // The rule matches "printer-tool.exe" (a perfectly valid raw file name) but its
        // replacement is the reserved name "PRN.exe" - resolveAlias must fall back to the
        // raw file name rather than propagate an alias that can never be created.
        const RuleSet rules(
            {AliasRule{L"bad-replacement", L"^printer-tool\\.exe$", L"PRN.exe", false}});

        const auto resolution =
            resolveAlias(std::filesystem::path(LR"(C:\pkg\printer-tool.exe)"), rules);

        Assert::IsTrue(resolution.has_value());
        Assert::AreEqual(std::wstring(L"printer-tool.exe"), resolution->alias);
        Assert::IsFalse(resolution->matchedRuleName.has_value());
    }
};
} // namespace syncwingetlink::tests
