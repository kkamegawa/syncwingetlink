// SPDX-License-Identifier: MIT

#include "LinkInspector.h"

#include "Paths.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>, which is where CompareStringOrdinal lives via <winnls.h>
// (pulled in transitively) - see PackageSourceError.cpp's isPortableInstallerType for the
// same pattern.
#include <Windows.h>

#include <winerror.h>
#include <winioctl.h>

#include <cstring>
#include <format>
#include <memory>
#include <string_view>
#include <vector>

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

// The Windows SDK declares FSCTL_GET_REPARSE_POINT and IO_REPARSE_TAG_SYMLINK (both in
// <Windows.h>) but not the REPARSE_DATA_BUFFER layout needed to interpret what it
// returns - that struct lives in the DDK-only ntifs.h. This is the well-known symbolic-
// link reparse buffer shape, documented publicly in [MS-FSCC] 2.1.2.4, reproduced here as
// plain byte offsets rather than a packed struct so every read can be bounds-checked
// against the buffer we actually received before it happens (see the tests/TempDirectory.h
// mount-point equivalent, used for the reverse - writing - direction).
constexpr std::size_t kReparseHeaderSize = 8; // ReparseTag(4) + ReparseDataLength(2) + Reserved(2)
constexpr std::size_t kSymlinkFixedFieldsSize = 12; // 4 USHORTs + Flags(ULONG), before PathBuffer
constexpr std::size_t kPathBufferStart = kReparseHeaderSize + kSymlinkFixedFieldsSize;
// [MS-FSCC] 2.1.2.4: substitute name is relative to the link's own directory rather than
// an absolute NT path. Not exposed by any public SDK header.
constexpr std::uint32_t kSymlinkFlagRelative = 0x00000001;

[[noreturn]] void rejectReparseData(const std::filesystem::path& linkPath, const char* reason)
{
    throw LinkInspectionError(std::string("DecodeReparseData: ") + reason, linkPath,
                              static_cast<std::uint32_t>(ERROR_INVALID_DATA));
}

// Windows only runs this project on little-endian architectures (x64/ARM64), so this is
// a plain load, not a byte-swap; memcpy avoids relying on reparseBuffer's alignment for a
// direct pointer-cast read.
[[nodiscard]] std::uint32_t readUInt32(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

[[nodiscard]] std::uint16_t readUInt16(std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// Strips the NT-namespace ("\??\") device prefix a symbolic link's absolute substitute
// name uses, converting the NT "UNC\server\share" form back to a normal Win32
// "\\server\share" path. This is a different prefix from the one
// paths::fromExtendedLengthPath handles (Win32's "\\?\"/"\\?\UNC\" extended-length
// forms) - callers apply that helper separately, afterward.
[[nodiscard]] std::wstring stripNtNamespacePrefix(std::wstring_view substituteName)
{
    constexpr std::wstring_view kNtPrefix = LR"(\??\)";
    if (!substituteName.starts_with(kNtPrefix))
    {
        return std::wstring(substituteName);
    }

    const std::wstring_view remainder = substituteName.substr(kNtPrefix.size());
    constexpr std::wstring_view kUncMarker = L"UNC\\";
    const bool isUnc =
        remainder.size() >= kUncMarker.size() &&
        ::CompareStringOrdinal(remainder.data(), static_cast<int>(kUncMarker.size()),
                               kUncMarker.data(), static_cast<int>(kUncMarker.size()),
                               TRUE) == CSTR_EQUAL;
    if (isUnc)
    {
        return LR"(\\)" + std::wstring(remainder.substr(kUncMarker.size()));
    }

    return std::wstring(remainder);
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

std::optional<std::filesystem::path>
decodeSymbolicLinkTarget(std::span<const std::byte> reparseBuffer,
                         const std::filesystem::path& linkPath)
{
    if (reparseBuffer.size() < kReparseHeaderSize)
    {
        rejectReparseData(linkPath, "buffer is smaller than the common reparse header");
    }

    const std::uint32_t reparseTag = readUInt32(reparseBuffer, 0);
    if (reparseTag != IO_REPARSE_TAG_SYMLINK)
    {
        // Some other reparse point (e.g. a mount point) - not this function's concern,
        // and not an error: the rest of the buffer is not validated against this format.
        return std::nullopt;
    }

    const std::uint16_t reparseDataLength = readUInt16(reparseBuffer, 4);
    if (kReparseHeaderSize + reparseDataLength != reparseBuffer.size())
    {
        rejectReparseData(linkPath,
                          "ReparseDataLength does not match the supplied buffer size");
    }
    if (reparseDataLength < kSymlinkFixedFieldsSize)
    {
        rejectReparseData(linkPath,
                          "buffer is too small for the symbolic-link fixed fields");
    }

    const std::uint16_t substituteNameOffset = readUInt16(reparseBuffer, 8);
    const std::uint16_t substituteNameLength = readUInt16(reparseBuffer, 10);
    const std::uint32_t flags = readUInt32(reparseBuffer, 16);

    if (substituteNameOffset % 2 != 0 || substituteNameLength % 2 != 0)
    {
        rejectReparseData(linkPath,
                          "substitute name offset or length is not UTF-16 aligned");
    }

    const std::size_t pathBufferSize = reparseDataLength - kSymlinkFixedFieldsSize;
    const std::size_t substituteNameEnd =
        static_cast<std::size_t>(substituteNameOffset) + substituteNameLength;
    if (substituteNameEnd > pathBufferSize)
    {
        rejectReparseData(linkPath,
                          "substitute name offset/length runs past the path buffer");
    }

    const std::size_t absoluteOffset = kPathBufferStart + substituteNameOffset;
    // Re-derived from reparseBuffer.size() directly rather than trusting the chain of
    // checks above, so no read below this line can exceed the buffer we were given.
    if (absoluteOffset + substituteNameLength > reparseBuffer.size())
    {
        rejectReparseData(linkPath, "substitute name would read past the supplied buffer");
    }

    std::wstring substituteName(substituteNameLength / sizeof(wchar_t), L'\0');
    std::memcpy(substituteName.data(), reparseBuffer.data() + absoluteOffset,
               substituteNameLength);

    const bool isRelative = (flags & kSymlinkFlagRelative) != 0;
    const std::filesystem::path rawTarget =
        isRelative ? (linkPath.parent_path() / substituteName)
                   : std::filesystem::path(stripNtNamespacePrefix(substituteName));

    // Strip any Win32 extended-length ("\\?\") prefix before normalizing, not after: the
    // prefix is a plain string form paths::fromExtendedLengthPath already knows how to
    // remove, and normalizing while it is still present is unnecessary here.
    return paths::fromExtendedLengthPath(rawTarget).lexically_normal();
}

std::optional<std::filesystem::path>
readSymbolicLinkTarget(const std::filesystem::path& linkPath)
{
    const std::filesystem::path extendedPath = paths::toExtendedLengthPath(linkPath);

    // Checked before the HANDLE is wrapped in RAII: CloseHandle(INVALID_HANDLE_VALUE) is
    // itself an invalid call, so the wrapper must not be given a chance to run its
    // deleter on a handle CreateFileW never actually produced.
    const HANDLE rawHandle = ::CreateFileW(
        extendedPath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (rawHandle == INVALID_HANDLE_VALUE)
    {
        throw LinkInspectionError("CreateFileW", linkPath, ::GetLastError());
    }
    const std::unique_ptr<void, decltype(&::CloseHandle)> handle(rawHandle, &::CloseHandle);

    // MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 KiB) is documented publicly but, like
    // REPARSE_DATA_BUFFER itself, only declared in the DDK-only ntifs.h.
    constexpr DWORD kMaximumReparseDataBufferSize = 16 * 1024;
    std::vector<std::byte> buffer(kMaximumReparseDataBufferSize);
    DWORD bytesReturned = 0;
    const BOOL succeeded = ::DeviceIoControl(
        handle.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
        static_cast<DWORD>(buffer.size()), &bytesReturned, nullptr);
    if (!succeeded)
    {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NOT_A_REPARSE_POINT)
        {
            return std::nullopt;
        }
        throw LinkInspectionError("DeviceIoControl", linkPath, error);
    }

    buffer.resize(bytesReturned);
    return decodeSymbolicLinkTarget(buffer, linkPath);
}
} // namespace syncwingetlink
