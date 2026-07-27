// SPDX-License-Identifier: MIT

#pragma once

#include "Model.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

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
} // namespace syncwingetlink
