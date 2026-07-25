// SPDX-License-Identifier: MIT

#pragma once

#include "Model.h"

#include <filesystem>
#include <vector>

namespace syncwingetlink
{
// Maximum directory depth walked below the scan root. Portable packages nest their
// payload only a few levels deep, so this is a loop guard rather than a real limit.
inline constexpr int kMaxScanDepth = 16;

// Collects the *.exe files below root, sorted by path for deterministic output.
// std::filesystem::recursive_directory_iterator does not, by default, descend into a
// symlink or a junction (MSVC's _Is_symlink_or_junction gate on _Should_recurse), so no
// extra reparse-point check is needed to prevent a traversal loop; see docs/PLAN.md
// section 6 and docs/adr-phase-2.md ADR-0009. A depth cap is kept as a second guard in
// case of a reparse tag future MSVC versions treat differently.
// Returns an empty vector when root does not exist or is not a directory. Paths in the
// result never carry the \\?\ long-path prefix, even though the walk uses it internally.
[[nodiscard]] std::vector<PackageExe> collectExecutables(const std::filesystem::path& root);

// True when the path exists and carries FILE_ATTRIBUTE_REPARSE_POINT (symlink, junction,
// or any other reparse tag).
[[nodiscard]] bool isReparsePoint(const std::filesystem::path& path);
} // namespace syncwingetlink
