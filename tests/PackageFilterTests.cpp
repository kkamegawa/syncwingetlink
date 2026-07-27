// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/PackageFilter.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] InstalledPackage makePackage(std::wstring id,
                                           const std::vector<std::wstring>& executableNames)
{
    InstalledPackage package;
    package.id = std::move(id);
    package.name = package.id;
    package.installLocation = std::filesystem::path(LR"(C:\Packages)") / package.id;

    for (const std::wstring& name : executableNames)
    {
        package.executables.push_back(PackageExe{package.installLocation / name});
    }

    return package;
}

[[nodiscard]] const InstalledPackage* findById(const std::vector<InstalledPackage>& packages,
                                               const std::wstring& id)
{
    const auto match = std::find_if(packages.begin(), packages.end(),
                                    [&](const InstalledPackage& p) { return p.id == id; });
    return match == packages.end() ? nullptr : &*match;
}
} // namespace

TEST_CLASS(MatchesGlobTests)
{
public:
    TEST_METHOD(literalPatternsMatchOnlyTheSameText)
    {
        Assert::IsTrue(matchesGlob(L"codex.exe", L"codex.exe"));
        Assert::IsFalse(matchesGlob(L"codex.exe", L"codex"));
        Assert::IsFalse(matchesGlob(L"codex", L"codex.exe"));
        Assert::IsFalse(matchesGlob(L"codex.exe", L"rg.exe"));
    }

    TEST_METHOD(anEmptyPatternMatchesOnlyAnEmptyValue)
    {
        Assert::IsTrue(matchesGlob(L"", L""));
        Assert::IsFalse(matchesGlob(L"", L"codex.exe"));
    }

    TEST_METHOD(aLoneStarMatchesEverythingIncludingNothing)
    {
        Assert::IsTrue(matchesGlob(L"*", L""));
        Assert::IsTrue(matchesGlob(L"*", L"codex.exe"));
        Assert::IsTrue(matchesGlob(L"***", L"codex.exe"));
    }

    TEST_METHOD(starMatchesAtEitherEndAndInTheMiddle)
    {
        Assert::IsTrue(matchesGlob(L"codex*", L"codex-x86_64-pc-windows-msvc.exe"));
        Assert::IsTrue(matchesGlob(L"*.exe", L"codex.exe"));
        Assert::IsTrue(matchesGlob(L"codex*.exe", L"codex-x86_64-pc-windows-msvc.exe"));
        Assert::IsTrue(matchesGlob(L"*windows*", L"codex-x86_64-pc-windows-msvc.exe"));
        Assert::IsFalse(matchesGlob(L"codex*.exe", L"ripgrep.exe"));
    }

    TEST_METHOD(starCanMatchAnEmptyRun)
    {
        Assert::IsTrue(matchesGlob(L"codex*.exe", L"codex.exe"));
        Assert::IsTrue(matchesGlob(L"*codex.exe", L"codex.exe"));
    }

    TEST_METHOD(multipleStarsBacktrackCorrectly)
    {
        Assert::IsTrue(matchesGlob(L"*a*b*c*", L"xxaxxbxxcxx"));
        Assert::IsFalse(matchesGlob(L"*a*b*c*", L"xxaxxcxxbxx"));
        // The naive greedy match fails this one: the first '*' must give a character back.
        Assert::IsTrue(matchesGlob(L"*abc", L"abcabc"));
    }

    TEST_METHOD(questionMarkMatchesExactlyOneCharacter)
    {
        Assert::IsTrue(matchesGlob(L"r?.exe", L"rg.exe"));
        Assert::IsFalse(matchesGlob(L"r?.exe", L"r.exe"));
        Assert::IsFalse(matchesGlob(L"r?.exe", L"rip.exe"));
        Assert::IsTrue(matchesGlob(L"???", L"abc"));
    }

    TEST_METHOD(matchingIsCaseInsensitive)
    {
        Assert::IsTrue(matchesGlob(L"CODEX.EXE", L"codex.exe"));
        Assert::IsTrue(matchesGlob(L"openai.codex", L"OpenAI.Codex"));
        Assert::IsTrue(matchesGlob(L"*.EXE", L"codex.exe"));
    }

    TEST_METHOD(charactersWithNoCaseMappingStillCompareByValue)
    {
        Assert::IsTrue(matchesGlob(L"\u65E5\u672C\u8A9E.exe", L"\u65E5\u672C\u8A9E.exe"));
        Assert::IsFalse(matchesGlob(L"\u65E5\u672C\u8A9E.exe", L"\u4E2D\u56FD\u8A9E.exe"));
    }
};

TEST_CLASS(PackageFilterTests)
{
public:
    TEST_METHOD(aFilterWithNoPatternsIsTheIdentity)
    {
        const PackageFilter filter;
        Assert::IsTrue(filter.isEmpty());

        std::vector<InstalledPackage> packages{makePackage(L"OpenAI.Codex", {L"codex.exe"}),
                                               makePackage(L"junegunn.fzf", {L"fzf.exe"})};

        const auto filtered = filter.apply(packages);

        Assert::AreEqual(static_cast<std::size_t>(2), filtered.size());
    }

    TEST_METHOD(includeMatchesThePackageIdentifier)
    {
        const PackageFilter filter({L"OpenAI.*"}, {});

        std::vector<InstalledPackage> packages{makePackage(L"OpenAI.Codex", {L"codex.exe"}),
                                               makePackage(L"junegunn.fzf", {L"fzf.exe"})};

        const auto filtered = filter.apply(std::move(packages));

        Assert::AreEqual(static_cast<std::size_t>(1), filtered.size());
        Assert::IsTrue(filtered.front().id == L"OpenAI.Codex");
    }

    TEST_METHOD(includeMatchesTheExecutableFileName)
    {
        const PackageFilter filter({L"fzf.exe"}, {});

        std::vector<InstalledPackage> packages{makePackage(L"OpenAI.Codex", {L"codex.exe"}),
                                               makePackage(L"junegunn.fzf", {L"fzf.exe"})};

        const auto filtered = filter.apply(std::move(packages));

        Assert::AreEqual(static_cast<std::size_t>(1), filtered.size());
        Assert::IsTrue(filtered.front().id == L"junegunn.fzf");
    }

    TEST_METHOD(filteringIsPerExecutableNotPerPackage)
    {
        const PackageFilter filter({L"rg.exe"}, {});

        std::vector<InstalledPackage> packages{
            makePackage(L"BurntSushi.ripgrep.MSVC", {L"rg.exe", L"rg-debug.exe"})};

        const auto filtered = filter.apply(std::move(packages));

        Assert::AreEqual(static_cast<std::size_t>(1), filtered.size());
        Assert::AreEqual(static_cast<std::size_t>(1), filtered.front().executables.size());
        Assert::IsTrue(filtered.front().executables.front().path.filename() == L"rg.exe");
    }

    TEST_METHOD(excludeRemovesAMatchingExecutable)
    {
        const PackageFilter filter({}, {L"*-debug.exe"});

        std::vector<InstalledPackage> packages{
            makePackage(L"BurntSushi.ripgrep.MSVC", {L"rg.exe", L"rg-debug.exe"})};

        const auto filtered = filter.apply(std::move(packages));

        Assert::AreEqual(static_cast<std::size_t>(1), filtered.size());
        Assert::AreEqual(static_cast<std::size_t>(1), filtered.front().executables.size());
        Assert::IsTrue(filtered.front().executables.front().path.filename() == L"rg.exe");
    }

    TEST_METHOD(excludeWinsOverInclude)
    {
        const PackageFilter filter({L"OpenAI.*"}, {L"codex.exe"});

        Assert::IsFalse(filter.includesExecutable(L"OpenAI.Codex", L"codex.exe"));

        std::vector<InstalledPackage> packages{makePackage(L"OpenAI.Codex", {L"codex.exe"})};

        Assert::IsTrue(filter.apply(std::move(packages)).empty());
    }

    TEST_METHOD(packagesLeftWithNoExecutablesAreDropped)
    {
        const PackageFilter filter({L"codex.exe"}, {});

        std::vector<InstalledPackage> packages{makePackage(L"OpenAI.Codex", {L"codex.exe"}),
                                               makePackage(L"junegunn.fzf", {L"fzf.exe"})};

        const auto filtered = filter.apply(std::move(packages));

        Assert::AreEqual(static_cast<std::size_t>(1), filtered.size());
        Assert::IsNull(findById(filtered, L"junegunn.fzf"));
    }

    TEST_METHOD(severalIncludePatternsAreOred)
    {
        const PackageFilter filter({L"codex.exe", L"fzf.exe"}, {});

        std::vector<InstalledPackage> packages{makePackage(L"OpenAI.Codex", {L"codex.exe"}),
                                               makePackage(L"junegunn.fzf", {L"fzf.exe"}),
                                               makePackage(L"Other.Tool", {L"other.exe"})};

        const auto filtered = filter.apply(std::move(packages));

        Assert::AreEqual(static_cast<std::size_t>(2), filtered.size());
        Assert::IsNull(findById(filtered, L"Other.Tool"));
    }

    TEST_METHOD(patternsAreMatchedCaseInsensitively)
    {
        const PackageFilter filter({L"openai.*"}, {});

        Assert::IsTrue(filter.includesExecutable(L"OpenAI.Codex", L"codex.exe"));
    }

    TEST_METHOD(patternsMatchTheFileNameNotTheWholePath)
    {
        // The install location is C:\Packages\..., so a pattern anchored on the directory
        // must not match: only the bare file name is offered to the matcher.
        const PackageFilter filter({L"C:\\Packages\\*"}, {});

        std::vector<InstalledPackage> packages{makePackage(L"OpenAI.Codex", {L"codex.exe"})};

        Assert::IsTrue(filter.apply(std::move(packages)).empty());
    }
};
} // namespace syncwingetlink::tests
