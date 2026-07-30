// SPDX-License-Identifier: MIT

#pragma once

namespace syncwingetlink::cli
{
// The single source of truth for this CLI's own reported version - the one place
// `--version` (cli/Dispatch.cpp's printVersion()) reads from, so no second literal is
// hardcoded anywhere else in cli/. Kept in sync with src/app.manifest's
// assemblyIdentity version attribute by convention: no build-time unification exists
// between the two yet (that would need a versioned resource/manifest-generation step,
// which is a larger, separate concern - a candidate for M8's release tooling, not this
// issue's scope), so a version bump means updating both by hand.
inline constexpr wchar_t kVersion[] = L"0.1.0";
} // namespace syncwingetlink::cli
