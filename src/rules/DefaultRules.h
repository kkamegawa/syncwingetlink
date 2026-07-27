// SPDX-License-Identifier: MIT

#pragma once

#include "RuleSet.h"

namespace syncwingetlink
{
// The alias replacement rules embedded in the binary (docs/rules.md), used when neither
// an explicit --rules path nor a user rules.json is selected - see the source-priority
// issue (#42) for how this fits into that ordering. Built directly from
// std::vector<AliasRule> rather than round-tripping through RuleSet::parse(), so no
// apartment is needed just to obtain the defaults.
[[nodiscard]] RuleSet defaultRules();
} // namespace syncwingetlink
