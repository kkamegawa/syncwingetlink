// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <rules/DefaultRules.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
TEST_CLASS(DefaultRulesTests)
{
public:
    TEST_METHOD(rustTargetTripleSuffixesAreStripped)
    {
        const RuleSet rules = defaultRules();

        const auto x64Msvc = rules.resolve(L"codex-x86_64-pc-windows-msvc.exe");
        Assert::IsTrue(x64Msvc.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), x64Msvc->alias);
        Assert::AreEqual(std::wstring(L"strip-rust-target-triple"), x64Msvc->ruleName);

        const auto arm64Msvc = rules.resolve(L"codex-aarch64-pc-windows-msvc.exe");
        Assert::IsTrue(arm64Msvc.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), arm64Msvc->alias);

        const auto x64Gnu = rules.resolve(L"codex-x86_64-pc-windows-gnu.exe");
        Assert::IsTrue(x64Gnu.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), x64Gnu->alias);

        const auto i686Msvc = rules.resolve(L"codex-i686-pc-windows-msvc.exe");
        Assert::IsTrue(i686Msvc.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), i686Msvc->alias);
    }

    TEST_METHOD(rustTargetTripleMatchingIsCaseInsensitive)
    {
        const RuleSet rules = defaultRules();
        const auto match = rules.resolve(L"codex-X86_64-PC-WINDOWS-MSVC.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), match->alias);
    }

    TEST_METHOD(versionAndArchSuffixesAreStripped)
    {
        const RuleSet rules = defaultRules();

        // The documented docs/rules.md example.
        const auto restic = rules.resolve(L"restic_0.15.2_windows_amd64.exe");
        Assert::IsTrue(restic.has_value());
        Assert::AreEqual(std::wstring(L"restic.exe"), restic->alias);
        Assert::AreEqual(std::wstring(L"strip-version-and-arch"), restic->ruleName);

        const auto withV = rules.resolve(L"mytool-v1.2.0-win-arm64.exe");
        Assert::IsTrue(withV.has_value());
        Assert::AreEqual(std::wstring(L"mytool.exe"), withV->alias);

        const auto noArch = rules.resolve(L"widget-2.0.exe");
        Assert::IsTrue(noArch.has_value());
        Assert::AreEqual(std::wstring(L"widget.exe"), noArch->alias);
    }

    // #38's negative test: the version/arch rule is deliberately broad (every group after
    // the version number is optional), so it must not be assumed safe without a check that
    // it leaves ordinary file names - ones with no version-like substring at all - alone.
    TEST_METHOD(fileNamesWithoutAVersionLikeSubstringAreNotRewritten)
    {
        const RuleSet rules = defaultRules();

        Assert::IsFalse(rules.resolve(L"jq.exe").has_value());
        Assert::IsFalse(rules.resolve(L"kubectl.exe").has_value());
        // A single integer with no fractional part does not satisfy \d+\.\d+.
        Assert::IsFalse(rules.resolve(L"app-3.exe").has_value());
    }

    TEST_METHOD(theRustTargetTripleRuleIsTriedBeforeTheVersionAndArchRule)
    {
        // codex-x86_64-pc-windows-msvc.exe contains no \d+\.\d+ substring, so this does
        // not by itself prove ordering, but pins the expectation down explicitly: the more
        // specific rule must win when both could plausibly apply to a related name.
        const RuleSet rules = defaultRules();
        const auto match = rules.resolve(L"codex-x86_64-pc-windows-msvc.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"strip-rust-target-triple"), match->ruleName);
    }
};
} // namespace syncwingetlink::tests
