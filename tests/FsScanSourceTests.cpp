// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/FsScanSource.h>

#include "TempDirectory.h"

#include <algorithm>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] const InstalledPackage* findById(const std::vector<InstalledPackage>& packages,
                                               const std::wstring& id)
{
    const auto match = std::find_if(packages.begin(), packages.end(),
                                    [&](const InstalledPackage& p) { return p.id == id; });
    return match == packages.end() ? nullptr : &*match;
}

constexpr std::wstring_view kSourceSuffix = L"_Microsoft.Winget.Source_8wekyb3d8bbwe";
} // namespace

TEST_CLASS(FsScanSourceTests)
{
public:
    TEST_METHOD(missingPackagesDirectoryYieldsNoPackages)
    {
        const TempDirectory temp(L"fsscan-missing");
        FsScanSource source(temp.path() / L"absent");

        Assert::IsTrue(source.enumeratePackages().empty());
    }

    TEST_METHOD(emptyPackagesDirectoryYieldsNoPackages)
    {
        const TempDirectory temp(L"fsscan-empty");
        FsScanSource source(temp.path());

        Assert::IsTrue(source.enumeratePackages().empty());
    }

    TEST_METHOD(packageIdIsDerivedFromTheDirectoryName)
    {
        Assert::IsTrue(packageIdFromDirectoryName(L"OpenAI.Codex" + std::wstring(kSourceSuffix)) ==
                       L"OpenAI.Codex");
        Assert::IsTrue(packageIdFromDirectoryName(L"BurntSushi.ripgrep.MSVC_Source_Hash") ==
                       L"BurntSushi.ripgrep.MSVC");
        Assert::IsTrue(packageIdFromDirectoryName(L"NoSeparator") == L"NoSeparator");
        Assert::IsTrue(packageIdFromDirectoryName(L"_LeadingSeparator").empty());
    }

    TEST_METHOD(eachPackageDirectoryBecomesOnePackage)
    {
        const TempDirectory temp(L"fsscan-packages");
        const std::wstring codexDirectory = L"OpenAI.Codex" + std::wstring(kSourceSuffix);
        const std::wstring fzfDirectory = L"junegunn.fzf" + std::wstring(kSourceSuffix);

        temp.createFile(std::filesystem::path(codexDirectory) / L"codex.exe");
        temp.createFile(std::filesystem::path(fzfDirectory) / L"fzf.exe");

        FsScanSource source(temp.path());
        const auto packages = source.enumeratePackages();

        Assert::AreEqual(static_cast<std::size_t>(2), packages.size());

        const InstalledPackage* codex = findById(packages, L"OpenAI.Codex");
        Assert::IsNotNull(codex);
        Assert::AreEqual(static_cast<std::size_t>(1), codex->executables.size());
        Assert::IsTrue(codex->executables.front().path.filename() == L"codex.exe");
        Assert::IsTrue(codex->installLocation.filename().native() == codexDirectory);
        Assert::IsNotNull(findById(packages, L"junegunn.fzf"));
    }

    TEST_METHOD(nestedExecutablesAreAttributedToTheirPackage)
    {
        const TempDirectory temp(L"fsscan-nested");
        const std::wstring directory = L"BurntSushi.ripgrep.MSVC" + std::wstring(kSourceSuffix);

        temp.createFile(std::filesystem::path(directory) /
                        L"ripgrep-15.2.0-x86_64-pc-windows-msvc" / L"rg.exe");
        temp.createFile(std::filesystem::path(directory) / (directory + L".db"));

        FsScanSource source(temp.path());
        const auto packages = source.enumeratePackages();

        Assert::AreEqual(static_cast<std::size_t>(1), packages.size());
        Assert::AreEqual(static_cast<std::size_t>(1), packages.front().executables.size());
        Assert::IsTrue(packages.front().executables.front().path.filename() == L"rg.exe");
    }

    TEST_METHOD(packagesWithoutExecutablesAreDropped)
    {
        const TempDirectory temp(L"fsscan-noexe");
        const std::wstring directory = L"Some.Package" + std::wstring(kSourceSuffix);

        temp.createFile(std::filesystem::path(directory) / L"README.md");

        FsScanSource source(temp.path());

        Assert::IsTrue(source.enumeratePackages().empty());
    }

    TEST_METHOD(looseFilesAtTheRootAreIgnored)
    {
        const TempDirectory temp(L"fsscan-loose");
        temp.createFile(L"stray.exe");

        FsScanSource source(temp.path());

        Assert::IsTrue(source.enumeratePackages().empty());
    }

    TEST_METHOD(versionIsUnknownWhenScanningTheFilesystem)
    {
        const TempDirectory temp(L"fsscan-version");
        temp.createFile(std::filesystem::path(L"Some.Package" + std::wstring(kSourceSuffix)) /
                        L"tool.exe");

        FsScanSource source(temp.path());
        const auto packages = source.enumeratePackages();

        Assert::AreEqual(static_cast<std::size_t>(1), packages.size());
        // The filesystem carries no version metadata; only the COM source can supply it.
        Assert::IsTrue(packages.front().version.empty());
    }

    TEST_METHOD(resultPathsNeverCarryTheExtendedLengthPrefix)
    {
        const TempDirectory temp(L"fsscan-prefix");
        const std::wstring directory = L"Some.Package" + std::wstring(kSourceSuffix);
        temp.createFile(std::filesystem::path(directory) / L"tool.exe");

        FsScanSource source(temp.path());
        const auto packages = source.enumeratePackages();

        Assert::AreEqual(static_cast<std::size_t>(1), packages.size());
        const std::wstring installLocation = packages.front().installLocation.native();
        Assert::IsFalse(installLocation.starts_with(LR"(\\?\)"));
        Assert::IsTrue(packages.front().installLocation == temp.path() / directory);

        const std::wstring exePath = packages.front().executables.front().path.native();
        Assert::IsFalse(exePath.starts_with(LR"(\\?\)"));
    }

    TEST_METHOD(resultsAreSortedById)
    {
        const TempDirectory temp(L"fsscan-sort");
        temp.createFile(std::filesystem::path(L"Zeta.Package" + std::wstring(kSourceSuffix)) /
                        L"z.exe");
        temp.createFile(std::filesystem::path(L"Alpha.Package" + std::wstring(kSourceSuffix)) /
                        L"a.exe");

        FsScanSource source(temp.path());
        const auto packages = source.enumeratePackages();

        Assert::AreEqual(static_cast<std::size_t>(2), packages.size());
        Assert::IsTrue(packages.front().id == L"Alpha.Package");
        Assert::IsTrue(packages.back().id == L"Zeta.Package");
    }
};
} // namespace syncwingetlink::tests
