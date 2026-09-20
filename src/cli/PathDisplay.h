// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace syncwingetlink::cli
{
// How paths are rendered for a human. Threaded explicitly through every presentation
// entry point (cli::ScanReport, cli::Json, tui::TuiApp) rather than held as a
// process-wide setting: a global would make these otherwise-pure functions
// order-dependent under the unit tests, which is exactly what the cli//core split
// exists to avoid.
struct PathDisplayOptions
{
    // --showspecialfolder / -s (docs/adr-phase-10.md ADR-0048).
    bool showSpecialFolders{false};
};

// One known folder and the environment-variable spelling that stands in for it.
// `root` carries no trailing separator.
struct KnownFolderMapping
{
    std::wstring root;
    std::wstring variable;
};

// Rewrites a leading known-folder prefix as its %NAME% form, or returns `path`
// unchanged when nothing matches.
//
// Matching rules (docs/adr-phase-10.md ADR-0048):
//   - Ordinal, case-insensitive, like every other path comparison in this codebase.
//   - Component-boundary only: the character after the matched prefix must be a
//     separator or the end of the string, so "C:\Users\bob" never rewrites
//     "C:\Users\bob2\...".
//   - Longest match wins. %LOCALAPPDATA% lives under %USERPROFILE%, so scan order alone
//     would produce the less useful of the two.
//
// The folder table is a parameter rather than queried internally so this stays a pure
// function testable against a synthetic table, with no dependency on the machine the
// tests run on.
[[nodiscard]] std::wstring abbreviateKnownFolders(std::wstring_view path,
                                                  std::span<const KnownFolderMapping> folders);

// The three user-scoped folders whose real form carries the account name:
// %LOCALAPPDATA%, %APPDATA%, %USERPROFILE%. Queried once via SHGetKnownFolderPath and
// cached. A folder the shell declines to report is simply absent from the table - this
// never throws, since failing to abbreviate a path is cosmetic, not fatal (unlike
// paths::getLocalAppDataDirectory(), which cannot proceed without its answer).
[[nodiscard]] std::span<const KnownFolderMapping> knownFolderMappings();

// The single entry point every path-printing site uses: abbreviate (when asked), then
// sanitize. Doing it in that order matters - sanitizeForDisplay() must be the last thing
// applied to anything reaching a terminal or a JSON document.
[[nodiscard]] std::wstring formatPathForDisplay(const std::filesystem::path& path,
                                                const PathDisplayOptions& options);
} // namespace syncwingetlink::cli
