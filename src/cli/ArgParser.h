// SPDX-License-Identifier: MIT

#pragma once

#include "core/Model.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace syncwingetlink::cli
{
// Why parseArguments() rejected the command line. main.cpp (M6's dispatch, #56) maps
// every kind onto exit code 3 ("argument/config error") - see docs/PLAN.md §8's exit
// code table.
enum class ArgParseErrorKind
{
    UnknownCommand,       // the first positional token is not scan/fix/test-rule
    UnknownOption,        // a token starting with "-"/"--" is not a recognized option
    MissingOptionValue,   // an option that requires a value (e.g. --rules) has none
    InvalidOptionValue,   // an option's value does not belong to its documented set
                          // (e.g. --source neither com, fs, nor auto)
    MissingArgument,      // test-rule was given no NAME token at all
    UnexpectedArgument,   // an extra positional token followed a command that takes none
    InvalidPathOverride,  // --links-dir/--packages-dir/--rules resolved to an empty or
                          // \\.\ device path
    InvalidTestRuleName,  // test-rule's NAME is not a bare, valid alias-shaped file name
    ConflictingOptions,   // --json was combined with fix and no --yes
};

class ArgParseError : public std::runtime_error
{
public:
    ArgParseError(ArgParseErrorKind kind, const std::string& message)
        : std::runtime_error(message), m_kind(kind)
    {
    }

    [[nodiscard]] ArgParseErrorKind kind() const noexcept
    {
        return m_kind;
    }

private:
    ArgParseErrorKind m_kind;
};

// True when either --help/-h or --version was requested. parseArguments() recognizes
// these anywhere *before a "--" terminator* (see below) and short-circuits every other
// validation the rest of the command line would otherwise trigger - a user asking for
// --help should get help even if the rest of the invocation is malformed. Once "--" has
// been seen, every later token (including a literal "--help") is positional instead, so
// `syncwingetlink -- --help` does NOT show help - it fails with UnknownCommand, exactly
// like any other unrecognized first positional token. #57 owns rendering the
// corresponding AppCommand::Help/AppCommand::Version output; this function only sets
// the command.
//
// Parses argv (excluding the program name at argv[0]) into AppOptions, per the surface
// documented in docs/PLAN.md §8:
//
//   Commands: scan (default), fix, test-rule NAME
//   Options: --source com|fs|auto, --tui, --dry-run, --yes/-y, --rules <path>,
//            --packages-dir <path>, --links-dir <path>, --include <glob>,
//            --exclude <glob>, --json, --verbose, --quiet, --fail-on-missing,
//            --no-color, --version, --help/-h
//
// A "--" token stops option parsing; every token after it is positional (this lets a
// test-rule NAME that happens to start with "-" be passed unambiguously).
//
// Path overrides (--links-dir/--packages-dir/--rules) are rejected if empty or if they
// resolve to a "\\.\" device path, and are otherwise stored made-absolute
// (std::filesystem::absolute + lexically_normal) rather than as a raw relative path, so
// AppOptions always holds a stable, displayable path. Deliberately NOT required to
// already exist: an absent Packages directory is a normal, tolerated state (M2's
// FsScanSource treats it as "no packages found", not a failure - docs/adr-phase-2.md
// ADR-0010), and an absent Links directory is the exact condition `fix` exists to
// correct. Existence/readability of --rules is validated downstream by
// rules/RuleSetSelector, which already distinguishes "absent" from "malformed"
// (ADR-0013); duplicating that check here would only let the two disagree.
//
// test-rule's NAME argument is validated with isValidAliasFileName() (rules/RuleSet.h) -
// it must be a bare file name (no path separators, no drive letter), not a path,
// matching the file-name argument #40 (M3) is responsible for resolving and printing.
//
// --json combined with fix and no --yes is rejected: an unattended script that requests
// JSON output but leaves confirmation prompting on would either hang waiting on a
// prompt that a script cannot answer, or (worse) misinterpret a non-interactive refusal
// as something other than the config error it is.
//
// --tui (M7, issue #59) is rejected when combined with scan/test-rule (there is no
// checklist to show - `fix` is the only command with candidates to repair), or with
// --json/--yes (both imply an unattended, scriptable invocation, the opposite of an
// interactive checklist). Note that #59's TUI is a presentation choice layered on top
// of `fix`, not a fourth command of its own - AppOptions::useTui stays a bool alongside
// AppCommand::Fix, exactly as it already does.
//

// Throws ArgParseError for any of the conditions above. Never touches AppOptions fields
// this function does not own, and never performs a filesystem-mutating operation itself.
[[nodiscard]] AppOptions parseArguments(const std::vector<std::wstring>& args);
} // namespace syncwingetlink::cli
