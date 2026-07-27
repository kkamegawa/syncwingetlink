// SPDX-License-Identifier: MIT

#include "AliasResolver.h"

namespace syncwingetlink
{
std::optional<AliasResolution> resolveAlias(const std::filesystem::path& executablePath,
                                            const RuleSet& rules)
{
    const std::wstring fileName = executablePath.filename().native();

    if (const std::optional<AliasRuleMatch> match = rules.resolve(fileName); match.has_value())
    {
        return AliasResolution{match->alias, match->ruleName};
    }

    if (isValidAliasFileName(fileName))
    {
        return AliasResolution{fileName, std::nullopt};
    }

    return std::nullopt;
}
} // namespace syncwingetlink
