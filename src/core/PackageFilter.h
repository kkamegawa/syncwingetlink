// SPDX-License-Identifier: MIT

#pragma once

#include "Model.h"

#include <string>
#include <string_view>
#include <vector>

namespace syncwingetlink
{
// Matches value against a wildcard pattern, ordinal and case-insensitive.
//
// The supported syntax is deliberately small: '*' matches any run of characters
// (including none) and '?' matches exactly one. Character classes ('[a-z]'), brace
// expansion and path-separator semantics are NOT supported - the values matched here are
// a package identifier or a bare executable file name, never a path, so a pattern has no
// directory structure to reason about. See docs/adr-phase-2.md ADR-0010.
//
// Comparison is ordinal (CompareStringOrdinal), not locale-sensitive, for the same reason
// isPortableInstallerType() is: a locale-dependent case fold such as the Turkish dotless-i
// must not change whether a user's --include matches. '?' consumes one UTF-16 code unit,
// so it matches half of a surrogate pair rather than a whole non-BMP character; package
// identifiers and executable names in practice are BMP, and '*' is unaffected.
[[nodiscard]] bool matchesGlob(std::wstring_view pattern, std::wstring_view value) noexcept;

// The --include / --exclude selection over enumerated packages.
//
// Matching is per-executable, and a pattern is tested against both the owning package's
// identifier and the executable's file name - so `--include OpenAI.Codex` and
// `--include codex*.exe` both select the same executable. An exclude match always wins
// over an include match, and a package whose executables are all filtered out is dropped
// entirely rather than reported with an empty list.
//
// This is pure logic with no filesystem or COM dependency: it is applied to the result of
// IPackageSource::enumeratePackages(), not inside a source, so both sources behave
// identically. Wiring it to AppOptions::includePatterns/excludePatterns happens in the
// M6 CLI, which is the first code with an AppOptions to hand.
class PackageFilter
{
public:
    PackageFilter() = default;
    PackageFilter(std::vector<std::wstring> includePatterns,
                  std::vector<std::wstring> excludePatterns);

    [[nodiscard]] bool includesExecutable(std::wstring_view packageId,
                                          std::wstring_view executableFileName) const;

    // Returns the packages that survive filtering, each holding only its selected
    // executables. Packages left with no executables are removed.
    [[nodiscard]] std::vector<InstalledPackage> apply(std::vector<InstalledPackage> packages) const;

    // True when no pattern was supplied at all, in which case apply() is the identity.
    [[nodiscard]] bool isEmpty() const noexcept;

private:
    std::vector<std::wstring> m_includePatterns;
    std::vector<std::wstring> m_excludePatterns;
};
} // namespace syncwingetlink
