// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/Model.h>
#include <core/Paths.h>

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
};
} // namespace syncwingetlink::tests
