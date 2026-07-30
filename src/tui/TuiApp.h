// SPDX-License-Identifier: MIT

#pragma once

#include "ChecklistModel.h"
#include "TerminalSession.h"

#include <vector>

namespace syncwingetlink::tui
{
// The result of running the checklist to completion - either the user pressed Enter
// (possibly with nothing selected, which is a successful no-op per the documented
// contract) or cancelled via Escape, Q, or Ctrl+C.
enum class ChecklistOutcome
{
    Confirmed,
    Cancelled,
};

struct ChecklistRunResult
{
    ChecklistOutcome outcome{ChecklistOutcome::Cancelled};
    // Populated only when outcome is Confirmed; empty (not populated) when Cancelled -
    // a cancellation never authorizes any repair regardless of what had been toggled.
    std::vector<ChecklistCandidate> selectedCandidates;
};

// Drives model through session: renders the checklist, reads key/resize events, and
// returns once the user confirms (Enter) or cancels (Escape/Q/Ctrl+C). Never itself
// decides whether a selected candidate is actually repaired - that remains the
// caller's job (cli::Dispatch for #59; the shared executor for #60), via the same
// repairLink() every non-interactive path already uses.
//
// Package-derived text (each candidate's alias and executable path) is passed through
// cli::sanitizeForDisplay() before being combined with the intentional TUI control
// sequences this function writes - never combined raw, matching the sanitization
// boundary cli::Console::writeLine() already enforces for the non-interactive paths.
//
// A read failure from session (session.readEvent() returning nullopt - e.g. the input
// handle became invalid) is treated as a cancellation: no repair is authorized either
// way, and looping forever on a dead input source would just hang.
[[nodiscard]] ChecklistRunResult runChecklist(TerminalSession& session, ChecklistModel& model);
} // namespace syncwingetlink::tui
