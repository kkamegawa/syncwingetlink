// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>

namespace syncwingetlink::paths
{
[[nodiscard]] std::filesystem::path getLocalAppDataDirectory();

[[nodiscard]] std::filesystem::path
getLinksDirectory(const std::optional<std::filesystem::path>& overridePath = std::nullopt);

[[nodiscard]] std::filesystem::path
getPackagesDirectory(const std::optional<std::filesystem::path>& overridePath = std::nullopt);
} // namespace syncwingetlink::paths
