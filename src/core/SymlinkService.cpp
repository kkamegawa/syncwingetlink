// SPDX-License-Identifier: MIT

#include "SymlinkService.h"

#include "LinkInspector.h"
#include "Paths.h"

#include <Windows.h>

#include <format>
#include <utility>

namespace syncwingetlink
{
namespace
{
// path.string() transcodes through the process's narrow (ACP) locale, which can mangle
// or throw on a non-ASCII path. path.u8string() transcodes to UTF-8 independently of any
// locale; the reinterpret_cast from char8_t to char is the standard, compiler-supported
// interop idiom for consuming a u8string as a narrow std::string (P1423). Mirrors
// LinkInspector.cpp's own toUtf8().
[[nodiscard]] std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

[[noreturn]] void rejectCandidate(const char* reason)
{
    throw std::invalid_argument(std::string("Invalid RepairItem candidate: ") + reason);
}

void validateCandidate(const RepairItem& candidate)
{
    if (candidate.executable.path.empty())
    {
        rejectCandidate("executable path must not be empty");
    }
    if (candidate.alias.empty())
    {
        rejectCandidate("alias must not be empty");
    }
    if (candidate.linkPath.empty())
    {
        rejectCandidate("link path must not be empty");
    }
}

[[nodiscard]] SymlinkRepairOutcome dryRunOutcomeFor(LinkStatus status)
{
    if (status == LinkStatus::Missing)
    {
        return SymlinkRepairOutcome::WouldCreate;
    }
    if (status == LinkStatus::Broken)
    {
        return SymlinkRepairOutcome::WouldReplaceBroken;
    }
    if (status == LinkStatus::Ok)
    {
        return SymlinkRepairOutcome::SkippedOk;
    }
    // LinkStatus::Mismatch - the only remaining value.
    return SymlinkRepairOutcome::RefusedMismatch;
}

// Classifies a delete/create/verification failure, upgrading it to
// InsufficientPermission when win32ErrorCode is access-denied or privilege-not-held.
// Until #51 wires the real Developer Mode/elevation queries into production operations,
// both states report Unknown here rather than being guessed.
[[nodiscard]] SymlinkServiceError buildError(SymlinkServiceErrorKind kind, std::string operation,
                                             const std::filesystem::path& path,
                                             std::uint32_t win32ErrorCode,
                                             const SymlinkServiceOperations& operations)
{
    if (win32ErrorCode == ERROR_ACCESS_DENIED || win32ErrorCode == ERROR_PRIVILEGE_NOT_HELD)
    {
        const DeveloperModeState developerMode =
            operations.queryDeveloperMode ? operations.queryDeveloperMode()
                                          : DeveloperModeState::Unknown;
        const ElevationState elevation =
            operations.queryElevation ? operations.queryElevation() : ElevationState::Unknown;
        return SymlinkServiceError(SymlinkServiceErrorKind::InsufficientPermission,
                                   std::move(operation), path, win32ErrorCode, developerMode,
                                   elevation);
    }
    return SymlinkServiceError(kind, std::move(operation), path, win32ErrorCode);
}

// Builds the production SymlinkServiceOperations around the real Win32 APIs and
// inspectLink(). Both Win32-facing paths are normalized through
// paths::toExtendedLengthPath() before the call, matching LinkInspector.cpp's own
// convention. queryDeveloperMode/queryElevation are placeholders until #51 adds the real
// registry and process-token queries - reporting Unknown unconditionally is honest,
// whereas guessing a state this code has not actually queried would not be.
[[nodiscard]] SymlinkServiceOperations makeProductionOperations()
{
    SymlinkServiceOperations operations;
    operations.inspect = [](const PackageExe& executable, const std::wstring& alias,
                            const std::filesystem::path& linkPath) {
        return inspectLink(executable, alias, linkPath);
    };
    operations.deleteEntry = [](const std::filesystem::path& linkPath) -> std::uint32_t {
        const std::filesystem::path extendedPath = paths::toExtendedLengthPath(linkPath);
        if (::DeleteFileW(extendedPath.c_str()))
        {
            return 0;
        }
        return ::GetLastError();
    };
    operations.create = [](const std::filesystem::path& target,
                           const std::filesystem::path& linkPath,
                           std::uint32_t flags) -> std::uint32_t {
        const std::filesystem::path extendedTarget = paths::toExtendedLengthPath(target);
        const std::filesystem::path extendedLink = paths::toExtendedLengthPath(linkPath);
        if (::CreateSymbolicLinkW(extendedLink.c_str(), extendedTarget.c_str(), flags))
        {
            return 0;
        }
        return ::GetLastError();
    };
    operations.queryDeveloperMode = [] { return DeveloperModeState::Unknown; };
    operations.queryElevation = [] { return ElevationState::Unknown; };
    return operations;
}
} // namespace

std::string SymlinkServiceError::buildMessage(SymlinkServiceErrorKind kind,
                                              const std::string& operation,
                                              const std::filesystem::path& path,
                                              std::uint32_t win32ErrorCode)
{
    if (kind == SymlinkServiceErrorKind::VerificationFailed)
    {
        return std::format("Post-repair verification via '{}' did not report Ok for '{}'",
                           operation, toUtf8(path));
    }
    return std::format("{} failed for '{}' (Win32 error {})", operation, toUtf8(path),
                       win32ErrorCode);
}

SymlinkServiceError::SymlinkServiceError(SymlinkServiceErrorKind kind, std::string operation,
                                         std::filesystem::path path,
                                         std::uint32_t win32ErrorCode,
                                         DeveloperModeState developerMode,
                                         ElevationState elevation)
    : std::runtime_error(buildMessage(kind, operation, path, win32ErrorCode)), m_kind(kind),
      m_operation(std::move(operation)), m_path(std::move(path)),
      m_win32ErrorCode(win32ErrorCode), m_developerMode(developerMode), m_elevation(elevation)
{
}

SymlinkRepairResult repairLink(const RepairItem& candidate, RepairMode mode)
{
    static const SymlinkServiceOperations kProductionOperations = makeProductionOperations();
    return repairLink(candidate, mode, kProductionOperations);
}

SymlinkRepairResult repairLink(const RepairItem& candidate, RepairMode mode,
                               const SymlinkServiceOperations& operations)
{
    validateCandidate(candidate);

    const RepairItem fresh =
        operations.inspect(candidate.executable, candidate.alias, candidate.linkPath);

    // Checked before the mode branch below, not just inside the Execute/Broken case: an
    // invalid observation is a programming-contract violation regardless of which mode
    // the caller asked for, and DryRun must not silently report WouldReplaceBroken for a
    // fresh state inspectLink() should never actually produce.
    if (fresh.status == LinkStatus::Broken && fresh.entryKind != LinkEntryKind::SymbolicLink)
    {
        rejectCandidate("a Broken fresh status requires a symbolic-link entry kind");
    }

    if (mode == RepairMode::DryRun)
    {
        return SymlinkRepairResult{fresh, dryRunOutcomeFor(fresh.status), std::nullopt};
    }

    if (fresh.status == LinkStatus::Ok)
    {
        return SymlinkRepairResult{fresh, SymlinkRepairOutcome::SkippedOk, std::nullopt};
    }
    if (fresh.status == LinkStatus::Mismatch)
    {
        return SymlinkRepairResult{fresh, SymlinkRepairOutcome::RefusedMismatch, std::nullopt};
    }
    if (fresh.status == LinkStatus::Broken)
    {
        const std::uint32_t deleteError = operations.deleteEntry(fresh.linkPath);
        if (deleteError != 0)
        {
            throw buildError(SymlinkServiceErrorKind::DeleteFailed, "DeleteFileW",
                             fresh.linkPath, deleteError, operations);
        }
    }
    // LinkStatus::Missing falls straight through to creation below, same as Broken does
    // once its (required) delete above has succeeded.

    constexpr std::uint32_t kCreateFlags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    const std::uint32_t createError =
        operations.create(candidate.executable.path, fresh.linkPath, kCreateFlags);
    if (createError != 0)
    {
        throw buildError(SymlinkServiceErrorKind::CreateFailed, "CreateSymbolicLinkW",
                         fresh.linkPath, createError, operations);
    }

    const RepairItem verified =
        operations.inspect(candidate.executable, candidate.alias, candidate.linkPath);
    if (verified.status != LinkStatus::Ok)
    {
        throw buildError(SymlinkServiceErrorKind::VerificationFailed, "inspectLink",
                         fresh.linkPath, 0, operations);
    }

    const SymlinkRepairOutcome outcome = (fresh.status == LinkStatus::Broken)
                                             ? SymlinkRepairOutcome::ReplacedBroken
                                             : SymlinkRepairOutcome::Created;
    return SymlinkRepairResult{fresh, outcome, verified};
}
} // namespace syncwingetlink
