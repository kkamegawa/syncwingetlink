// SPDX-License-Identifier: MIT

#pragma once

#include "Model.h"
#include "SymlinkService.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace syncwingetlink
{
// One candidate's final disposition within a repair batch - the vocabulary the CLI and
// TUI paths report progress and the final summary in (docs/adr-phase-6.md ADR-0028).
// Distinct from SymlinkRepairOutcome (SymlinkService.h), which only exists for a
// candidate that actually reached repairLink() - Declined and NotAttempted never do,
// and Failed can result from a thrown SymlinkServiceError that never produced one
// either.
enum class RepairDisposition
{
    Created,
    ReplacedBroken,
    PlannedCreate,        // --dry-run: would create
    PlannedReplaceBroken, // --dry-run: would replace
    // Offered (an interactive "y/N" prompt, or a checklist item left unchecked) and
    // refused. Distinct from Planned*: unlike --dry-run, no pre-action inspection is
    // performed for a declined item at all - the caller's consent decision, not an
    // inspection, is what decided its fate. A declined item never reaches
    // repairLink().
    Declined,
    SkippedOk,        // repairLink() found it already Ok on fresh inspection
    RefusedMismatch,  // repairLink() found it Mismatch on fresh inspection
    Failed,           // repairLink() threw
    NotAttempted,     // never reached - the batch was interrupted before this item
};

struct RepairItemResult
{
    // The pre-batch candidate, for alias/status display - never the source of the
    // disposition itself, which always comes from a fresh repairLink() call or an
    // explicit decline/interruption decision, per the same re-inspection discipline
    // SymlinkService.h already documents for repairLink() itself.
    RepairItem candidate;
    RepairDisposition disposition{RepairDisposition::NotAttempted};
    // Set only when disposition is Created/ReplacedBroken/PlannedCreate/
    // PlannedReplaceBroken/SkippedOk/RefusedMismatch - the SymlinkRepairResult
    // repairLink() actually returned, so JSON serialization (cli::toJsonFixResult,
    // docs/adr-phase-5.md ADR-0022) keeps consuming exactly what it already did.
    std::optional<SymlinkRepairResult> repairResult;
    // Set only when disposition is Failed - the caught SymlinkServiceError, preserved
    // so a caller can still produce the same permission-guidance/error text the
    // pre-#60 CLI path already produced per item. This executor does not format that
    // text itself; it has no Console to write to.
    std::optional<SymlinkServiceError> error;
};

struct RepairBatchSummary
{
    std::size_t selected{0};  // candidates offered to the batch
    std::size_t processed{0}; // candidates actually reached before any interruption
    std::size_t remaining{0}; // selected - processed
    std::size_t created{0};
    std::size_t replaced{0};
    std::size_t planned{0}; // PlannedCreate + PlannedReplaceBroken
    std::size_t declined{0};
    std::size_t skippedOk{0};
    std::size_t refusedMismatch{0};
    std::size_t failed{0};
    bool anyInsufficientPermission{false};
    bool interrupted{false};
};

struct RepairBatchResult
{
    std::vector<RepairItemResult> items; // one per processed candidate, in order;
                                         // never includes NotAttempted entries for
                                         // candidates the batch stopped before reaching
    RepairBatchSummary summary;
};

// Asks whether to proceed (true) or decline (false) with one Missing/Broken candidate
// that needs consent. Never called for Ok/Mismatch (they never needed consent),
// when RepairBatchOptions::assumeYes is true, when preApprovedAliases has a value, or
// in DryRun mode (the whole batch reports Planned outcomes without asking anything).
using RepairConsentCallback = std::function<bool(const RepairItem& candidate)>;

// Polled between (never during) items - returning true stops the batch before the next
// item is attempted. Mirrors cli::Dispatch's existing Ctrl+C handling
// (SetConsoleCtrlHandler-backed atomic flag), injected here instead of hardwired so
// this executor stays testable without a real console-control handler and the TUI path
// (whose checklist already owns its own, unrelated cancellation - see
// docs/adr-phase-6.md ADR-0026/0027) is not forced to reuse this specific mechanism.
using InterruptionPoll = std::function<bool()>;

// Called once per candidate immediately after its disposition is decided, with the
// 1-based current position and the total selected - the `[current/total] alias:
// result` progress-line contract. Never called for a candidate the batch stops before
// reaching.
using RepairProgressCallback =
    std::function<void(std::size_t current, std::size_t total, const RepairItemResult& item)>;

// Performs one candidate's repair (or dry-run report). Defaults to the real,
// single-implementation repairLink(candidate, mode) (SymlinkService.h) when
// RepairBatchOptions::repairFunction is left unset; tests supply a deterministic
// callback instead - the same seam-injection pattern SymlinkServiceOperations,
// ConsoleOperations, and TerminalOperations already establish in this codebase, so
// this executor's own decision/counting logic is testable without symlink privilege,
// Developer Mode, or elevation.
using RepairFunction =
    std::function<SymlinkRepairResult(const RepairItem& candidate, RepairMode mode)>;

struct RepairBatchOptions
{
    RepairMode mode{RepairMode::Execute};

    // Skips the interactive consent() callback for every Missing/Broken candidate:
    // approved when true outright (matching --yes), otherwise unconditionally
    // interactive via preApprovedAliases/consent below. Mirrors AppOptions::assumeYes.
    bool assumeYes{false};

    // When set, a candidate's alias is looked up here (ordinal, case-insensitive - the
    // same comparison every other alias lookup in this codebase uses) instead of
    // calling consent() - the TUI path's checklist selection already decided consent
    // for every offered candidate before the batch ever starts.
    std::optional<std::vector<std::wstring>> preApprovedAliases;

    // Used only when assumeYes is false and preApprovedAliases has no value - the
    // non-interactive CLI path's per-item "Repair X (currently Y)? [y/N]" prompt.
    RepairConsentCallback consent;

    InterruptionPoll pollInterrupted;
    RepairProgressCallback onProgress; // optional

    // Defaults to the real repairLink(candidate, mode) when left empty - production
    // callers never need to set this.
    RepairFunction repairFunction;
};

// Runs candidates through the same repairLink() every path already uses, deciding each
// item's RepairDisposition and building one aggregate summary - so the CLI's
// non-interactive fix path and the TUI's post-checklist repair phase share a single
// implementation of both the result accounting and (via exitCodeFor() below) the
// exit-code decision, and cannot drift apart on either (docs/adr-phase-6.md ADR-0028).
[[nodiscard]] RepairBatchResult runRepairBatch(const std::vector<RepairItem>& candidates,
                                               const RepairBatchOptions& options);

// The batch-level exit-code decision, corrected here from the pre-#60 CLI's own
// precedence: `src/cli/Dispatch.cpp`'s original runFix() checked an interruption
// *before* insufficient permission, so a permission failure followed by Ctrl+C
// returned 10, not 2 - contradicting the documented "insufficient permission returns
// 2" priority. This function is the one place that decision is made from now on:
//
//   1. Insufficient permission -> InsufficientPermission, regardless of any
//      interruption or other failure in the same batch.
//   2. Otherwise, an interruption with items still remaining
//      (summary.interrupted && summary.remaining > 0), or any other failed item
//      (summary.failed > 0) -> PartialFailure. An interruption observed only after
//      the last candidate had already been processed (remaining == 0) does not count
//      here by itself - there was nothing left it could have prevented.
//   3. Otherwise -> Success, covering full success, every candidate declined/skipped,
//      an empty batch, and a complete (uninterrupted) dry run.
enum class RepairBatchExitCode
{
    Success,
    InsufficientPermission,
    PartialFailure,
};
[[nodiscard]] RepairBatchExitCode exitCodeFor(const RepairBatchSummary& summary) noexcept;
} // namespace syncwingetlink
