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

// Collects the *.exe files below root. Entries carrying FILE_ATTRIBUTE_REPARSE_POINT are
// skipped so symlinks and junctions cannot form a traversal loop; see docs/PLAN.md
// section 6. Returns an empty vector when root does not exist or is not a directory.
[[nodiscard]] std::vector<PackageExe> collectExecutables(const std::filesystem::path& root);

// True when the path exists and carries FILE_ATTRIBUTE_REPARSE_POINT (symlink, junction,
// or any other reparse tag).
[[nodiscard]] bool isReparsePoint(const std::filesystem::path& path);
} // namespace syncwingetlink
