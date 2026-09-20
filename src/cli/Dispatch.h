// SPDX-License-Identifier: MIT

#pragma once

#include "core/Model.h"
#include "core/PackageSourceError.h"
#include "core/SymlinkService.h"
#include "rules/RuleSet.h"

#include <optional>
#include <string>
#include <vector>

namespace syncwingetlink::cli
{
// The process exit codes documented in docs/PLAN.md §8. main.cpp returns
// static_cast<int>(...) of whichever value run() below settles on.
enum class ExitCode : int
{
    Success = 0,
    FixNeeded = 1,
    InsufficientPermission = 2,
    ArgumentError = 3,
    PackageEnumerationFailed = 4,
    PartialFailure = 10,
};

// Total over every PackageSourceErrorKind value - every enumeration failure maps to
// ExitCode::PackageEnumerationFailed regardless of which kind, per docs/PLAN.md §8
// ("any PackageSourceErrorKind surfaced by an explicit --source com/fs").
[[nodiscard]] ExitCode exitCodeFor(PackageSourceErrorKind kind) noexcept;

// Total over every RuleSetErrorKind value - every rules.json/rules-input failure maps
// to ExitCode::ArgumentError, including the new LimitExceeded/RegexEvaluationFailed
// kinds #105 added.
[[nodiscard]] ExitCode exitCodeFor(RuleSetErrorKind kind) noexcept;

// Total over every SymlinkServiceErrorKind value - InsufficientPermission maps to
// ExitCode::InsufficientPermission; every other kind (DeleteFailed/CreateFailed/
// VerificationFailed) maps to ExitCode::PartialFailure, since a single failing item in
// an otherwise-successful batch is the "some repairs failed" case, not the permission
// case.
[[nodiscard]] ExitCode exitCodeFor(SymlinkServiceErrorKind kind) noexcept;

// A TUI checklist must not be shown when the requested elevation is declined or
// intentionally suppressed. The line-oriented fix flow may continue and report its
// per-item permission result, so it keeps the historical continue behavior.
[[nodiscard]] std::optional<ExitCode> exitCodeAfterElevationDeclined(bool useTui) noexcept;

// What a candidate's LinkStatus becomes in the `fix --tui` checklist.
enum class ChecklistRowKind
{
    // Not shown. Nothing to decide, and a healthy inventory would bury the rows that
    // matter.
    NotListed,
    // Shown with a checkbox. Checking it is consent to create or replace that link.
    Selectable,
    // Shown as `[-] ... [cannot repair]`, and no key press can select it. The user still
    // has to know the entry is there, but repairLink() always refuses it.
    Informational,
};

// The single place a LinkStatus is mapped onto its role in the checklist
// (docs/adr-phase-10.md ADR-0047): Missing/Broken are selectable, Mismatch is
// informational, Ok is not listed. tui::ChecklistModel deliberately never interprets a
// LinkStatus itself - it stays a pure selection state machine - so this mapping is the
// load-bearing half of that decision, and is exported here (like
// exitCodeAfterElevationDeclined above) so it can be tested directly rather than only
// through runFix(), which needs a real console, filesystem, and package source.
//
// Total over every LinkStatus value, so adding one fails to compile here rather than
// silently defaulting a new state into or out of the checklist.
[[nodiscard]] ChecklistRowKind checklistRowKindFor(LinkStatus status) noexcept;

// Runs the whole CLI given already-parsed process arguments (excluding argv[0] - the
// program name). This is the only function main.cpp calls; kept separate from wmain so
// it is testable without a real process, console, or COM apartment - a caller
// constructs Console/AppOptions or supplies argv exactly as production code would, but
// nothing here depends on wmain's exact signature.
//
// Every exception type this module's own dependencies can throw (ArgParseError,
// PackageSourceError, RuleSetError, SymlinkServiceError, LinkInspectionError, and
// std::filesystem::filesystem_error) is caught inside run() and mapped to the
// documented exit code; run() itself is not expected to throw. main.cpp's own
// top-level try/catch (docs/adr-phase-5.md ADR-0024) exists as a last-resort net for
// anything that still escapes despite that - a defensive backstop, not a normal path.
[[nodiscard]] int run(const std::vector<std::wstring>& args);
} // namespace syncwingetlink::cli
