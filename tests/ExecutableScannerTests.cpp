// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/ExecutableScanner.h>

#include "TempDirectory.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] bool containsFileName(const std::vector<PackageExe>& executables,
                                    const std::wstring& fileName)
{
    return std::any_of(executables.begin(), executables.end(), [&](const PackageExe& exe) {
        return exe.path.filename().native() == fileName;
    });
}
} // namespace

TEST_CLASS(ExecutableScannerTests)
{
public:
    TEST_METHOD(missingRootYieldsNoExecutables)
    {
        const TempDirectory temp(L"scanner-missing");

        Assert::IsTrue(collectExecutables(temp.path() / L"does-not-exist").empty());
    }

    TEST_METHOD(emptyRootIsRejectedWithoutThrowing)
    {
        Assert::IsTrue(collectExecutables({}).empty());
    }

    TEST_METHOD(nestedExecutablesAreCollected)
    {
        const TempDirectory temp(L"scanner-nested");
        temp.createFile(L"tool.exe");
        temp.createFile(LR"(bin\nested.exe)");
        temp.createFile(LR"(bin\deeper\deep.exe)");

        const auto executables = collectExecutables(temp.path());

        Assert::AreEqual(static_cast<std::size_t>(3), executables.size());
        Assert::IsTrue(containsFileName(executables, L"tool.exe"));
        Assert::IsTrue(containsFileName(executables, L"nested.exe"));
        Assert::IsTrue(containsFileName(executables, L"deep.exe"));
    }

    TEST_METHOD(nonExecutableFilesAreIgnored)
    {
        const TempDirectory temp(L"scanner-filter");
        temp.createFile(L"tool.exe");
        temp.createFile(L"readme.md");
        temp.createFile(L"library.dll");
        temp.createFile(L"index.db");

        const auto executables = collectExecutables(temp.path());

        Assert::AreEqual(static_cast<std::size_t>(1), executables.size());
        Assert::IsTrue(containsFileName(executables, L"tool.exe"));
    }

    TEST_METHOD(executableExtensionMatchIsCaseInsensitive)
    {
        const TempDirectory temp(L"scanner-case");
        temp.createFile(L"upper.EXE");
        temp.createFile(L"mixed.Exe");

        Assert::AreEqual(static_cast<std::size_t>(2), collectExecutables(temp.path()).size());
    }

    TEST_METHOD(metadataAliasIsNotSuppliedByTheFilesystem)
    {
        const TempDirectory temp(L"scanner-alias");
        temp.createFile(L"tool.exe");

        const auto executables = collectExecutables(temp.path());

        Assert::AreEqual(static_cast<std::size_t>(1), executables.size());
        Assert::IsFalse(executables.front().metadataAlias.has_value());
    }

    TEST_METHOD(directoriesBeyondTheDepthCapAreNotWalked)
    {
        const TempDirectory temp(L"scanner-depth");

        std::filesystem::path relative;
        for (int level = 0; level <= kMaxScanDepth + 2; ++level)
        {
            relative /= L"level";
        }
        temp.createFile(relative / L"tooDeep.exe");
        temp.createFile(L"shallow.exe");

        const auto executables = collectExecutables(temp.path());

        Assert::IsTrue(containsFileName(executables, L"shallow.exe"));
        Assert::IsFalse(containsFileName(executables, L"tooDeep.exe"));
    }

    TEST_METHOD(reparsePointsAreNotFollowed)
    {
        const TempDirectory temp(L"scanner-reparse");
        const auto realDirectory = temp.createDirectory(L"real");
        temp.createFile(LR"(real\real.exe)");

        std::error_code error;
        std::filesystem::create_directory_symlink(realDirectory, temp.path() / L"link", error);
        if (error)
        {
            // Creating a symlink needs Developer Mode or elevation. Report the skip rather
            // than pretending the traversal guard was exercised.
            Logger::WriteMessage("reparsePointsAreNotFollowed: symlink creation unavailable, "
                                 "loop guard assertion skipped\n");
            return;
        }

        Assert::IsTrue(isReparsePoint(temp.path() / L"link"));
        Assert::IsFalse(isReparsePoint(realDirectory));

        const auto executables = collectExecutables(temp.path());

        // The executable is found once through the real directory and never through the
        // symlink, so a self-referencing link cannot produce duplicates or hang.
        Assert::AreEqual(static_cast<std::size_t>(1), executables.size());
    }

    TEST_METHOD(reparsePointCheckIsFalseForMissingPaths)
    {
        const TempDirectory temp(L"scanner-reparse-missing");

        Assert::IsFalse(isReparsePoint(temp.path() / L"absent"));
    }
};
} // namespace syncwingetlink::tests
