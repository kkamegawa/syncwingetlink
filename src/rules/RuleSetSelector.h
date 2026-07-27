// SPDX-License-Identifier: MIT

#pragma once

#include "RuleSet.h"

#include <filesystem>
#include <functional>
#include <optional>

namespace syncwingetlink
{
// Reads path as UTF-8 text (a leading BOM is tolerated and stripped) and parses it via
// RuleSet::parse(). Throws RuleSetError(FileReadError) if the file cannot be opened or
// read; propagates whatever RuleSet::parse() throws if the content is malformed. Never
// substitutes a default on failure - that decision belongs to the caller (selectRuleSet()
// below), since the same function is used for both an explicit --rules path (any failure
// is fatal) and the auto-discovered user rules file (only absence, not failure, falls
// through).
[[nodiscard]] RuleSet loadRuleSetFromFile(const std::filesystem::path& path);

// Returns the location to check for the user rules file. Injectable so tests can point
// selectRuleSet() at a temporary directory rather than the real user profile; production
// code should pass paths::getUserRulesFilePath (selectRuleSet()'s default).
using UserRulesPathProvider = std::function<std::filesystem::path()>;

[[nodiscard]] std::filesystem::path defaultUserRulesPathProvider();

// Selects the effective RuleSet, in priority order (docs/PLAN.md §7, docs/rules.md):
//
//   1. explicitPath, if set (--rules <path>). Required to exist and parse: a missing,
//      unreadable, or invalid file is a propagated RuleSetError and never falls through
//      to a lower tier.
//   2. the user rules file, at whatever path userRulesPath() returns. An absent file
//      falls through to (3) - that is the normal, unconfigured state. A file that exists
//      but is unreadable or invalid is a propagated RuleSetError, exactly like tier 1:
//      silently falling back to embedded defaults would hide a config mistake the user
//      made themselves.
//   3. defaultRules() (rules/DefaultRules.h), when neither of the above applies.
[[nodiscard]] RuleSet
selectRuleSet(const std::optional<std::filesystem::path>& explicitPath,
             const UserRulesPathProvider& userRulesPath = defaultUserRulesPathProvider);
} // namespace syncwingetlink
