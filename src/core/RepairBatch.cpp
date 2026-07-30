// SPDX-License-Identifier: MIT

#include "RepairBatch.h"

// CompareStringOrdinal lives in <winnls.h>, pulled in transitively via <Windows.h> -
// the same boundary core/LinkInspector.cpp and core/PackageFilter.cpp already cross
// for the same ordinal, non-locale-sensitive alias/path comparisons this file needs.
#include <Windows.h>

#include <string_view>
#include <utility>

namespace syncwingetlink
{
namespace
{
[[nodiscard]] bool equalsOrdinalIgnoreCase(std::wstring_view left,
                                          std::wstring_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool containsAliasOrdinalIgnoreCase(const std::vector<std::wstring>& aliases,
                                                   std::wstring_view alias) noexcept
{
    for (const std::wstring& candidate : aliases)
    {
        if (equalsOrdinalIgnoreCase(candidate, alias))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool needsConsent(LinkStatus status) noexcept
{
    return status == LinkStatus::Missing || status == LinkStatus::Broken;
}

[[nodiscard]] RepairDisposition dispositionFor(SymlinkRepairOutcome outcome) noexcept
{
    switch (outcome)
    {
    case SymlinkRepairOutcome::WouldCreate:
        return RepairDisposition::PlannedCreate;
    case SymlinkRepairOutcome::WouldReplaceBroken:
        return RepairDisposition::PlannedReplaceBroken;
    case SymlinkRepairOutcome::Created:
        return RepairDisposition::Created;
    case SymlinkRepairOutcome::ReplacedBroken:
        return RepairDisposition::ReplacedBroken;
    case SymlinkRepairOutcome::SkippedOk:
        return RepairDisposition::SkippedOk;
    case SymlinkRepairOutcome::RefusedMismatch:
        return RepairDisposition::RefusedMismatch;
    }
    return RepairDisposition::Failed; // Unreachable in practice; a defensive default
                                      // rather than undefined behavior for a future
                                      // SymlinkRepairOutcome value this switch has not
                                      // been updated for yet.
}

void accumulate(RepairBatchSummary& summary, const RepairItemResult& item) noexcept
{
    switch (item.disposition)
    {
    case RepairDisposition::Created:
        ++summary.created;
        break;
    case RepairDisposition::ReplacedBroken:
        ++summary.replaced;
        break;
    case RepairDisposition::PlannedCreate:
    case RepairDisposition::PlannedReplaceBroken:
        ++summary.planned;
        break;
    case RepairDisposition::Declined:
        ++summary.declined;
        break;
    case RepairDisposition::SkippedOk:
        ++summary.skippedOk;
        break;
    case RepairDisposition::RefusedMismatch:
        ++summary.refusedMismatch;
        break;
    case RepairDisposition::Failed:
        ++summary.failed;
        if (item.error.has_value() &&
            item.error->kind() == SymlinkServiceErrorKind::InsufficientPermission)
        {
            summary.anyInsufficientPermission = true;
        }
        break;
    case RepairDisposition::NotAttempted:
        break; // Never reaches here - see the loop below.
    }
}
} // namespace

RepairBatchResult runRepairBatch(const std::vector<RepairItem>& candidates,
                                 const RepairBatchOptions& options)
{
    RepairBatchResult result;
    result.summary.selected = candidates.size();
    result.items.reserve(candidates.size());

    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        if (options.pollInterrupted && options.pollInterrupted())
        {
            result.summary.interrupted = true;
            break; // Every candidate from here on stays NotAttempted and is not added
                   // to result.items - remaining is derived below, not tracked per item.
        }

        const RepairItem& candidate = candidates[index];
        RepairItemResult itemResult;
        itemResult.candidate = candidate;

        if (needsConsent(candidate.status) && options.mode == RepairMode::Execute &&
            !options.assumeYes)
        {
            bool approved = true;
            if (options.preApprovedAliases.has_value())
            {
                approved =
                    containsAliasOrdinalIgnoreCase(*options.preApprovedAliases, candidate.alias);
            }
            else if (options.consent)
            {
                approved = options.consent(candidate);
            }

            if (!approved)
            {
                itemResult.disposition = RepairDisposition::Declined;
                accumulate(result.summary, itemResult);
                ++result.summary.processed;
                if (options.onProgress)
                {
                    options.onProgress(index + 1, candidates.size(), itemResult);
                }
                result.items.push_back(std::move(itemResult));
                continue;
            }
        }

        try
        {
            SymlinkRepairResult repairResult = options.repairFunction
                                                    ? options.repairFunction(candidate, options.mode)
                                                    : repairLink(candidate, options.mode);
            itemResult.disposition = dispositionFor(repairResult.outcome);
            itemResult.repairResult = std::move(repairResult);
        }
        catch (const SymlinkServiceError& error)
        {
            itemResult.disposition = RepairDisposition::Failed;
            itemResult.error = error;
        }

        accumulate(result.summary, itemResult);
        ++result.summary.processed;
        if (options.onProgress)
        {
            options.onProgress(index + 1, candidates.size(), itemResult);
        }
        result.items.push_back(std::move(itemResult));
    }

    result.summary.remaining = result.summary.selected - result.summary.processed;
    return result;
}

RepairBatchExitCode exitCodeFor(const RepairBatchSummary& summary) noexcept
{
    if (summary.anyInsufficientPermission)
    {
        return RepairBatchExitCode::InsufficientPermission;
    }
    if ((summary.interrupted && summary.remaining > 0) || summary.failed > 0)
    {
        return RepairBatchExitCode::PartialFailure;
    }
    return RepairBatchExitCode::Success;
}
} // namespace syncwingetlink
