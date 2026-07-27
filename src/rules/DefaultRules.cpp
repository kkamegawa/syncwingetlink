// SPDX-License-Identifier: MIT

#include "DefaultRules.h"

namespace syncwingetlink
{
RuleSet defaultRules()
{
    // Order matters: RuleSet::resolve() applies the first matching rule. The Rust
    // target-triple rule is listed first because it is the more specific of the two -
    // matching it ahead of the broader version/architecture rule keeps a Rust-built
    // executable's alias derivation independent of whether that second rule's pattern
    // happens to also match. See docs/rules.md for the documented samples this mirrors.
    std::vector<AliasRule> rules;

    rules.push_back(AliasRule{
        L"strip-rust-target-triple",
        L"^(.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
        L"$1.exe",
        true,
    });

    rules.push_back(AliasRule{
        L"strip-version-and-arch",
        L"^(.+?)[-_]v?\\d+\\.\\d+[^\\\\/]*?(windows|win)?[-_]?(amd64|x64|arm64)?\\.exe$",
        L"$1.exe",
        true,
    });

    return RuleSet(std::move(rules));
}
} // namespace syncwingetlink
