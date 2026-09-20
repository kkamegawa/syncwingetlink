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
//   - An extended-length path is matched in its non-extended form. `--links-dir` and
//     friends accept a `\\?\`-prefixed path verbatim (ArgParser rejects only `\\.\`),
//     and SHGetKnownFolderPath never reports one, so a literal comparison would leave
//     exactly those paths carrying the account name. A path that matches nothing is
//     still returned byte-for-byte as it came in, `\\?\` included.
//
// The folder table is a parameter rather than queried internally so this stays a pure
// function testable against a synthetic table, with no dependency on the machine the
// tests run on.
[[nodiscard]] std::wstring abbreviateKnownFolders(std::wstring_view path,
                                                  std::span<const KnownFolderMapping> folders);

// The same substitution applied to every occurrence *inside* a longer piece of text,
// for diagnostics that embed a path mid-sentence rather than printing one on its own -
// core's exception messages, which are formatted in core/ (as UTF-8) long before
// cli::Dispatch decides how to display them, e.g.
// "CreateSymbolicLinkW failed for 'C:\Users\...\Links\tool.exe' (Win32 error 5)".
//
// Boundary rule, slightly wider than the prefix form above: an occurrence is replaced
// when the character after it is a separator, the end of the string, or a quote. The
// quote allowance is what catches a message naming the folder itself, since this
// codebase's diagnostics wrap a path in '...'. The trade-off is a directory whose own
// name begins with an apostrophe directly after a known-folder root ("C:\Users\bob's
// stuff" where bob is also the profile) would be abbreviated one component early - a
// cosmetic mis-render in a case that does not occur in practice, weighed against
// leaving the account name in every error message.
[[nodiscard]] std::wstring abbreviateKnownFoldersInText(std::wstring_view text,
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

// The diagnostic counterpart of formatPathForDisplay(), for a message that embeds a
// path rather than being one. Callers still sanitize afterwards the way they already
// did - this only substitutes, so the existing sanitize-at-the-Console boundary is
// unchanged.
[[nodiscard]] std::wstring formatDiagnosticForDisplay(std::wstring_view text,
                                                      const PathDisplayOptions& options);
} // namespace syncwingetlink::cli
