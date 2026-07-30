# Architecture Decision Records — M6 Phase

This file continues the chronological record in [`adr-phase-4.md`](./adr-phase-4.md).

---

## ADR-0020 — CLI argument model, and the path-override validation scope narrowed from the M6 review's first draft

- **Date**: 2026-07-30
- **Affected**: `cli/ArgParser`, `docs/PLAN.md` §8, `docs/TODO.md` M6, the Wiki page
  `plan/syncwingetlink/m6-command-line-interface`
- **Status**: Accepted

### Decision

1. **`parseArguments()` takes `const std::vector<std::wstring>&`**, not `int argc,
   wchar_t* argv[]` directly. `main.cpp` (#56) is expected to construct this vector from
   `wmain`'s `argv[1..]` once it exists. Keeping the parser's entry point a plain value
   type rather than a raw pointer pair keeps it trivially testable without constructing
   real `argv` arrays, and avoids tying the parser itself to `wmain`'s exact signature.
2. **`--help`/`-h`/`--version` short-circuit every other validation.** They are detected
   token-by-token during the same scan as every other option, but as soon as one is seen
   (and no `--` terminator has appeared yet), `parseArguments()` returns immediately with
   `AppCommand::Help`/`AppCommand::Version` set, ignoring the rest of the command line —
   including an otherwise-invalid option or command earlier or later in `argv`. A user
   asking for help should get help even if the rest of the invocation is malformed.
3. **A `--` terminator stops option recognition.** Every token after it is treated as
   positional, letting a `test-rule` `NAME` that happens to start with `-` be passed
   unambiguously (`isValidAliasFileName()` does not itself forbid a leading hyphen).
4. **`--json` combined with `fix` and no `--yes` is a parse-time error
   (`ArgParseErrorKind::ConflictingOptions`)**, not deferred to dispatch. A script that
   requests JSON output almost certainly cannot answer an interactive confirmation
   prompt, so the conflict is caught before any enumeration or filesystem work begins.
5. **Path-override validation is narrower than this review's own first draft
   specified**, corrected here after implementing against the existing codebase:
   - `--links-dir`, `--packages-dir`, and `--rules` are rejected only when empty or when
     they resolve to a `\\.\` device path.
   - They are **not** required to already exist. The M6 Wiki page's first draft (and
     this issue's original acceptance criteria) said the opposite — "rejected if not an
     existing directory" — which would have contradicted `docs/adr-phase-2.md`
     ADR-0010's decision that an absent Packages directory is a normal, tolerated state
     (`FsScanSource` reports zero packages, not a failure) and would have made it
     impossible to run `fix` the very first time, before `Links` has ever been created —
     exactly the situation the tool exists to correct.
   - `--rules`'s existence and readability stay `rules::RuleSetSelector`'s job
     (`loadRuleSetFromFile()`), which ADR-0013 already documents as distinguishing an
     absent user-rules file (falls through) from a malformed one (fatal). Re-checking
     existence in `ArgParser` would only create a second place for that policy to drift
     from the first.
   - Accepted values are still normalized to an absolute, `lexically_normal` path
     (`std::filesystem::absolute` when not already absolute) so `AppOptions` always
     holds a stable, displayable value — independent of the process's current directory
     at the moment the value is later echoed or consumed.
6. **`Paths::toExtendedLengthPath()` was found to already make a relative path absolute
   before prefixing it** (`path.is_absolute() ? path : std::filesystem::absolute(path)`,
   `src/core/Paths.cpp`), contradicting a second claim in this review's first draft (that
   a relative path had to be made absolute "before" that function, implying it does not
   do so itself). `ArgParser`'s own absolutization (point 5) is therefore for its own
   reason — a stable value in `AppOptions` — not a workaround for a bug that does not
   exist.
7. **`--fail-on-missing` and `--no-color` are added to `docs/PLAN.md` §8's option
   table.** `AppOptions::failOnMissing` already existed (added with M5) but was never
   listed in the CLI spec; `AppOptions::noColor` is new, added by this issue for #54 to
   consume. `docs/PLAN.md` §8 also gains exit code `4` (package enumeration failed),
   needed because an explicit `--source com`/`--source fs` failure previously had no
   documented destination — `3` means "your arguments/config are wrong", which a
   data-source outage is not.

### Reason

- Points 5 and 6 are corrections, not new design: the M6 review that produced the Wiki
  page and the original sub-issue bodies was done by reading `docs/PLAN.md`, the ADRs,
  and the *headers* of `core/Paths.h`, but before writing `cli/ArgParser.cpp` against
  the actual implementation in `Paths.cpp` and against `FsScanSource`'s documented
  absent-directory tolerance. Implementing the narrower, correct rule here — rather than
  the wiki page's first-draft wording — avoids shipping a validation rule that would
  make ordinary, already-supported use (a fresh machine's first `fix`, or pointing
  `--packages-dir` at a not-yet-populated test fixture) fail with a spurious
  configuration error.
- Rejecting device paths and empty values, without requiring existence, still closes the
  actual risk the original finding was chasing — a nonsensical or dangerous override
  value — without contradicting ADR-0010's or ADR-0013's already-accepted tolerance
  policies.
- `--json` + `fix` without `--yes` fails at parse time rather than dispatch because the
  contradiction is visible from `AppOptions` alone, with no need to touch the
  filesystem or a package source first; catching it earlier gives a script a faster,
  cheaper failure.

### Consequences

- `src/cli/ArgParser.{h,cpp}` implement `parseArguments()`, `ArgParseError`, and
  `ArgParseErrorKind` per the description above.
- `core/Model.h`'s `AppOptions` gains `bool noColor{false}`.
- `tests/ArgParserTests.cpp` covers every command, every option, the `--` terminator,
  the `--json`/`fix`/`--yes` conflict, `--help`/`--version` short-circuiting, and the
  corrected path-override rules (empty and device-path rejection; relative-path
  absolutization; a non-existent override path is accepted, not rejected).
- `docs/PLAN.md` §8 gains `--fail-on-missing`, `--no-color`, and exit code `4`.
- `docs/TODO.md` M6's `ArgParser` line is checked off, pointing at #53 and this ADR.
- The Wiki page `plan/syncwingetlink/m6-command-line-interface` was corrected in place
  (its S5 and W8 rows) rather than left with the inaccurate first-draft wording, since
  the page is meant to be the durable, accurate record other M6 sub-issues (#54–#57,
  the rules-input hardening issue) build against.
- #56 (dispatch and exit codes) is responsible for constructing `AppOptions` from real
  `wmain` argv, mapping `ArgParseErrorKind` and every other error kind onto the exit
  codes in `docs/PLAN.md` §8, and echoing the resolved effective paths before any
  mutating operation — none of that is implemented yet.
