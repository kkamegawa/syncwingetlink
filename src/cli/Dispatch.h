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
