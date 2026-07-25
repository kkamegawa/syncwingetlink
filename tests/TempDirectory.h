// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace syncwingetlink::tests
{
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
