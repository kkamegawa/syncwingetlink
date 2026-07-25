// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

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
};
} // namespace syncwingetlink::tests
