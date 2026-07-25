// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/Model.h>

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
};
} // namespace syncwingetlink::tests
