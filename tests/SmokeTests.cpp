// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/Model.h>
#include <core/Paths.h>

#include <stdexcept>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
TEST_CLASS(SmokeTests)
{
public:
    TEST_METHOD(frameworkLoads)
    {
        Assert::IsTrue(true);
    }

    TEST_METHOD(modelDefaultsAreSafe)
    {
        const AppOptions options;
        const RepairItem repairItem;

        Assert::IsTrue(options.command == AppCommand::Scan);
        Assert::IsTrue(options.source == PackageSource::Auto);
        Assert::IsTrue(options.logLevel == LogLevel::Normal);
        Assert::IsFalse(options.dryRun);
        Assert::IsTrue(repairItem.status == LinkStatus::Missing);
    }

    TEST_METHOD(pathOverridesAreUsedVerbatim)
    {
        const std::filesystem::path overridePath = LR"(X:\custom\winget)";

        Assert::IsTrue(paths::getLinksDirectory(overridePath) == overridePath);
        Assert::IsTrue(paths::getPackagesDirectory(overridePath) == overridePath);
    }

    TEST_METHOD(defaultPathsUseLocalAppData)
    {
        const auto localAppData = paths::getLocalAppDataDirectory();
        const auto links = paths::getLinksDirectory();
        const auto packages = paths::getPackagesDirectory();

        Assert::IsFalse(localAppData.empty());
        Assert::IsTrue(links == localAppData / L"Microsoft" / L"WinGet" / L"Links");
        Assert::IsTrue(packages == localAppData / L"Microsoft" / L"WinGet" / L"Packages");
    }

    TEST_METHOD(longPathsAreNormalizedForWin32)
    {
        Assert::IsTrue(paths::toExtendedLengthPath(LR"(C:\tools\..\bin\tool.exe)") ==
                       LR"(\\?\C:\bin\tool.exe)");
        Assert::IsTrue(paths::toExtendedLengthPath(LR"(\\server\share\tool.exe)") ==
                       LR"(\\?\UNC\server\share\tool.exe)");
        Assert::IsTrue(paths::toExtendedLengthPath(LR"(\\?\C:\already\extended.exe)") ==
                       LR"(\\?\C:\already\extended.exe)");
        Assert::IsTrue(paths::toExtendedLengthPath(LR"(\\.\pipe\device)") ==
                       LR"(\\.\pipe\device)");
    }

    TEST_METHOD(emptyLongPathIsRejected)
    {
        Assert::ExpectException<std::invalid_argument>(
            [] { static_cast<void>(paths::toExtendedLengthPath({})); });
    }

    TEST_METHOD(relativeLongPathIsMadeAbsolute)
    {
        const std::filesystem::path relative = LR"(relative\tool.exe)";

        std::filesystem::path expected = std::filesystem::absolute(relative).lexically_normal();
        expected.make_preferred();

        Assert::IsTrue(paths::toExtendedLengthPath(relative) ==
                       std::filesystem::path(LR"(\\?\)" + expected.native()));
    }

    TEST_METHOD(extendedLengthPrefixIsStripped)
    {
        Assert::IsTrue(paths::fromExtendedLengthPath(LR"(\\?\C:\bin\tool.exe)") ==
                       LR"(C:\bin\tool.exe)");
        Assert::IsTrue(paths::fromExtendedLengthPath(LR"(\\?\UNC\server\share\tool.exe)") ==
                       LR"(\\server\share\tool.exe)");
        Assert::IsTrue(paths::fromExtendedLengthPath(LR"(C:\already\plain.exe)") ==
                       LR"(C:\already\plain.exe)");
        Assert::IsTrue(paths::fromExtendedLengthPath(LR"(\\.\pipe\device)") ==
                       LR"(\\.\pipe\device)");
    }

    TEST_METHOD(extendedLengthPrefixRoundTrips)
    {
        const std::filesystem::path original = LR"(C:\tools\bin\tool.exe)";

        Assert::IsTrue(
            paths::fromExtendedLengthPath(paths::toExtendedLengthPath(original)) == original);
    }

    // #62: non-ASCII round-trip coverage. The fixture name combines Japanese katakana
    // (no case distinction at all) with U+1F600 GRINNING FACE, a non-BMP character
    // chosen specifically because it also has no case distinction - sidestepping any
    // mismatch between Console.cpp's CompareStringOrdinal-based comparisons and NTFS's
    // own $UpCase case-folding table, which are not guaranteed to agree for a
    // case-sensitive script. Written with \uXXXX/\UXXXXXXXX escapes only, per this
    // project's rule that test sources carry no raw non-ASCII bytes; \U (8-digit) is
    // used for the non-BMP character specifically because a pair of \u (4-digit)
    // escapes naming individual surrogate code points is ill-formed - see this file's
    // own doc comment in docs/task.md's issue #62 entry for the full reasoning.
    TEST_METHOD(extendedLengthPrefixRoundTripsNonAsciiAbsolutePath)
    {
        // Already absolute, so this exercises toExtendedLengthPath's pure-lexical
        // branch (lexically_normal()+make_preferred(), no GetFullPathNameW call) -
        // src/core/Paths.cpp's is_absolute() ? path : std::filesystem::absolute(path).
        const std::filesystem::path original =
            LR"(C:\tools\)" L"\u30C6\u30B9\u30C8\u30D1\u30C3\u30B1\u30FC\u30B8\U0001F600.exe";

        Assert::IsTrue(
            paths::fromExtendedLengthPath(paths::toExtendedLengthPath(original)) == original);
    }

    TEST_METHOD(extendedLengthPrefixRoundTripsNonAsciiRelativePath)
    {
        // Same fixture name, passed as relative input this time - exercises the other
        // branch of toExtendedLengthPath (std::filesystem::absolute(), a real
        // GetFullPathNameW call), which the already-absolute test above cannot reach.
        const std::filesystem::path relative =
            L"\u30C6\u30B9\u30C8\u30D1\u30C3\u30B1\u30FC\u30B8\U0001F600.exe";

        std::filesystem::path expectedAbsolute =
            std::filesystem::absolute(relative).lexically_normal();
        expectedAbsolute.make_preferred();

        const std::filesystem::path extended = paths::toExtendedLengthPath(relative);
        Assert::IsTrue(extended == std::filesystem::path(LR"(\\?\)" + expectedAbsolute.native()));
        Assert::IsTrue(paths::fromExtendedLengthPath(extended) == expectedAbsolute);
    }

    TEST_METHOD(relativeLongPathUnderExtendedCurrentDirectoryStaysExtended)
    {
        const auto originalCurrentPath = std::filesystem::current_path();
        struct CurrentPathGuard
        {
            std::filesystem::path original;
            ~CurrentPathGuard()
            {
                std::filesystem::current_path(original);
            }
        } guard{originalCurrentPath};

        std::filesystem::current_path(
            std::filesystem::path(LR"(\\?\)" + originalCurrentPath.native()));

        const std::wstring result =
            paths::toExtendedLengthPath(LR"(relative\tool.exe)").native();

        Assert::IsTrue(result.starts_with(LR"(\\?\)"));
        Assert::IsFalse(result.starts_with(LR"(\\?\UNC\)"));
    }
};
} // namespace syncwingetlink::tests
