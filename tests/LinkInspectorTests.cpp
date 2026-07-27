// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/LinkInspector.h>

#include <filesystem>
#include <stdexcept>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] PackageExe makeExecutable()
{
    PackageExe exe;
    exe.path = LR"(C:\Packages\Codex\codex-x86_64-pc-windows-msvc.exe)";
    return exe;
}

constexpr std::wstring_view kAlias = L"codex.exe";
constexpr std::wstring_view kLinkPath = LR"(C:\Links\codex.exe)";
} // namespace

TEST_CLASS(LinkInspectorTests)
{
public:
    // --- Classification table rows (docs/adr-phase-3.md ADR-0014) ---

    TEST_METHOD(noEntryIsMissingWithNoExistingTarget)
    {
        const LinkObservation observation{LinkEntryKind::None, std::nullopt,
                                          TargetRelation::NotApplicable};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Missing);
        Assert::IsTrue(item.entryKind == LinkEntryKind::None);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(regularFileIsMismatchWithNoExistingTarget)
    {
        const LinkObservation observation{LinkEntryKind::RegularFile, std::nullopt,
                                          TargetRelation::NotApplicable};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::RegularFile);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(nonSymlinkReparsePointIsMismatchWithNoExistingTarget)
    {
        const LinkObservation observation{LinkEntryKind::OtherReparsePoint, std::nullopt,
                                          TargetRelation::NotApplicable};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::OtherReparsePoint);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(symbolicLinkWithAbsentTargetIsBroken)
    {
        const std::filesystem::path decodedTarget = LR"(C:\Packages\Codex\old-name.exe)";
        const LinkObservation observation{LinkEntryKind::SymbolicLink, decodedTarget,
                                          TargetRelation::Missing};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Broken);
        Assert::IsTrue(item.entryKind == LinkEntryKind::SymbolicLink);
        Assert::IsTrue(item.existingTarget.has_value());
        Assert::AreEqual(decodedTarget.native(), item.existingTarget->native());
    }

    TEST_METHOD(symbolicLinkResolvingToExpectedFileIsOk)
    {
        const std::filesystem::path decodedTarget = makeExecutable().path;
        const LinkObservation observation{LinkEntryKind::SymbolicLink, decodedTarget,
                                          TargetRelation::SameFile};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Ok);
        Assert::IsTrue(item.existingTarget.has_value());
    }

    TEST_METHOD(symbolicLinkResolvingToAnotherExistingFileIsMismatch)
    {
        const std::filesystem::path decodedTarget =
            LR"(C:\Packages\OtherPackage\other.exe)";
        const LinkObservation observation{LinkEntryKind::SymbolicLink, decodedTarget,
                                          TargetRelation::DifferentFile};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.existingTarget.has_value());
        Assert::AreEqual(decodedTarget.native(), item.existingTarget->native());
    }

    // --- Passthrough fields ---

    TEST_METHOD(executableAliasAndLinkPathArePassedThroughUnchanged)
    {
        const LinkObservation observation{LinkEntryKind::None, std::nullopt,
                                          TargetRelation::NotApplicable};
        const PackageExe executable = makeExecutable();

        const RepairItem item = classifyLink(observation, executable, std::wstring(kAlias),
                                             std::filesystem::path(kLinkPath));

        Assert::AreEqual(executable.path.native(), item.executable.path.native());
        Assert::AreEqual(std::wstring(kAlias), item.alias);
        Assert::AreEqual(std::filesystem::path(kLinkPath).native(), item.linkPath.native());
    }

    // --- Invalid observation combinations must fail explicitly ---

    TEST_METHOD(noEntryWithADecodedTargetIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::None,
                                          std::filesystem::path(LR"(C:\somewhere.exe)"),
                                          TargetRelation::NotApplicable};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(regularFileWithATargetRelationIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::RegularFile, std::nullopt,
                                          TargetRelation::SameFile};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(otherReparsePointWithADecodedTargetIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::OtherReparsePoint,
                                          std::filesystem::path(LR"(C:\somewhere.exe)"),
                                          TargetRelation::NotApplicable};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(symbolicLinkWithoutADecodedTargetIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::SymbolicLink, std::nullopt,
                                          TargetRelation::Missing};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(symbolicLinkLeftAtNotApplicableIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::SymbolicLink,
                                          std::filesystem::path(LR"(C:\somewhere.exe)"),
                                          TargetRelation::NotApplicable};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    // --- LinkInspectionError ---

    TEST_METHOD(linkInspectionErrorCarriesOperationPathAndWin32ErrorCode)
    {
        const std::filesystem::path path = LR"(C:\Links\codex.exe)";
        const LinkInspectionError error("DeviceIoControl", path, 5u);

        Assert::AreEqual(std::string("DeviceIoControl"), error.operation());
        Assert::AreEqual(path.native(), error.path().native());
        Assert::AreEqual(5u, error.win32ErrorCode());
        Assert::IsTrue(std::string(error.what()).find("DeviceIoControl") !=
                       std::string::npos);
    }
};
} // namespace syncwingetlink::tests
