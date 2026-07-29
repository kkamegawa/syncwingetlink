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

    TEST_METHOD(mismatchDryRunRefusesWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Mismatch);
        FakeOperations fake;
        fake.inspectResults = {
            makeCandidate(LinkStatus::Mismatch, LinkEntryKind::RegularFile)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::DryRun, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::RefusedMismatch);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    // Completes the DryRun proof the individual per-state tests above already started:
    // one pass over all four fresh states confirming DryRun invokes none of
    // deleteEntry/create/queryDeveloperMode/queryElevation - not just delete/create,
    // which the per-state tests already checked, but the permission queries too, since
    // DryRun never throws SymlinkServiceError and so never reaches buildError() at all.
    TEST_METHOD(dryRunNeverInvokesAnyMutationOrPermissionQueryCallbackForAnyState)
    {
        struct Case
        {
            LinkStatus status;
            LinkEntryKind entryKind;
            SymlinkRepairOutcome expectedOutcome;
        };
        const Case cases[] = {
            {LinkStatus::Missing, LinkEntryKind::None, SymlinkRepairOutcome::WouldCreate},
            {LinkStatus::Broken, LinkEntryKind::SymbolicLink,
             SymlinkRepairOutcome::WouldReplaceBroken},
            {LinkStatus::Ok, LinkEntryKind::SymbolicLink, SymlinkRepairOutcome::SkippedOk},
            {LinkStatus::Mismatch, LinkEntryKind::RegularFile,
             SymlinkRepairOutcome::RefusedMismatch},
        };

        for (const Case& testCase : cases)
        {
            const RepairItem candidate = makeCandidate(testCase.status, testCase.entryKind);
            FakeOperations fake;
            fake.inspectResults = {makeCandidate(testCase.status, testCase.entryKind)};

            const SymlinkRepairResult result =
                repairLink(candidate, RepairMode::DryRun, fake.toOperations());

            Assert::IsTrue(result.outcome == testCase.expectedOutcome);
            Assert::IsFalse(result.postActionItem.has_value());
            Assert::AreEqual(1, fake.inspectCalls);
            Assert::AreEqual(0, fake.deleteCalls);
            Assert::AreEqual(0, fake.createCalls);
            Assert::AreEqual(0, fake.developerModeQueries);
            Assert::AreEqual(0, fake.elevationQueries);
        }
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

    // A candidate discovered as Broken by an earlier scan is exactly the case
    // docs/adr-phase-4.md ADR-0018's re-inspection rule exists for: another process (or a
    // prior repairLink() call in the same batch) may have already changed linkPath by the
    // time fix actually runs. These three complete the coverage
    // candidateStatusIsIgnoredInFavorOfFreshInspection started, one for each fresh state a
    // stale Broken candidate could now resolve to.

    TEST_METHOD(staleBrokenCandidateNowOkIsSkippedWithoutMutation)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Broken, LinkEntryKind::SymbolicLink);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::SkippedOk);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    TEST_METHOD(staleBrokenCandidateNowMissingCreatesRatherThanDeletes)
    {
        // The entry that used to be a broken symbolic link is gone entirely (e.g. another
        // process cleaned it up) - this is Missing's create path, not Broken's
        // delete-then-create, and must never call deleteEntry for a linkPath that no
        // longer has anything to delete.
        const RepairItem candidate = makeCandidate(LinkStatus::Broken, LinkEntryKind::SymbolicLink);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing),
                               makeCandidate(LinkStatus::Ok, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::Created);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(1, fake.createCalls);
    }

    TEST_METHOD(staleBrokenCandidateNowMismatchIsRefusedWithoutMutation)
    {
        // The entry now resolves to a different existing file - Mismatch is never
        // deleted, even though the candidate the caller supplied still says Broken.
        const RepairItem candidate = makeCandidate(LinkStatus::Broken, LinkEntryKind::SymbolicLink);
        FakeOperations fake;
        fake.inspectResults = {
            makeCandidate(LinkStatus::Mismatch, LinkEntryKind::SymbolicLink)};

        const SymlinkRepairResult result =
            repairLink(candidate, RepairMode::Execute, fake.toOperations());

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::RefusedMismatch);
        Assert::AreEqual(0, fake.deleteCalls);
        Assert::AreEqual(0, fake.createCalls);
    }

    // --- Permission-shaped failures: classified and carry whatever
    //     queryDeveloperMode()/queryElevation() report at the moment of failure ---

    TEST_METHOD(accessDeniedOnCreateIsClassifiedAsInsufficientPermission)
    {
        const RepairItem candidate = makeCandidate(LinkStatus::Missing);
        FakeOperations fake;
        fake.inspectResults = {makeCandidate(LinkStatus::Missing)};
        fake.createResults = {ERROR_ACCESS_DENIED};
        fake.developerMode = DeveloperModeState::Disabled;
        fake.elevation = ElevationState::NotElevated;

        try
        {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
            Assert::Fail(L"Expected SymlinkServiceError");
        }
        catch (const SymlinkServiceError& error)
        {
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::InsufficientPermission);
            Assert::IsTrue(error.developerModeState() == DeveloperModeState::Disabled);
            Assert::IsTrue(error.elevationState() == ElevationState::NotElevated);
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
        fake.developerMode = DeveloperModeState::Enabled;
        fake.elevation = ElevationState::Elevated;

        try
        {
            (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
            Assert::Fail(L"Expected SymlinkServiceError");
        }
        catch (const SymlinkServiceError& error)
        {
            Assert::IsTrue(error.kind() == SymlinkServiceErrorKind::InsufficientPermission);
            // Enabled + Elevated is an unusual but real combination (e.g. an elevated
            // token still lacking symlink privilege via local policy) - the states are
            // reported as queried, never coerced to a "shouldn't happen" default.
            Assert::IsTrue(error.developerModeState() == DeveloperModeState::Enabled);
            Assert::IsTrue(error.elevationState() == ElevationState::Elevated);
        }
    }

    // The full 3x3 Developer Mode x elevation matrix, proving every combination is
    // carried through the InsufficientPermission classification unchanged - not just the
    // two combinations the tests above happen to exercise.
    TEST_METHOD(insufficientPermissionCarriesEveryDeveloperModeElevationCombination)
    {
        constexpr DeveloperModeState kDeveloperModeStates[] = {
            DeveloperModeState::Enabled, DeveloperModeState::Disabled,
            DeveloperModeState::Unknown};
        constexpr ElevationState kElevationStates[] = {
            ElevationState::Elevated, ElevationState::NotElevated, ElevationState::Unknown};

        for (const DeveloperModeState developerMode : kDeveloperModeStates)
        {
            for (const ElevationState elevation : kElevationStates)
            {
                const RepairItem candidate = makeCandidate(LinkStatus::Missing);
                FakeOperations fake;
                fake.inspectResults = {makeCandidate(LinkStatus::Missing)};
                fake.createResults = {ERROR_ACCESS_DENIED};
                fake.developerMode = developerMode;
                fake.elevation = elevation;

                try
                {
                    (void)repairLink(candidate, RepairMode::Execute, fake.toOperations());
                    Assert::Fail(L"Expected SymlinkServiceError");
                }
                catch (const SymlinkServiceError& error)
                {
                    Assert::IsTrue(error.kind() ==
                                   SymlinkServiceErrorKind::InsufficientPermission);
                    Assert::IsTrue(error.developerModeState() == developerMode);
                    Assert::IsTrue(error.elevationState() == elevation);
                }
            }
        }
    }

    // Errors unrelated to permission must never trigger a Developer Mode/elevation query,
    // and must keep their original kind rather than being upgraded.
    TEST_METHOD(unrelatedCreateFailureIsNotClassifiedAsInsufficientPermission)
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
        }
        Assert::AreEqual(0, fake.developerModeQueries);
        Assert::AreEqual(0, fake.elevationQueries);
    }

    // A successful verification failure is likewise never permission-related.
    TEST_METHOD(verificationFailureDoesNotQueryPermissionState)
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
        Assert::AreEqual(0, fake.developerModeQueries);
        Assert::AreEqual(0, fake.elevationQueries);
    }

    // --- Filesystem-backed: the real registry/process-token queries, exercised via a
    //     genuine permission failure on a host confirmed to lack both Developer Mode and
    //     elevation (docs/adr-phase-3.md ADR-0016) ---

    TEST_METHOD(productionPermissionQueriesReflectRealHostStateOnAGenuineFailure)
    {
        const TempDirectory temp(L"symlink-service-permission-query");
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
            // Symlink privilege turned out to be available on this host after all - the
            // permission-query path was never exercised, which is not itself a failure.
            Assert::IsTrue(result.outcome == SymlinkRepairOutcome::Created);
            Logger::WriteMessage(
                L"Symlink creation succeeded on this host; the Developer Mode/elevation "
                L"queries were not exercised by this test.\n");
        }
        catch (const SymlinkServiceError& error)
        {
            if (error.kind() != SymlinkServiceErrorKind::InsufficientPermission)
            {
                Logger::WriteMessage(
                    L"Skipped: creation failed for a reason other than insufficient "
                    L"permission, so the Developer Mode/elevation queries were not "
                    L"exercised.\n");
                return;
            }
            // The real process-token query was actually invoked and, unlike the registry
            // read below, has no legitimate reason to fail for the current process -
            // confirm it produced a real answer instead of defaulting to Unknown.
            Assert::IsTrue(error.elevationState() != ElevationState::Unknown);
            // Developer Mode's registry key may never have been created on a host that
            // has never touched the setting, which is itself a legitimate Unknown per
            // queryDeveloperModeFromRegistry()'s own contract - only log it, don't assert.
            Logger::WriteMessage(
                error.developerModeState() == DeveloperModeState::Unknown
                    ? L"Developer Mode registry value is absent/unreadable on this host "
                      L"(reported Unknown, which is correct).\n"
                    : L"Developer Mode registry value was read successfully on this host.\n");
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

    TEST_METHOD(productionRepairLinkReplacesARealBrokenLink)
    {
        const TempDirectory temp(L"symlink-service-replace-broken");
        const std::filesystem::path executablePath = temp.createFile(L"codex-real.exe");
        const std::filesystem::path staleTarget = temp.path() / L"codex-old.exe";
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";

        if (!createFileSymlink(staleTarget, linkPath))
        {
            // Same fallback as productionRepairLinkCreatesARealMissingLink above: this
            // host has neither Developer Mode nor elevation, so even building the broken
            // fixture itself is unavailable here.
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }

        PackageExe executable;
        executable.path = executablePath;
        RepairItem candidate;
        candidate.executable = executable;
        candidate.alias = std::wstring(kAlias);
        candidate.linkPath = linkPath;
        // Deliberately stale, per the re-inspection rule: repairLink() must arrive at
        // Broken from its own fresh inspectLink() call, not from this field.
        candidate.status = LinkStatus::Ok;

        const SymlinkRepairResult result = repairLink(candidate, RepairMode::Execute);

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::ReplacedBroken);
        Assert::IsTrue(result.preActionItem.status == LinkStatus::Broken);
        Assert::IsTrue(result.postActionItem.has_value());
        Assert::IsTrue(result.postActionItem->status == LinkStatus::Ok);

        const RepairItem reInspected = inspectLink(executable, std::wstring(kAlias), linkPath);
        Assert::IsTrue(reInspected.status == LinkStatus::Ok);
        Assert::IsTrue(reInspected.existingTarget.has_value());
        Assert::AreEqual(executablePath.native(), reInspected.existingTarget->native());
    }

    TEST_METHOD(productionDryRunLeavesARealMissingLinkUntouched)
    {
        const TempDirectory temp(L"symlink-service-dryrun-missing");
        const std::filesystem::path executablePath = temp.createFile(L"codex-real.exe");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";

        PackageExe executable;
        executable.path = executablePath;
        RepairItem candidate;
        candidate.executable = executable;
        candidate.alias = std::wstring(kAlias);
        candidate.linkPath = linkPath;
        candidate.status = LinkStatus::Missing;

        const SymlinkRepairResult result = repairLink(candidate, RepairMode::DryRun);

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::WouldCreate);
        Assert::IsFalse(result.postActionItem.has_value());
        // No symlink privilege is needed to prove this - DryRun never calls
        // CreateSymbolicLinkW at all, on any host.
        Assert::IsFalse(std::filesystem::exists(linkPath));
    }

    TEST_METHOD(productionDryRunLeavesARealBrokenLinkUntouched)
    {
        const TempDirectory temp(L"symlink-service-dryrun-broken");
        const std::filesystem::path executablePath = temp.createFile(L"codex-real.exe");
        const std::filesystem::path staleTarget = temp.path() / L"codex-old.exe";
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";

        if (!createFileSymlink(staleTarget, linkPath))
        {
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }

        PackageExe executable;
        executable.path = executablePath;
        RepairItem candidate;
        candidate.executable = executable;
        candidate.alias = std::wstring(kAlias);
        candidate.linkPath = linkPath;
        candidate.status = LinkStatus::Broken;

        const SymlinkRepairResult result = repairLink(candidate, RepairMode::DryRun);

        Assert::IsTrue(result.outcome == SymlinkRepairOutcome::WouldReplaceBroken);
        // The link must still exist, still be broken, and still point at the original
        // stale target - DryRun never called deleteEntry or create.
        const RepairItem reInspected = inspectLink(executable, std::wstring(kAlias), linkPath);
        Assert::IsTrue(reInspected.status == LinkStatus::Broken);
        Assert::IsTrue(reInspected.existingTarget.has_value());
        Assert::AreEqual(staleTarget.native(), reInspected.existingTarget->native());
    }
};
} // namespace syncwingetlink::tests
