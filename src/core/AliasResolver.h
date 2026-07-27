// SPDX-License-Identifier: MIT

#pragma once

#include "rules/RuleSet.h"

#include <filesystem>
#include <optional>
#include <string>

namespace syncwingetlink
{
struct AliasResolution
{
    std::wstring alias;

    // The rule that produced alias, or nullopt when the raw-file-name tier was used
    // instead (no rule matched, or the matching rule's result was invalid - see
    // RuleSet::resolve()).
    std::optional<std::wstring> matchedRuleName;
};

// Decides <alias>.exe for a real executable file, in priority order:
//
//   (1) COM metadata (a PortableCommandAlias-equivalent) - permanently unreachable, and
//       therefore not implemented at all rather than coded as a branch that always falls
//       through. The winget COM API exposes no such field, so WingetComSource can never
//       populate one; see docs/adr-phase-2.md ADR-0009 and ADR-0012.
//   (2) the first rule in rules whose pattern matches the file name in full
//       (RuleSet::resolve()).
//   (3) the raw executable file name, used as-is if it is itself a well-formed alias file
//       name (isValidAliasFileName()).
//
// Returns nullopt only if neither tier 2 nor tier 3 produces a valid alias - i.e. the raw
// file name itself is not well-formed. Not expected in practice (ExecutableScanner and
// WingetComSource both only ever report *.exe files reached by a directory walk), but
// checked rather than assumed: this is the boundary where a rule-authoring or
// path-construction bug would otherwise silently propagate an unusable alias downstream.
[[nodiscard]] std::optional<AliasResolution>
resolveAlias(const std::filesystem::path& executablePath, const RuleSet& rules);
} // namespace syncwingetlink
