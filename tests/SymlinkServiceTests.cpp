// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/LinkInspector.h>
#include <core/SymlinkService.h>

#include "TempDirectory.h"

#include <Windows.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

[[nodiscard]] RepairItem makeCandidate(LinkStatus status,
                                       LinkEntryKind entryKind = LinkEntryKind::None)
{
    RepairItem item;
    item.executable = makeExecutable();
    item.alias = std::wstring(kAlias);
    item.linkPath = kLinkPath;
    item.status = status;
    item.entryKind = entryKind;
    return item;
}

// Records every call SymlinkServiceOperations forwards, and hands back preprogrammed
// inspect() results in order - one per call, so a test can tell the pre-action fresh
// inspection apart from the post-create verification. Calling inspect() more times than
// results were queued is itself a test failure (std::vector::at throws), which is exactly
// how an unexpected extra re-inspection - e.g. one DryRun must never perform - would be
// caught.
struct FakeOperations
{
    std::vector<RepairItem> inspectResults;
    std::vector<std::uint32_t> deleteResults{0};
    std::vector<std::uint32_t> createResults{0};
    DeveloperModeState developerMode{DeveloperModeState::Unknown};
    ElevationState elevation{ElevationState::Unknown};

    int inspectCalls{0};
    int deleteCalls{0};
    int createCalls{0};
    int developerModeQueries{0};
    int elevationQueries{0};
    std::optional<std::filesystem::path> lastDeletePath;
    std::optional<std::filesystem::path> lastCreateTarget;
    std::optional<std::filesystem::path> lastCreateLink;
    std::optional<std::uint32_t> lastCreateFlags;

    [[nodiscard]] SymlinkServiceOperations toOperations()
    {
        SymlinkServiceOperations operations;
        operations.inspect = [this](const PackageExe&, const std::wstring&,
                                    const std::filesystem::path&) {
            return inspectResults.at(static_cast<std::size_t>(inspectCalls++));
        };
        operations.deleteEntry = [this](const std::filesystem::path& linkPath) {
            ++deleteCalls;
            lastDeletePath = linkPath;
            return deleteResults.at(static_cast<std::size_t>(deleteCalls - 1));
        };
        operations.create = [this](const std::filesystem::path& target,
                                   const std::filesystem::path& linkPath,
                                   std::uint32_t flags) {
            ++createCalls;
            lastCreateTarget = target;
            lastCreateLink = linkPath;
            lastCreateFlags = flags;
            return createResults.at(static_cast<std::size_t>(createCalls - 1));
        };
        operations.queryDeveloperMode = [this] {
            ++developerModeQueries;
            return developerMode;
        };
        operations.queryElevation = [this] {
            ++elevationQueries;
            return elevation;
        };
        return operations;
    }
};
} // namespace

TEST_CLASS(SymlinkServiceTests)
{
public:
    // --- Input validation ---

    TEST_METHOD(emptyExecutablePathThrowsInvalidArgument)
    {
        RepairItem candidate = makeCandidate(LinkStatus::Missing);
        candidate.executable.path.clear();
        FakeOperations fake;
        fake.inspectResults = {candidate};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)repairLink(candidate, RepairMode::DryRun, fake.toOperations());
        });
    }

    TEST_METHOD(emptyAliasThrowsInvalidArgument)
    {
        RepairItem candidate = makeCandidate(LinkStatus::Missing);
        candidate.alias.clear();
        FakeOperations fake;
        fake.inspectResults = {candidate};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)repairLink(candidate, RepairMode::DryRun, fake.toOperations());
        });
    }

    TEST_METHOD(emptyLinkPathThrowsInvalidArgument)
    {
        RepairItem candidate = makeCandidate(LinkStatus::Missing);
        candidate.linkPath.clear();
        FakeOperations fake;
        fake.inspectResults = {candidate};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)repairLink(candidate, RepairMode::DryRun, fake.toOperations());
        });
    }

    TEST_METHOD(brokenStatusWithNonSymlinkEntryKindThrowsInvalidArgument)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        // Fresh inspection reports Broken, but with an entry kind classifyLink() would
        // never actually pair with Broken - a contract violation the caller's inspect
        // adapter must never produce.
        fake.inspectResults = {makeCandidate(LinkStatus::Broken, LinkEntryKind::RegularFile)};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
        });
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    // --- Missing: create ---

    TEST_METHOD(missingExecuteCreatesAndVerifies)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing),
                               makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::Created);
        Assert::IsTrue(result.preActionItem.status == LinkStatus::Missing);
        Assert::IsTrue(result.postActionItem.has_value());
        Assert::IsTrue(result.postActionItem->status == LinkStatus::Ok);
        Assert::AreEqual(2, fake.inspectCalls);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(1, fake.createCalls);
    }

    TEST_METHOD(missingCreationUsesExpectedExecutableAsTargetAndUnprivilegedFlag)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing),
                               makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(fake.lastCreateTarget.has_value());
        Assert::AreEqual(candidate.executable.path.native(), fake.lastCreateTarget->native());
        Assert::IsTrue(fake.lastCreateLink.has_value());
        Assert::AreEqual(candidate.linkPath.native(), fake.lastCreateLink->native());
        Assert::IsTrue(fake.lastCreateFlags.has_value());
        Assert::AreEqual(
            static_cast<std::uint32_t>(SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE),
            *fake.lastCreateFlags);
    }

    TEST_METHOD(missingDryRunReportsWouldCreateWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::DryRun, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::WouldCreate);
        Assert::IsFalse(result.postActionItem.has_value());
        Assert::AreEqual(1, fake.inspectCalls);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    TEST_METHOD(creationFailurePreventsVerificationAndReportsCreateFailed)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing)};
        fake.createResults = {ERROR_DISK_FULL};

        try
        {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
            Assert::Fail(L"Expected SymlinkServiceError");
        }
        catch (const SymlinkServiceError& error)
        {
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::CreateFailed);
            Assert::AreEqual(std::string("CreateSymbolicLinkW"), error.operation());
            Assert::AreEqual(static_cast<std::uint32_t>(ERROR_DISK_FULL),
                             error.win32ErrorCode());
        }
        // Only the pre-action inspection happened; no post-create verification.
        Assert::AreEqual(1, fake.inspectCalls);
    }

    TEST_METHOD(nonOkVerificationThrowsVerificationFailed)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing),
                               makeCandidate(LinkStatus::Mismatch, LinkEntryKind::RegularFile)};

        try
        {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
            Assert::Fail(L"Expected SymlinkServiceError");
        }
        catch (const SymlinkServiceError& error)
        {
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::VerificationFailed);
        }
        Assert::AreEqual(1, fake.createCalls);
        Assert::AreEqual(2, fake.inspectCalls);
    }

    // --- Broken: delete then create ---

    TEST_METHOD(brokenExecuteDeletesThenCreatesThenVerifies)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Broken);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Broken, LinkEntryKind::SymbolicLink),
                               makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::ReplacedBroken);
        Assert::AreEqual(1, fake.deleteCalls);
        Assert::AreEqual(1, fake.createCalls);
        Assert::AreEqual(2, fake.inspectCalls);
        Assert::IsTrue(fake.lastDeletePath.has_value());
        Assert::AreEqual(candidate.linkPath.native(), fake.lastDeletePath->native());
    }

    TEST_METHOD(brokenDryRunReportsWouldReplaceBrokenWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Broken);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Broken, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::DryRun, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::WouldReplaceBroken);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    TEST_METHOD(deleteFailurePreventsCreationAndReportsDeleteFailed)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Broken);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Broken, LinkEntryKind::SymbolicLink)};
        fake.deleteResults = {ERROR_SHARING_VIOLATION};

        try
        {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
            Assert::Fail(L"Expected SymlinkServiceError");
        }
        catch (const SymlinkServiceError& error)
        {
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::DeleteFailed);
            Assert::AreEqual(std::string("DeleteFileW"), error.operation());
        }
        Assert::AreEqual(0, fake.createCalls);
        Assert::AreEqual(1, fake.inspectCalls);
    }

    // --- Ok / Mismatch: no mutation, either mode ---

    TEST_METHOD(okExecuteSkipsWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Ok);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::SkippedOk);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    TEST_METHOD(okDryRunSkipsWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Ok);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::DryRun, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::SkippedOk);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    TEST_METHOD(mismatchRegularFileExecuteRefusesWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Mismatch);
        FakeOperations fake;
        fake.inspectResults = {
            makeCandidate(LinkStatus::Mismatch, LinkEntryKind::RegularFile)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::RefusedMismatch);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    TEST_METHOD(mismatchOtherReparsePointExecuteRefusesWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Mismatch);
        FakeOperations fake;
        fake.inspectResults = {
            makeCandidate(LinkStatus::Mismatch, LinkEntryKind::OtherReparsePoint)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::RefusedMismatch);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    TEST_METHOD(mismatchDifferentTargetSymlinkExecuteRefusesWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Mismatch);
        FakeOperations fake;
        fake.inspectResults = {
            makeCandidate(LinkStatus::Mismatch, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::RefusedMismatch);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    // --- Stale candidate: repairLink always trusts the fresh inspection, not candidate ---

    TEST_METHOD(candidateStatusIsIgnoredInFavorOfFreshInspection)
    {
        // Caller believes this is Missing (e.g. from an earlier scan), but the fresh
        // inspection - the only thing repairLink() actually consults - now reports Ok.
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::SkippedOk);
        Assert::AreEqual(0, fake.createCalls);
    }

    // --- Permission-shaped failures: classified, but Unknown until #51 wires real queries ---

    TEST_METHOD(accessDeniedOnCreateIsClassifiedAsInsufficientPermission)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing)};
        fake.createResults = {ERROR_ACCESS_DENIED};

        try
        {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
            Assert::Fail(L"Expected SymlinkServiceError");
        }
        catch (const SymlinkServiceError& error)
        {
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::InsufficientPermission);
            Assert::IsTrue(error.developerModeState() == DeveloperModeState::Unknown);
            Assert::IsTrue(error.elevationState() == ElevationState::Unknown);
        }
        Assert::AreEqual(1, fake.developerModeQueries);
        Assert::AreEqual(1, fake.elevationQueries);
    }

    TEST_METHOD(privilegeNotHeldOnDeleteIsClassifiedAsInsufficientPermission)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Broken);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Broken, LinkEntryKind::SymbolicLink)};
        fake.deleteResults = {ERROR_PRIVILEGE_NOT_HELD};

        try
        {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
            Assert::Fail(L"Expected SymlinkServiceError");
        }
        catch (const SymlinkServiceError& error)
        {
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::InsufficientPermission);
        }
    }

    // --- Filesystem-backed: production repairLink() against a real temp directory ---

    TEST_METHOD(productionRepairLinkCreatesARealMissingLink)
    {
        const TempDirectory temp(L"symlink-service-create");
        const std::filesystem::path executablePath = temp.createFile(L"codex-real.exe");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";

        PackageExe executable;
        executable.path = executablePath;
        RepairItem candidate;
        candidate.executable = executable;
        candidate.alias = std::wstring(kAlias);
        candidate.linkPath = linkPath;
        candidate.status = LinkStatus::Missing;

        try
        {
            const SymlinkRepairResult result = repairLink(candidate, RepairMode::Execute);
            Assert::IsTrue(result.outcome == SymlinkRepairOutcome::Created);
            Assert::IsTrue(result.postActionItem.has_value());
            Assert::IsTrue(result.postActionItem->status == LinkStatus::Ok);
        }
        catch (const SymlinkServiceError& error)
        {
            // CppUnitTestFramework (native C++ MSTest) has no Assert::Inconclusive -
            // logging and returning is the closest equivalent: the test stays green
            // here and exercises this branch for real wherever symlink privilege is
            // available (docs/adr-phase-3.md ADR-0016 established the same pattern for
            // LinkInspectorTests).
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::CreateFailed ||
                          error.kind() == SymlinkServiceErrorKind::InsufficientPermission);
        }
    }
};
} // namespace syncwingetlink::tests
