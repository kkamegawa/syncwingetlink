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
};
} // namespace syncwingetlink::tests
