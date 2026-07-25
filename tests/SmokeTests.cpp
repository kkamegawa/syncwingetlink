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
