// SPDX-License-Identifier: MIT

#pragma once

#include "Model.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace syncwingetlink
{
// Whether repairLink() may mutate the filesystem or must only report what it would do.
enum class RepairMode
{
    Execute,
    DryRun,
};

// What repairLink() did (or would do) for one candidate, derived from a fresh
// inspectLink() call made immediately before any decision (docs/adr-phase-4.md) - never
// from the status already recorded on the RepairItem the caller supplied.
enum class SymlinkRepairOutcome
{
    WouldCreate,
    WouldReplaceBroken,
    Created,
    ReplacedBroken,
    SkippedOk,
    RefusedMismatch,
};

// The complete outcome of one repairLink() call.
struct SymlinkRepairResult
{
    // The fresh RepairItem observed immediately before the outcome was decided - not the
    // (possibly stale) candidate the caller passed in.
    RepairItem preActionItem;
    SymlinkRepairOutcome outcome{SymlinkRepairOutcome::SkippedOk};
    // Set only when outcome is Created or ReplacedBroken: the post-creation
    // re-inspection, which repairLink() has already confirmed reports status Ok.
    std::optional<RepairItem> postActionItem;
};

// Whether Windows Developer Mode is enabled. Queried only after a mutating Win32 call
// fails with a permission-shaped error (docs/adr-phase-4.md) - never inferred from that
// failure itself. Unknown means the query could not be answered, not that the setting is
// off.
enum class DeveloperModeState
{
    Enabled,
    Disabled,
    Unknown,
};

// Whether the current process token is elevated, queried under the same condition as
// DeveloperModeState above.
enum class ElevationState
{
    Elevated,
    NotElevated,
    Unknown,
};

enum class SymlinkServiceErrorKind
{
    // The handle-based delete (see SymlinkServiceOperations::deleteEntry) or
    // CreateSymbolicLinkW failed with ERROR_ACCESS_DENIED or ERROR_PRIVILEGE_NOT_HELD;
    // developerModeState()/elevationState() carry the queried states.
    InsufficientPermission,
    DeleteFailed,
    CreateFailed,
    VerificationFailed,
};

// A failure repairLink() encountered while repairing a link: an unrecoverable delete
// (see SymlinkServiceOperations::deleteEntry) or CreateSymbolicLinkW failure, or a
// successful creation whose immediate post-create re-inspection did not report Ok.
// Distinct from std::invalid_argument, which repairLink() throws instead for a
// programming-contract violation (an empty candidate field, or a fresh Broken status
// paired with a non-symbolic-link entry kind) - no Win32 operation is attempted in that
// case, matching LinkInspector.h's classifyLink() split (docs/adr-phase-3.md ADR-0014).
class SymlinkServiceError : public std::runtime_error
{
public:
    SymlinkServiceError(SymlinkServiceErrorKind kind, std::string operation,
                        std::filesystem::path path, std::uint32_t win32ErrorCode,
                        DeveloperModeState developerMode = DeveloperModeState::Unknown,
                        ElevationState elevation = ElevationState::Unknown);

    [[nodiscard]] SymlinkServiceErrorKind kind() const noexcept
    {
        return m_kind;
    }

    // The operation that failed: "deleteEntry" (the handle-based delete;
    // docs/adr-phase-7.md ADR-0035 - no longer literally "DeleteFileW", since the delete
    // is no longer a single by-name Win32 call) or "CreateSymbolicLinkW". "inspectLink"
    // for VerificationFailed, which is not itself a failed Win32 call.
    [[nodiscard]] const std::string& operation() const noexcept
    {
        return m_operation;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

    // 0 for VerificationFailed, which has no underlying Win32 failure of its own.
    [[nodiscard]] std::uint32_t win32ErrorCode() const noexcept
    {
        return m_win32ErrorCode;
    }

    // Meaningful only when kind() is InsufficientPermission; Unknown otherwise.
    [[nodiscard]] DeveloperModeState developerModeState() const noexcept
    {
        return m_developerMode;
    }

    // Meaningful only when kind() is InsufficientPermission; Unknown otherwise.
    [[nodiscard]] ElevationState elevationState() const noexcept
    {
        return m_elevation;
    }

private:
    static std::string buildMessage(SymlinkServiceErrorKind kind, const std::string& operation,
                                    const std::filesystem::path& path,
                                    std::uint32_t win32ErrorCode);

    SymlinkServiceErrorKind m_kind;
    std::string m_operation;
    std::filesystem::path m_path;
    std::uint32_t m_win32ErrorCode;
    DeveloperModeState m_developerMode;
    ElevationState m_elevation;
};

// The Win32-facing operations repairLink() drives. The single-argument repairLink()
// overload builds this around the real APIs; tests supply deterministic callbacks so
// every mutation and error branch is covered without symlink privilege, Developer Mode,
// or elevation.
struct SymlinkServiceOperations
{
    // Re-inspects executable/alias/linkPath and returns the fresh RepairItem. Production
    // code calls inspectLink(); repairLink() never uses a candidate's own stored status
    // as this result.
    std::function<RepairItem(const PackageExe& executable, const std::wstring& alias,
                             const std::filesystem::path& linkPath)>
        inspect;

    // Deletes the entry at linkPath. The production implementation opens linkPath once by
    // name, re-verifies on that handle that the entry is still a file symbolic link (not
    // a directory, not some other reparse tag), and deletes by handle rather than a
    // second by-name call - closing the TOCTOU window between repairLink()'s fresh
    // inspection and the delete itself (docs/adr-phase-7.md ADR-0035). Returns 0 on
    // success; otherwise either the failed Win32 call's GetLastError() value, or
    // ERROR_INVALID_DATA if the re-verification found the entry no longer matches what
    // was inspected (no Win32 call itself failed in that case).
    std::function<std::uint32_t(const std::filesystem::path& linkPath)> deleteEntry;

    // Creates a file symbolic link at linkPath targeting target, with the exact flags
    // repairLink() would pass to CreateSymbolicLinkW (always including
    // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE; never the directory-link flag, since
    // syncwingetlink only ever links executables). Returns 0 on success, otherwise the
    // failed call's GetLastError() value.
    std::function<std::uint32_t(const std::filesystem::path& target,
                                const std::filesystem::path& linkPath, std::uint32_t flags)>
        create;

    // Queries Windows Developer Mode. Called only after deleteEntry/create fails with a
    // permission-shaped error.
    std::function<DeveloperModeState()> queryDeveloperMode;

    // Queries the current process token's elevation state. Called under the same
    // condition as queryDeveloperMode above.
    std::function<ElevationState()> queryElevation;
};

// Safely repairs one candidate's command-alias symbolic link, re-inspecting it
// immediately before deciding anything rather than trusting candidate's own stored status
// (docs/adr-phase-4.md). Never touches an entry whose fresh state is Ok, and never
// deletes anything but a symbolic link whose fresh state is Broken.
//
// - Missing: create a file symbolic link (Execute) or report WouldCreate (DryRun).
// - Broken with a fresh SymbolicLink entry kind: delete then recreate (Execute), or
//   report WouldReplaceBroken (DryRun).
// - Ok: return SkippedOk without mutation, in both modes.
// - Mismatch: return RefusedMismatch without mutation, in both modes. A regular file, a
//   non-symlink reparse point, and a symbolic link resolving to another existing file are
//   never deleted, even in Execute mode.
//
// Every successful Created/ReplacedBroken outcome is confirmed by re-inspecting linkPath
// after creation; a post-create inspection that does not report Ok throws
// SymlinkServiceError(VerificationFailed) rather than being reported as success. If
// deleting a broken link succeeds but the subsequent creation fails, the failure is
// reported as-is - the deleted entry was already broken, and no rollback is synthesized
// (docs/adr-phase-4.md).
//
// Throws std::invalid_argument if candidate.executable.path, candidate.alias, or
// candidate.linkPath is empty, or if the fresh inspection reports Broken paired with an
// entry kind other than SymbolicLink - both are programming-contract violations, not
// operational failures (see SymlinkServiceError's documentation).
//
// Throws SymlinkServiceError for a delete (SymlinkServiceOperations::deleteEntry) or
// CreateSymbolicLinkW failure, or a failed post-create verification. Neither delete,
// create, nor verification is ever invoked in DryRun mode.
[[nodiscard]] SymlinkRepairResult repairLink(const RepairItem& candidate, RepairMode mode);

// Same contract as above, driven entirely through operations rather than the real Win32
// APIs and inspectLink() - the deterministic test seam.
[[nodiscard]] SymlinkRepairResult repairLink(const RepairItem& candidate, RepairMode mode,
                                             const SymlinkServiceOperations& operations);

// Public query helpers for startup-time policy checks. These use the same registry/token
// sources the permission-classification path records after a mutating failure.
[[nodiscard]] DeveloperModeState queryDeveloperMode() noexcept;
[[nodiscard]] ElevationState queryElevation() noexcept;
} // namespace syncwingetlink
