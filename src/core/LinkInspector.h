// SPDX-License-Identifier: MIT

#pragma once

#include "Model.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace syncwingetlink
{
// How an observed link entry's decoded target relates to the package executable
// syncwingetlink expects it to point at. Meaningful only when the entry is a symbolic
// link; every other LinkEntryKind must report NotApplicable (classifyLink rejects any
// other combination - see its documentation).
enum class TargetRelation
{
    NotApplicable,
    Missing,
    SameFile,
    DifferentFile,
};

// A filesystem-independent snapshot of what was found at Links\<alias>.exe. Produced by
// the Win32 probe added in #45/#46 and consumed by the pure classifyLink() below; kept
// separate from RepairItem so classifyLink can be unit tested without any filesystem or
// Win32 dependency.
struct LinkObservation
{
    LinkEntryKind entryKind{LinkEntryKind::None};
    // The decoded symbolic-link target. Present if and only if entryKind is
    // SymbolicLink - it reflects the raw decoded path whether or not that path
    // currently exists, so it is set even when targetRelation is Missing.
    std::optional<std::filesystem::path> decodedTarget;
    TargetRelation targetRelation{TargetRelation::NotApplicable};
};

// A Win32-level failure encountered while probing a link entry: access denial, a
// sharing violation, malformed reparse data, an expected package executable that
// disappears during inspection, or another unexpected I/O failure (docs/adr-phase-3.md).
// Not used for an invalid classifyLink() observation, which is a programming-contract
// violation rather than an operational failure - see classifyLink's documentation for
// why that case throws std::invalid_argument instead.
class LinkInspectionError : public std::runtime_error
{
public:
    LinkInspectionError(std::string operation, std::filesystem::path path,
                         std::uint32_t win32ErrorCode)
        : std::runtime_error(buildMessage(operation, path, win32ErrorCode)),
          m_operation(std::move(operation)), m_path(std::move(path)),
          m_win32ErrorCode(win32ErrorCode)
    {
    }

    // The Win32/CRT operation that failed, e.g. "CreateFileW" or "DeviceIoControl".
    [[nodiscard]] const std::string& operation() const noexcept
    {
        return m_operation;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

    [[nodiscard]] std::uint32_t win32ErrorCode() const noexcept
    {
        return m_win32ErrorCode;
    }

private:
    static std::string buildMessage(const std::string& operation,
                                     const std::filesystem::path& path,
                                     std::uint32_t win32ErrorCode);

    std::string m_operation;
    std::filesystem::path m_path;
    std::uint32_t m_win32ErrorCode;
};

// Converts a valid LinkObservation into a complete RepairItem by applying the M4
// classification contract (docs/adr-phase-3.md ADR-0014):
//
//   | Observation                                        | Status   |
//   |-----------------------------------------------------|----------|
//   | No entry                                             | Missing  |
//   | Regular file                                         | Mismatch |
//   | Non-symlink reparse point                            | Mismatch |
//   | Symbolic link with an absent target                  | Broken   |
//   | Symbolic link resolving to the expected file         | Ok       |
//   | Symbolic link resolving to another existing file     | Mismatch |
//
// executable, alias, and linkPath are carried through into the returned RepairItem
// unchanged; only entryKind, status, and existingTarget are derived from observation.
//
// Pure and filesystem-independent - it performs no I/O, so it can be exercised directly
// by deterministic unit tests without any symlink privilege or filesystem fixture.
//
// Throws std::invalid_argument (never LinkInspectionError, which is reserved for real
// Win32 operation failures) if observation does not describe one of the rows above: a
// decoded target reported for a non-symbolic-link entry, a symbolic link missing its
// decoded target, or a symbolic link left at TargetRelation::NotApplicable.
[[nodiscard]] RepairItem classifyLink(const LinkObservation& observation,
                                       PackageExe executable, std::wstring alias,
                                       std::filesystem::path linkPath);

// Decodes a symbolic link's target from the raw bytes a
// DeviceIoControl(FSCTL_GET_REPARSE_POINT) call returned for linkPath. Pure and
// filesystem-independent - reparseBuffer is consumed as given, so malformed-buffer tests
// need no real reparse point or symlink privilege.
//
// Returns std::nullopt if reparseBuffer's ReparseTag is not IO_REPARSE_TAG_SYMLINK - the
// entry is some other kind of reparse point (e.g. a mount point), which the caller
// classifies LinkEntryKind::OtherReparsePoint rather than decoding as a symbolic link.
// That is not an error.
//
// Throws LinkInspectionError(win32ErrorCode() == ERROR_INVALID_DATA) if the tag is
// IO_REPARSE_TAG_SYMLINK but the buffer is otherwise malformed: shorter than the common
// reparse header, a ReparseDataLength inconsistent with reparseBuffer's actual size, or a
// substitute-name offset/length that is misaligned for UTF-16 or would read outside the
// buffer. No read this function performs can exceed reparseBuffer's bounds.
//
// A relative substitute name (SYMLINK_FLAG_RELATIVE set) is resolved against
// linkPath.parent_path(), not the process's current directory. An absolute substitute
// name's NT-namespace ("\??\", including the "\??\UNC\" form) prefix is stripped - this
// is not the same prefix as paths::fromExtendedLengthPath handles, which only recognizes
// the Win32 "\\?\"/"\\?\UNC\" extended-length forms; the result is still passed through
// that helper afterward so a target already expressed in extended-length form is
// normalized too. Public model paths therefore never retain either kind of prefix
// unnecessarily.
[[nodiscard]] std::optional<std::filesystem::path>
decodeSymbolicLinkTarget(std::span<const std::byte> reparseBuffer,
                         const std::filesystem::path& linkPath);

// Reads and decodes linkPath's symbolic-link target via CreateFileW
// (FILE_FLAG_OPEN_REPARSE_POINT) and DeviceIoControl(FSCTL_GET_REPARSE_POINT), then
// decodeSymbolicLinkTarget() above.
//
// Returns std::nullopt when linkPath is not a symbolic link: a genuine
// ERROR_NOT_A_REPARSE_POINT result (a regular file) or a reparse point whose data
// decodeSymbolicLinkTarget() reports is some other tag. Neither is an error.
//
// Throws LinkInspectionError for every other failure - CreateFileW or DeviceIoControl
// returning an unexpected error, or a malformed buffer per decodeSymbolicLinkTarget().
[[nodiscard]] std::optional<std::filesystem::path>
readSymbolicLinkTarget(const std::filesystem::path& linkPath);

// The production, filesystem-backed M4 probe: inspects linkPath and returns a complete
// RepairItem, combining GetFileAttributesW classification, readSymbolicLinkTarget()
// above, a FILE_ID_INFO (volume serial + 128-bit file id) identity comparison against
// executable, and classifyLink()'s classification contract. Read-only - performs no
// filesystem mutation.
//
// - GetFileAttributesW distinguishes absence from a regular file from a reparse point.
//   Only a clean ERROR_FILE_NOT_FOUND/ERROR_PATH_NOT_FOUND result is
//   LinkEntryKind::None; any other GetFileAttributesW failure throws
//   LinkInspectionError - it is never mislabeled Missing.
// - A reparse point that readSymbolicLinkTarget() reports is not a symbolic link becomes
//   LinkEntryKind::OtherReparsePoint (classifyLink then reports Mismatch); it is never
//   treated as a healthy link.
// - For a symbolic link, the decoded target's existence and identity are checked only
//   once a target was actually decoded: a clean absence is TargetRelation::Missing (an
//   access or sharing failure on the target is not mislabeled this way - it throws).
//   Only when the decoded target does exist is executable's own identity read, to
//   compare - so executable disappearing between package enumeration and this call
//   surfaces as LinkInspectionError, not a status a caller could plan a repair around.
[[nodiscard]] RepairItem inspectLink(PackageExe executable, std::wstring alias,
                                      std::filesystem::path linkPath);

// Groups items by alias, using ordinal case-insensitive comparison (the same
// CompareStringOrdinal-based policy PackageFilter and isPortableInstallerType already
// use, for the same reason: locale-dependent case folding must not change the answer).
// Within a group, executables are deduplicated by path (also ordinal
// case-insensitive) before counting - repeated RepairItems for the same executable
// (e.g. observed twice across a merged scan) are one executable, not a collision by
// themselves. A group is only reported once it has at least two distinct executables.
//
// Output is fully deterministic and independent of items's order: collision groups are
// sorted by alias, and each group's executables are sorted by path, both using the same
// ordinal comparison.
//
// Pure and filesystem-independent - items is consumed as given, already-computed
// RepairItems; this performs no I/O of its own.
[[nodiscard]] std::vector<AliasCollision> detectAliasCollisions(std::span<const RepairItem> items);
} // namespace syncwingetlink
