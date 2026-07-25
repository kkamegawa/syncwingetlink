// SPDX-License-Identifier: MIT

#pragma once

#include <Windows.h>
#include <winioctl.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace syncwingetlink::tests
{
namespace detail
{
// winioctl.h declares FSCTL_SET_REPARSE_POINT but not the REPARSE_DATA_BUFFER layout
// needed to fill it in (that lives in the DDK-only ntifs.h). This is the well-known
// user-mode shape for a mount-point (junction) reparse point.
#pragma pack(push, 1)
struct MountPointReparseBuffer
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1];
};
#pragma pack(pop)
} // namespace detail

// Creates target as an NTFS junction (mount point) rather than a symlink, so the caller
// does not need Developer Mode or elevation - only write access to the parent directory.
// Returns false (and leaves linkPath uncreated) if junction creation fails for any
// reason.
[[nodiscard]] inline bool createJunction(const std::filesystem::path& target,
                                         const std::filesystem::path& linkPath)
{
    if (!::CreateDirectoryW(linkPath.c_str(), nullptr))
    {
        return false;
    }

    const std::unique_ptr<void, decltype(&::CloseHandle)> handle(
        ::CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr),
        &::CloseHandle);
    if (handle.get() == INVALID_HANDLE_VALUE)
    {
        std::error_code ignored;
        std::filesystem::remove(linkPath, ignored);
        return false;
    }

    const std::wstring substituteName = LR"(\??\)" + target.native();
    const std::wstring printName = target.native();

    constexpr std::size_t kBufferSize = sizeof(detail::MountPointReparseBuffer) + 2 * MAX_PATH * sizeof(WCHAR);
    std::vector<char> buffer(kBufferSize, 0);
    auto* reparse = reinterpret_cast<detail::MountPointReparseBuffer*>(buffer.data());

    reparse->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->SubstituteNameOffset = 0;
    reparse->SubstituteNameLength =
        static_cast<USHORT>(substituteName.size() * sizeof(WCHAR));
    reparse->PrintNameOffset =
        static_cast<USHORT>(reparse->SubstituteNameLength + sizeof(WCHAR));
    reparse->PrintNameLength = static_cast<USHORT>(printName.size() * sizeof(WCHAR));
    // ReparseDataLength counts everything after the 8-byte
    // {ReparseTag, ReparseDataLength, Reserved} header: the two name-offset/length
    // fields plus both NUL-terminated name buffers.
    constexpr USHORT kNameFieldsSize = 4 * sizeof(USHORT);
    reparse->ReparseDataLength = static_cast<USHORT>(
        kNameFieldsSize + reparse->SubstituteNameLength + sizeof(WCHAR) +
        reparse->PrintNameLength + sizeof(WCHAR));

    std::memcpy(reinterpret_cast<char*>(reparse->PathBuffer) + reparse->SubstituteNameOffset,
               substituteName.c_str(), reparse->SubstituteNameLength);
    std::memcpy(reinterpret_cast<char*>(reparse->PathBuffer) + reparse->PrintNameOffset,
               printName.c_str(), reparse->PrintNameLength);

    constexpr DWORD kHeaderSize = sizeof(ULONG) + 2 * sizeof(USHORT);
    const DWORD totalSize = kHeaderSize + reparse->ReparseDataLength;

    DWORD bytesReturned = 0;
    const BOOL result =
        ::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(), totalSize,
                         nullptr, 0, &bytesReturned, nullptr);
    if (!result)
    {
        std::error_code ignored;
        std::filesystem::remove(linkPath, ignored);
        return false;
    }

    return true;
}

// Creates a unique directory under the system temp folder and removes it (and everything
// below it) on destruction, so a failing assertion cannot leave test data behind.
class TempDirectory
{
public:
    explicit TempDirectory(const std::wstring& label)
    {
        static std::atomic<unsigned long long> counter{0};
        const unsigned long long id = counter.fetch_add(1);
        m_path = std::filesystem::temp_directory_path() /
                 (L"syncwingetlink-" + label + L"-" + std::to_wstring(id));

        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        std::filesystem::create_directories(m_path);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return m_path;
    }

    // Creates every missing parent directory and writes a placeholder file.
    std::filesystem::path createFile(const std::filesystem::path& relative) const
    {
        const std::filesystem::path target = m_path / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream stream(target, std::ios::binary);
        stream << "test";
        return target;
    }

    std::filesystem::path createDirectory(const std::filesystem::path& relative) const
    {
        const std::filesystem::path target = m_path / relative;
        std::filesystem::create_directories(target);
        return target;
    }

private:
    std::filesystem::path m_path;
};
} // namespace syncwingetlink::tests
