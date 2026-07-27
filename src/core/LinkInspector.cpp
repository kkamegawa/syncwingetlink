// SPDX-License-Identifier: MIT

#include "LinkInspector.h"

#include <format>

namespace syncwingetlink
{
namespace
{
[[noreturn]] void rejectObservation(const char* reason)
{
    throw std::invalid_argument(std::string("Invalid LinkObservation: ") + reason);
}

// path.string() transcodes through the process's narrow (ACP) locale, which can mangle
// or throw on a non-ASCII path. path.u8string() transcodes to UTF-8 independently of any
// locale; the reinterpret_cast from char8_t to char is the standard, compiler-supported
// interop idiom for consuming a u8string as a narrow std::string (P1423).
[[nodiscard]] std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

void validateObservation(const LinkObservation& observation)
{
    if (observation.entryKind == LinkEntryKind::SymbolicLink)
    {
        if (!observation.decodedTarget.has_value())
        {
            rejectObservation("a symbolic link must carry its decoded target");
        }
        if (observation.targetRelation == TargetRelation::NotApplicable)
        {
            rejectObservation(
                "a symbolic link must resolve to Missing, SameFile, or DifferentFile");
        }
        return;
    }

    // None, RegularFile, and OtherReparsePoint never have a decoded target or a
    // meaningful target relation - only a symbolic link's substitute name is decoded.
    if (observation.decodedTarget.has_value())
    {
        rejectObservation("a decoded target is only valid for a symbolic link");
    }
    if (observation.targetRelation != TargetRelation::NotApplicable)
    {
        rejectObservation(
            "target relation must be NotApplicable without a symbolic link");
    }
}

LinkStatus statusFor(const LinkObservation& observation)
{
    if (observation.entryKind == LinkEntryKind::None)
    {
        return LinkStatus::Missing;
    }
    if (observation.entryKind != LinkEntryKind::SymbolicLink)
    {
        // RegularFile or OtherReparsePoint.
        return LinkStatus::Mismatch;
    }
    if (observation.targetRelation == TargetRelation::Missing)
    {
        return LinkStatus::Broken;
    }
    if (observation.targetRelation == TargetRelation::SameFile)
    {
        return LinkStatus::Ok;
    }
    // TargetRelation::DifferentFile - NotApplicable was already rejected by
    // validateObservation().
    return LinkStatus::Mismatch;
}
} // namespace

std::string LinkInspectionError::buildMessage(const std::string& operation,
                                               const std::filesystem::path& path,
                                               std::uint32_t win32ErrorCode)
{
    return std::format("{} failed for '{}' (Win32 error {})", operation, toUtf8(path),
                        win32ErrorCode);
}

RepairItem classifyLink(const LinkObservation& observation, PackageExe executable,
                         std::wstring alias, std::filesystem::path linkPath)
{
    validateObservation(observation);

    RepairItem item;
    item.executable = std::move(executable);
    item.alias = std::move(alias);
    item.linkPath = std::move(linkPath);
    item.entryKind = observation.entryKind;
    item.status = statusFor(observation);
    item.existingTarget = observation.decodedTarget;
    return item;
}
} // namespace syncwingetlink
