// SPDX-License-Identifier: MIT

#pragma once

// SYNCWINGETLINK_VER_MAJOR/MINOR/PATCH are defined project-wide by
// props/syncwingetlink.common.props, ultimately sourced from Directory.Build.props's
// single ProductVersion property (issue #118, docs/adr-phase-6.md ADR-0032) - the same
// property src/syncwingetlink.rc's VS_VERSION_INFO resource derives its FILEVERSION/
// PRODUCTVERSION from. The #ifndef fallbacks below only matter for a tool that parses
// this header outside a full MSBuild invocation (e.g. a standalone clang-tidy run or
// IDE Intellisense pass); an actual build always has the real values defined. All three
// fall back to the neutral sentinel 0, not the current release's actual numbers - a
// real version literal here would silently drift from ProductVersion after the next
// version bump and defeat the point of a fallback that's supposed to make a missing
// define obvious rather than plausible-looking.
#ifndef SYNCWINGETLINK_VER_MAJOR
#define SYNCWINGETLINK_VER_MAJOR 0
#endif
#ifndef SYNCWINGETLINK_VER_MINOR
#define SYNCWINGETLINK_VER_MINOR 0
#endif
#ifndef SYNCWINGETLINK_VER_PATCH
#define SYNCWINGETLINK_VER_PATCH 0
#endif

// clang-format off
#define SYNCWINGETLINK_STRINGIZE2(s) #s
#define SYNCWINGETLINK_STRINGIZE(s) SYNCWINGETLINK_STRINGIZE2(s)
#define SYNCWINGETLINK_WIDEN2(s) L##s
#define SYNCWINGETLINK_WIDEN(s) SYNCWINGETLINK_WIDEN2(s)
#define SYNCWINGETLINK_WSTRINGIZE(s) SYNCWINGETLINK_WIDEN(SYNCWINGETLINK_STRINGIZE(s))
// clang-format on

namespace syncwingetlink::cli
{
// The single source of truth for this CLI's own reported version - the one place
// `--version` (cli/Dispatch.cpp's printVersion()) reads from, so no second literal is
// hardcoded anywhere else in cli/. Generated from SYNCWINGETLINK_VER_MAJOR/MINOR/PATCH
// above rather than a hand-written literal, so a version bump means changing
// Directory.Build.props's ProductVersion property in exactly one place.
// src/app.manifest's assemblyIdentity version stays hand-maintained (docs/adr-phase-5.md
// ADR-0025) - unifying that too would need a manifest-generation build step this project
// does not have - but syncwingetlink.vcxproj's VerifyManifestVersionMatchesProductVersion
// target fails the build if it drifts from this same ProductVersion property
// (docs/adr-phase-6.md ADR-0032).
inline constexpr wchar_t kVersion[] =
    SYNCWINGETLINK_WSTRINGIZE(SYNCWINGETLINK_VER_MAJOR)
    L"." SYNCWINGETLINK_WSTRINGIZE(SYNCWINGETLINK_VER_MINOR)
    L"." SYNCWINGETLINK_WSTRINGIZE(SYNCWINGETLINK_VER_PATCH);
} // namespace syncwingetlink::cli

#undef SYNCWINGETLINK_STRINGIZE2
#undef SYNCWINGETLINK_STRINGIZE
#undef SYNCWINGETLINK_WIDEN2
#undef SYNCWINGETLINK_WIDEN
#undef SYNCWINGETLINK_WSTRINGIZE
