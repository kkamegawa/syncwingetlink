// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/RepairBatch.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] RepairItem makeItem(const std::wstring& alias, LinkStatus status)
{
    RepairItem item;
    item.executable.path = L"C:\\Packages\\" + alias;
    item.alias = alias;
    item.linkPath = L"C:\\Links\\" + alias;
    item.status = status;
    return item;
}

[[nodiscard]] SymlinkRepairResult makeResult(SymlinkRepairOutcome outcome)
{
    SymlinkRepairResult result;
    result.outcome = outcome;
    return result;
}

// A scripted repairFunction: returns the next outcome queued for the candidate's
// alias, or throws the queued SymlinkServiceError instead. Mirrors the
// SymlinkServiceOperations-style fakes SymlinkServiceTests.cpp already uses, applied
// one level up - RepairBatch's own decision/counting logic is what these tests target,
// not repairLink() itself (already covered by SymlinkServiceTests.cpp).
class ScriptedRepairs
{
public:
    void queueOutcome(const std::wstring& alias, SymlinkRepairOutcome outcome)
    {
        m_outcomes[alias] = outcome;
    }

    void queueFailure(const std::wstring& alias, SymlinkServiceError error)
    {
        m_failures.emplace(alias, std::move(error));
    }

    [[nodiscard]] RepairFunction asFunction()
    {
        return [this](const RepairItem& candidate, RepairMode) -> SymlinkRepairResult {
            m_callCount[candidate.alias] += 1;
            const auto failureIt = m_failures.find(candidate.alias);
            if (failureIt != m_failures.end())
            {
                throw failureIt->second;
            }
            const auto outcomeIt = m_outcomes.find(candidate.alias);
            Assert::IsTrue(outcomeIt != m_outcomes.end(),
                           L"ScriptedRepairs: no outcome/failure queued for this alias");
            return makeResult(outcomeIt->second);
        };
    }

    [[nodiscard]] int callCountFor(const std::wstring& alias) const
    {
        const auto it = m_callCount.find(alias);
        return it == m_callCount.end() ? 0 : it->second;
    }

private:
    std::map<std::wstring, SymlinkRepairOutcome> m_outcomes;
    std::map<std::wstring, SymlinkServiceError> m_failures;
    std::map<std::wstring, int> m_callCount;
};

[[nodiscard]] SymlinkServiceError makePermissionError(const std::wstring& alias)
{
    return SymlinkServiceError(SymlinkServiceErrorKind::InsufficientPermission,
                               "CreateSymbolicLinkW", L"C:\\Links\\" + alias, 5,
                               DeveloperModeState::Disabled, ElevationState::NotElevated);
}

[[nodiscard]] SymlinkServiceError makeCreateFailedError(const std::wstring& alias)
{
    return SymlinkServiceError(SymlinkServiceErrorKind::CreateFailed, "CreateSymbolicLinkW",
                               L"C:\\Links\\" + alias, 1);
}
} // namespace

TEST_CLASS(RunRepairBatchFullSuccessTests)
{
public:
    TEST_METHOD(createdAndReplacedAreCountedAndConsentIsAskedForEach)
    {
        ScriptedRepairs repairs;
        repairs.queueOutcome(L"missing.exe", SymlinkRepairOutcome::Created);
        repairs.queueOutcome(L"broken.exe", SymlinkRepairOutcome::ReplacedBroken);

        RepairBatchOptions options;
        options.mode = RepairMode::Execute;
        options.repairFunction = repairs.asFunction();
        int consentCalls = 0;
        options.consent = [&consentCalls](const RepairItem&) {
            ++consentCalls;
            return true;
        };

        const std::vector<RepairItem> candidates = {
            makeItem(L"missing.exe", LinkStatus::Missing),
            makeItem(L"broken.exe", LinkStatus::Broken),
        };

        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{2}, result.summary.selected);
        Assert::AreEqual(size_t{2}, result.summary.processed);
        Assert::AreEqual(size_t{0}, result.summary.remaining);
        Assert::AreEqual(size_t{1}, result.summary.created);
        Assert::AreEqual(size_t{1}, result.summary.replaced);
        Assert::AreEqual(2, consentCalls);
        Assert::IsTrue(exitCodeFor(result.summary) == RepairBatchExitCode::Success);
    }

    TEST_METHOD(okAndMismatchNeverAskForConsent)
    {
        ScriptedRepairs repairs;
        repairs.queueOutcome(L"ok.exe", SymlinkRepairOutcome::SkippedOk);
        repairs.queueOutcome(L"mismatch.exe", SymlinkRepairOutcome::RefusedMismatch);

        RepairBatchOptions options;
        options.repairFunction = repairs.asFunction();
        options.consent = [](const RepairItem&) -> bool {
            Assert::Fail(L"consent() must never be called for Ok/Mismatch candidates");
        };

        const std::vector<RepairItem> candidates = {
            makeItem(L"ok.exe", LinkStatus::Ok),
            makeItem(L"mismatch.exe", LinkStatus::Mismatch),
        };

        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{1}, result.summary.skippedOk);
        Assert::AreEqual(size_t{1}, result.summary.refusedMismatch);
        Assert::IsTrue(exitCodeFor(result.summary) == RepairBatchExitCode::Success);
    }
};

TEST_CLASS(RunRepairBatchDryRunTests)
{
public:
    TEST_METHOD(dryRunProducesPlannedOutcomesWithoutAskingConsent)
    {
        ScriptedRepairs repairs;
        repairs.queueOutcome(L"missing.exe", SymlinkRepairOutcome::WouldCreate);
        repairs.queueOutcome(L"broken.exe", SymlinkRepairOutcome::WouldReplaceBroken);

        RepairBatchOptions options;
        options.mode = RepairMode::DryRun;
        options.repairFunction = repairs.asFunction();
        options.consent = [](const RepairItem&) -> bool {
            Assert::Fail(L"consent() must never be called in DryRun mode");
        };

        const std::vector<RepairItem> candidates = {
            makeItem(L"missing.exe", LinkStatus::Missing),
            makeItem(L"broken.exe", LinkStatus::Broken),
        };

        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{2}, result.summary.planned);
        Assert::AreEqual(size_t{0}, result.summary.created);
        Assert::IsTrue(exitCodeFor(result.summary) == RepairBatchExitCode::Success);
    }
};

TEST_CLASS(RunRepairBatchDeclinedTests)
{
public:
    TEST_METHOD(consentRefusalIsDeclinedNotAttempted)
    {
        ScriptedRepairs repairs; // Nothing queued - repairFunction must not be called.

        RepairBatchOptions options;
        options.repairFunction = repairs.asFunction();
        options.consent = [](const RepairItem&) { return false; };

        const std::vector<RepairItem> candidates = {makeItem(L"missing.exe", LinkStatus::Missing)};
        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{1}, result.summary.declined);
        Assert::AreEqual(size_t{1}, result.summary.processed);
        Assert::AreEqual(0, repairs.callCountFor(L"missing.exe"));
        Assert::IsTrue(result.items[0].disposition == RepairDisposition::Declined);
        Assert::IsFalse(result.items[0].repairResult.has_value());
        Assert::IsTrue(exitCodeFor(result.summary) == RepairBatchExitCode::Success);
    }

    TEST_METHOD(preApprovedAliasesSkipsConsentCallbackEntirely)
    {
        ScriptedRepairs repairs;
        repairs.queueOutcome(L"selected.exe", SymlinkRepairOutcome::Created);

        RepairBatchOptions options;
        options.repairFunction = repairs.asFunction();
        options.preApprovedAliases = std::vector<std::wstring>{L"selected.exe"};
        options.consent = [](const RepairItem&) -> bool {
            Assert::Fail(L"consent() must not be called when preApprovedAliases is set");
        };

        const std::vector<RepairItem> candidates = {
            makeItem(L"selected.exe", LinkStatus::Missing),
            makeItem(L"unselected.exe", LinkStatus::Broken),
        };

        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{1}, result.summary.created);
        Assert::AreEqual(size_t{1}, result.summary.declined);
        Assert::IsTrue(result.items[0].disposition == RepairDisposition::Created);
        Assert::IsTrue(result.items[1].disposition == RepairDisposition::Declined);
    }

    TEST_METHOD(preApprovedAliasesComparisonIsOrdinalCaseInsensitive)
    {
        ScriptedRepairs repairs;
        repairs.queueOutcome(L"Tool.exe", SymlinkRepairOutcome::Created);

        RepairBatchOptions options;
        options.repairFunction = repairs.asFunction();
        options.preApprovedAliases = std::vector<std::wstring>{L"TOOL.EXE"};

        const std::vector<RepairItem> candidates = {makeItem(L"Tool.exe", LinkStatus::Missing)};
        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::IsTrue(result.items[0].disposition == RepairDisposition::Created);
    }

    TEST_METHOD(assumeYesBypassesConsentEntirely)
    {
        ScriptedRepairs repairs;
        repairs.queueOutcome(L"missing.exe", SymlinkRepairOutcome::Created);

        RepairBatchOptions options;
        options.assumeYes = true;
        options.repairFunction = repairs.asFunction();
        options.consent = [](const RepairItem&) -> bool {
            Assert::Fail(L"consent() must not be called when assumeYes is true");
        };

        const std::vector<RepairItem> candidates = {makeItem(L"missing.exe", LinkStatus::Missing)};
        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::IsTrue(result.items[0].disposition == RepairDisposition::Created);
    }
};

TEST_CLASS(RunRepairBatchFailureTests)
{
public:
    TEST_METHOD(insufficientPermissionIsCountedAsFailedAndFlagged)
    {
        ScriptedRepairs repairs;
        repairs.queueFailure(L"missing.exe", makePermissionError(L"missing.exe"));

        RepairBatchOptions options;
        options.assumeYes = true;
        options.repairFunction = repairs.asFunction();

        const std::vector<RepairItem> candidates = {makeItem(L"missing.exe", LinkStatus::Missing)};
        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{1}, result.summary.failed);
        Assert::IsTrue(result.summary.anyInsufficientPermission);
        Assert::IsTrue(result.items[0].disposition == RepairDisposition::Failed);
        Assert::IsTrue(result.items[0].error.has_value());
        Assert::IsTrue(result.items[0].error->kind() ==
                       SymlinkServiceErrorKind::InsufficientPermission);
        Assert::IsTrue(exitCodeFor(result.summary) ==
                       RepairBatchExitCode::InsufficientPermission);
    }

    TEST_METHOD(otherFailureIsCountedAsFailedWithoutThePermissionFlag)
    {
        ScriptedRepairs repairs;
        repairs.queueFailure(L"missing.exe", makeCreateFailedError(L"missing.exe"));

        RepairBatchOptions options;
        options.assumeYes = true;
        options.repairFunction = repairs.asFunction();

        const std::vector<RepairItem> candidates = {makeItem(L"missing.exe", LinkStatus::Missing)};
        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{1}, result.summary.failed);
        Assert::IsFalse(result.summary.anyInsufficientPermission);
        Assert::IsTrue(exitCodeFor(result.summary) == RepairBatchExitCode::PartialFailure);
    }

    TEST_METHOD(permissionFailureTakesPrecedenceOverInterruption)
    {
        // D2/ADR-0028: this is the corrected precedence - the pre-#60 CLI checked
        // interruption first, so a permission failure followed by Ctrl+C used to
        // return 10, not 2.
        ScriptedRepairs repairs;
        repairs.queueFailure(L"first.exe", makePermissionError(L"first.exe"));

        RepairBatchOptions options;
        options.assumeYes = true;
        bool interruptAfterFirst = false;
        options.pollInterrupted = [&interruptAfterFirst]() { return interruptAfterFirst; };

        // The scripted repair itself throws for "first.exe" - this wrapper only adds
        // "flip the interruption flag once we're past that call, success or not", so
        // the batch is interrupted before it would otherwise reach "second.exe".
        const RepairFunction inner = repairs.asFunction();
        options.repairFunction = [&](const RepairItem& candidate, RepairMode mode) {
            struct FlagOnExit
            {
                bool& flag;
                ~FlagOnExit()
                {
                    flag = true;
                }
            } flagOnExit{interruptAfterFirst};
            return inner(candidate, mode);
        };

        const std::vector<RepairItem> candidates = {
            makeItem(L"first.exe", LinkStatus::Missing),
            makeItem(L"second.exe", LinkStatus::Missing),
        };

        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{1}, result.summary.processed);
        Assert::AreEqual(size_t{1}, result.summary.remaining);
        Assert::IsTrue(result.summary.interrupted);
        Assert::IsTrue(result.summary.anyInsufficientPermission);
        Assert::IsTrue(exitCodeFor(result.summary) ==
                       RepairBatchExitCode::InsufficientPermission);
    }
};

TEST_CLASS(RunRepairBatchInterruptionTests)
{
public:
    TEST_METHOD(interruptionBeforeAnyItemLeavesEverythingRemaining)
    {
        ScriptedRepairs repairs;
        RepairBatchOptions options;
        options.repairFunction = repairs.asFunction();
        options.pollInterrupted = []() { return true; };

        const std::vector<RepairItem> candidates = {
            makeItem(L"a.exe", LinkStatus::Missing),
            makeItem(L"b.exe", LinkStatus::Missing),
        };

        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{0}, result.summary.processed);
        Assert::AreEqual(size_t{2}, result.summary.remaining);
        Assert::IsTrue(result.summary.interrupted);
        Assert::IsTrue(result.items.empty());
        Assert::IsTrue(exitCodeFor(result.summary) == RepairBatchExitCode::PartialFailure);
    }

    TEST_METHOD(interruptionMidwayStopsBeforeTheNextItem)
    {
        ScriptedRepairs repairs;
        repairs.queueOutcome(L"a.exe", SymlinkRepairOutcome::Created);
        repairs.queueOutcome(L"b.exe", SymlinkRepairOutcome::Created);
        repairs.queueOutcome(L"c.exe", SymlinkRepairOutcome::Created);

        RepairBatchOptions options;
        options.assumeYes = true;
        options.repairFunction = repairs.asFunction();
        int processedSoFar = 0;
        options.onProgress = [&processedSoFar](std::size_t, std::size_t, const RepairItemResult&) {
            ++processedSoFar;
        };
        options.pollInterrupted = [&processedSoFar]() { return processedSoFar >= 2; };

        const std::vector<RepairItem> candidates = {
            makeItem(L"a.exe", LinkStatus::Missing),
            makeItem(L"b.exe", LinkStatus::Missing),
            makeItem(L"c.exe", LinkStatus::Missing),
        };

        const RepairBatchResult result = runRepairBatch(candidates, options);

        Assert::AreEqual(size_t{2}, result.summary.processed);
        Assert::AreEqual(size_t{1}, result.summary.remaining);
        Assert::IsTrue(result.summary.interrupted);
        Assert::AreEqual(0, repairs.callCountFor(L"c.exe"));
        Assert::IsTrue(exitCodeFor(result.summary) == RepairBatchExitCode::PartialFailure);
    }
};

// exitCodeFor() is tested directly against hand-built summaries too, not only through
// runRepairBatch() - in particular, the "interrupted with nothing remaining" case
// documented on exitCodeFor() cannot actually arise from runRepairBatch()'s own loop
// (interrupted is only ever set when at least one candidate is left unprocessed), so
// this exercises that defensive branch of the contract directly.
TEST_CLASS(ExitCodeForTests)
{
public:
    TEST_METHOD(successForAnEmptyOrFullyQuietSummary)
    {
        RepairBatchSummary summary;
        Assert::IsTrue(exitCodeFor(summary) == RepairBatchExitCode::Success);
    }

    TEST_METHOD(interruptedWithNoRemainingItemsIsStillSuccess)
    {
        RepairBatchSummary summary;
        summary.interrupted = true;
        summary.remaining = 0;
        Assert::IsTrue(exitCodeFor(summary) == RepairBatchExitCode::Success);
    }

    TEST_METHOD(interruptedWithRemainingItemsIsPartialFailure)
    {
        RepairBatchSummary summary;
        summary.interrupted = true;
        summary.remaining = 1;
        Assert::IsTrue(exitCodeFor(summary) == RepairBatchExitCode::PartialFailure);
    }

    TEST_METHOD(failedWithoutInterruptionIsStillPartialFailure)
    {
        RepairBatchSummary summary;
        summary.failed = 1;
        Assert::IsTrue(exitCodeFor(summary) == RepairBatchExitCode::PartialFailure);
    }

    TEST_METHOD(permissionFlagWinsOverEverythingElse)
    {
        RepairBatchSummary summary;
        summary.anyInsufficientPermission = true;
        summary.interrupted = true;
        summary.remaining = 5;
        summary.failed = 5;
        Assert::IsTrue(exitCodeFor(summary) == RepairBatchExitCode::InsufficientPermission);
    }
};
} // namespace syncwingetlink::tests
