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

    TEST_METHOD(junctionsAreNotFollowed)
    {
        const TempDirectory temp(L"scanner-reparse");
        const auto realDirectory = temp.createDirectory(L"real");
        temp.createFile(LR"(real\real.exe)");

        // A junction needs only write access to the parent directory, unlike a symlink
        // (Developer Mode / elevation), so this assertion runs unconditionally instead of
        // being silently skipped in a locked-down environment.
        Assert::IsTrue(createJunction(realDirectory, temp.path() / L"link"));
        Assert::IsTrue(isReparsePoint(temp.path() / L"link"));
        Assert::IsFalse(isReparsePoint(realDirectory));

        const auto executables = collectExecutables(temp.path());

        // The executable is found once through the real directory and never through the
        // junction, so a self-referencing link cannot produce duplicates or hang.
        Assert::AreEqual(static_cast<std::size_t>(1), executables.size());
        Assert::IsTrue(executables.front().path == realDirectory / L"real.exe");
    }

    TEST_METHOD(reparsePointCheckIsFalseForMissingPaths)
    {
        const TempDirectory temp(L"scanner-reparse-missing");

        Assert::IsFalse(isReparsePoint(temp.path() / L"absent"));
    }

    TEST_METHOD(resultPathsNeverCarryTheExtendedLengthPrefix)
    {
        const TempDirectory temp(L"scanner-prefix");
        temp.createFile(L"tool.exe");

        const auto executables = collectExecutables(temp.path());

        Assert::AreEqual(static_cast<std::size_t>(1), executables.size());
        const std::wstring resultPath = executables.front().path.native();
        Assert::IsFalse(resultPath.starts_with(LR"(\\?\)"));
        Assert::IsTrue(executables.front().path == temp.path() / L"tool.exe");
    }

    // #62: a nested non-ASCII directory/executable name (Japanese katakana + U+1F600,
    // a non-BMP character with no case distinction - see SmokeTests.cpp's matching
    // round-trip tests for why) is enumerated and its path preserved exactly. Written
    // with \uXXXX/\UXXXXXXXX escapes only; no raw non-ASCII bytes in this file.
    TEST_METHOD(nonAsciiNestedExecutableIsCollected)
    {
        const TempDirectory temp(L"scanner-non-ascii");
        const std::wstring directoryName =
            L"\u30C6\u30B9\u30C8\u30D1\u30C3\u30B1\u30FC\u30B8";
        const std::wstring fileName = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600.exe";
        const std::filesystem::path expected = temp.createFile(directoryName + L"\\" + fileName);

        const auto executables = collectExecutables(temp.path());

        Assert::AreEqual(static_cast<std::size_t>(1), executables.size());
        Assert::IsTrue(containsFileName(executables, fileName));
        Assert::IsTrue(executables.front().path == expected);
    }

    TEST_METHOD(resultsAreSortedByPath)
    {
        const TempDirectory temp(L"scanner-sort");
        temp.createFile(L"zeta.exe");
        temp.createFile(L"alpha.exe");
        temp.createFile(L"mid.exe");

        const auto executables = collectExecutables(temp.path());

        Assert::AreEqual(static_cast<std::size_t>(3), executables.size());
        Assert::IsTrue(std::is_sorted(executables.begin(), executables.end(),
                                      [](const PackageExe& a, const PackageExe& b) {
                                          return a.path < b.path;
                                      }));
    }
};
} // namespace syncwingetlink::tests
