// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/AliasResolver.h>
#include <rules/DefaultRules.h>
#include <rules/RuleSetSelector.h>

#include "TempDirectory.h"

#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
void writeUtf8File(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream stream(path, std::ios::binary);
    stream << content;
}

struct AliasCase
{
    std::wstring fileName;
    std::wstring expectedAlias;
};
} // namespace

// End-to-end coverage tying RuleSetSelector, DefaultRules, and AliasResolver together -
// each of those is unit tested on its own (RuleSetSelectorTests, DefaultRulesTests,
// AliasResolverTests), but nothing else exercises the full pipeline a real scan performs:
// select a RuleSet, then resolve an alias from a real executable path through it. This is
// the "cross-component regression matrix" called for in the M3 plan.
TEST_CLASS(AliasPipelineTests)
{
public:
    TEST_METHOD(representativeRealWorldExecutablesResolveCorrectlyWithNoRulesFileConfigured)
    {
        const TempDirectory temp(L"pipeline-no-rules-file");
        const RuleSet rules =
            selectRuleSet(std::nullopt, [&temp] { return temp.path() / L"absent.json"; });

        const std::vector<AliasCase> cases = {
            {L"codex-x86_64-pc-windows-msvc.exe", L"codex.exe"},
            {L"codex-aarch64-pc-windows-msvc.exe", L"codex.exe"},
            {L"codex-x86_64-pc-windows-gnu.exe", L"codex.exe"},
            {L"restic_0.15.2_windows_amd64.exe", L"restic.exe"},
            // No version-like substring at all: falls all the way through to the raw
            // file name, exercising AliasResolver's tier 3 with defaultRules() specifically
            // (not just the standalone RuleSet used in AliasResolverTests).
            {L"jq.exe", L"jq.exe"},
        };

        for (const AliasCase& testCase : cases)
        {
            const auto resolution =
                resolveAlias(std::filesystem::path(L"C:\\pkg\\") / testCase.fileName, rules);
            Assert::IsTrue(resolution.has_value());
            Assert::AreEqual(testCase.expectedAlias, resolution->alias);
        }
    }

    TEST_METHOD(aUserRuleSetCompletelyReplacesEmbeddedDefaultsRatherThanMerging)
    {
        // The user file below defines only a fixed-mapping rule (docs/rules.md's
        // "map-kubelogin" sample), with none of the embedded Rust-target-triple or
        // version/arch rules. selectRuleSet() must not merge the two rule sources: once a
        // user file is selected, resolveAlias() sees exactly its rules, not defaultRules()
        // plus its rules.
        const TempDirectory temp(L"pipeline-user-replaces-embedded");
        const std::filesystem::path userFile = temp.path() / L"rules.json";
        writeUtf8File(userFile,
                     R"({"version": 1, "rules": [{"name": "map-kubelogin", )"
                     R"("pattern": "^kubelogin.*\\.exe$", )"
                     R"("replacement": "kubectl-oidc_login.exe"}]})");

        const RuleSet rules = selectRuleSet(std::nullopt, [&userFile] { return userFile; });

        const auto kubelogin =
            resolveAlias(std::filesystem::path(LR"(C:\pkg\kubelogin-win-amd64.exe)"), rules);
        Assert::IsTrue(kubelogin.has_value());
        Assert::AreEqual(std::wstring(L"kubectl-oidc_login.exe"), kubelogin->alias);

        // The embedded Rust-target-triple rule is NOT present in this user RuleSet, so
        // this must fall back to the raw file name rather than being rewritten to
        // "codex.exe" - proof that embedded defaults were replaced, not merged in.
        const auto codex = resolveAlias(
            std::filesystem::path(LR"(C:\pkg\codex-x86_64-pc-windows-msvc.exe)"), rules);
        Assert::IsTrue(codex.has_value());
        Assert::AreEqual(std::wstring(L"codex-x86_64-pc-windows-msvc.exe"), codex->alias);
        Assert::IsFalse(codex->matchedRuleName.has_value());
    }

    TEST_METHOD(anExplicitRulesPathWinsEndToEndEvenWithADifferingUserFilePresent)
    {
        const TempDirectory temp(L"pipeline-explicit-wins");
        const std::filesystem::path explicitFile = temp.path() / L"explicit.json";
        const std::filesystem::path userFile = temp.path() / L"user.json";

        writeUtf8File(explicitFile,
                     R"({"version": 1, "rules": [{"name": "explicit-rule", )"
                     R"("pattern": "^tool\\.exe$", "replacement": "from-explicit.exe"}]})");
        writeUtf8File(userFile,
                     R"({"version": 1, "rules": [{"name": "user-rule", )"
                     R"("pattern": "^tool\\.exe$", "replacement": "from-user.exe"}]})");

        const RuleSet rules = selectRuleSet(explicitFile, [&userFile] { return userFile; });

        const auto resolution = resolveAlias(std::filesystem::path(LR"(C:\pkg\tool.exe)"), rules);
        Assert::IsTrue(resolution.has_value());
        Assert::AreEqual(std::wstring(L"from-explicit.exe"), resolution->alias);
    }

    TEST_METHOD(aMalformedUserFileFailsTheWholePipelineRatherThanSilentlyUsingDefaults)
    {
        const TempDirectory temp(L"pipeline-malformed-user");
        const std::filesystem::path malformed = temp.path() / L"rules.json";
        writeUtf8File(malformed, "{ this is not valid json");

        try
        {
            static_cast<void>(selectRuleSet(std::nullopt, [&malformed] { return malformed; }));
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError&)
        {
            // Expected: no RuleSet is produced at all, so a caller further up the
            // pipeline (the eventual M6 CLI) cannot proceed to scan with a half-applied
            // configuration.
        }
    }
};
} // namespace syncwingetlink::tests
