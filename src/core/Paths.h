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

[[nodiscard]] std::filesystem::path
toExtendedLengthPath(const std::filesystem::path& path);

// Reverses toExtendedLengthPath: strips a leading "\\?\" or rewrites "\\?\UNC\" back to
// "\\". A "\\.\" device path has no non-extended equivalent and is returned unchanged.
// Paths stored in the domain model (InstalledPackage, PackageExe) must go through this
// before leaving a scanning function, since \\?\-prefixed paths compare unequal to the
// paths callers naturally construct (rules, --json output, console display).
[[nodiscard]] std::filesystem::path
fromExtendedLengthPath(const std::filesystem::path& path);
} // namespace syncwingetlink::paths
