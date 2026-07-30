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

---

## ADR-0021 — Console: the operations seam, sanitization scope, and per-call (not process-wide) input-mode restoration

- **Date**: 2026-07-30
- **Affected**: `cli/Console`, `docs/TODO.md` M6, the Wiki page
  `plan/syncwingetlink/m6-command-line-interface`
- **Status**: Accepted

### Decision

1. **`Console` is driven through a `ConsoleOperations` seam**, the same
   inject-the-Win32-calls pattern `SymlinkServiceOperations` already establishes
   (ADR-0018/ADR-0019): `isConsole`, `write`, `tryEnableVirtualTerminal`,
   `restoreOutputMode`, and `readLine` are all `std::function` fields. The
   single-argument production constructor wires real `GetStdHandle`/`GetConsoleMode`/
   `WriteConsoleW`/`WriteFile`/`ReadConsoleW` calls; tests supply deterministic fakes.
   Unlike `SymlinkServiceOperations`'s free-function callbacks, `Console`'s production
   `tryEnableVirtualTerminal`/`restoreOutputMode` pair shares a small
   `std::shared_ptr<ProductionOutputModeState>` between two closures, because restoring
   a console mode requires remembering *this instance's* observed original mode across
   the call that changed it and the later call (from `~Console()`) that restores it -
   state `SymlinkService`'s stateless Win32 wrappers never needed.
2. **`sanitizeForDisplay()` strips, rather than escapes, four character classes**: C0
   controls (0x00-0x1F, including ESC) and DEL (0x7F), C1 controls (0x80-0x9F), and the
   Unicode bidi override/isolate characters (U+202A-U+202E, U+2066-U+2069). Stripping
   was chosen over an escaped representation (e.g. `\xNN`) because the sanitized copy is
   for display only and is never fed back into filesystem or comparison logic, so there
   is nothing that needs to round-trip; an escaped form would also itself be
   attacker-influenced text requiring its own display-safety reasoning, which stripping
   avoids entirely. Embedded CR/LF are included in the C0 range and so are also
   stripped - a package name containing a real newline cannot forge an extra output
   line.
3. **`isAffirmative()` accepts only `"y"`/`"yes"` (trimmed, ordinal case-insensitive)**,
   not e.g. a locale's translated affirmative or a bare non-empty line. `nullopt` (no
   line could be read at all) and an empty line (bare Enter) are both refusal, per the
   security contract's "non-interactive stdin is not consent" and "bare Enter is not
   consent" rules - this function is the single place both rules are enforced, so `fix`
   dispatch (#56) has no separate consent-interpretation logic to keep in sync.
4. **NO_COLOR is consulted through an injectable optional value, not read from the
   environment inside every constructor path.** The single-argument production
   constructor performs the real `GetEnvironmentVariableW(L"NO_COLOR", ...)` lookup and
   forwards only *presence* (via `noColorEnvSet()`, a pure function taking the looked-up
   value) into the same shared constructor logic the deterministic test constructor
   uses. The test constructor's override parameter defaults to `nullopt` ("unset")
   rather than performing a real lookup, so a test's outcome never depends on whether
   the actual process running the test suite happens to have `NO_COLOR` set.
5. **Virtual terminal mode is enabled once, at `Console` construction, and restored once,
   from `~Console()`** - a process-lifetime RAII scope, matching the security contract's
   "restored via RAII on every exit path." Input-mode changes for a confirmation prompt
   are the opposite: each `readLine()` call saves stdin's mode, temporarily ORs in
   `ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT`, reads one line, and
   restores the saved mode before returning - self-contained per call, not held for the
   process lifetime. The two differ because output mode is a capability decided once
   and reused for as long as the process runs, while input-mode is only ever needed for
   the narrow duration of one blocking read.
6. **`Console` has no notion of alias collisions.** `confirm()`'s `assumeYes` parameter
   bypasses only the interactive prompt; excluding a colliding candidate from an
   automatic repair (the security contract's collision rule, ADR-0017) is dispatch's
   (#56) responsibility, enforced before `Console::confirm()` or `SymlinkService::
   repairLink()` is ever called for that candidate. `Console.h` documents this
   boundary explicitly to prevent a future change from routing collision handling
   through `confirm()`'s bypass instead.
7. **Redirected-stream `readLine()` reads one byte at a time via `ReadFile`.** This is
   not throughput-oriented; a confirmation-prompt response is at most a handful of
   bytes, and the simplicity of a byte-at-a-time loop (stop at `'\n'`, decode the
   accumulated bytes as UTF-8 once) was preferred over a buffered reader with its own
   overrun/short-read bookkeeping for input this small.

### Reason

- Reusing the `SymlinkServiceOperations` seam pattern rather than inventing a different
  testing strategy keeps this codebase's one established way of making Win32-calling
  code deterministic consistent across `core/` and `cli/`.
- Stripping over escaping for `sanitizeForDisplay()` avoids a whole category of "is the
  escaped representation itself safe to print" follow-up questions, at acceptable cost
  since only display uses this function.
- A strict `"y"/"yes"` allowlist rather than "presence of any non-empty answer" removes
  an entire class of ambiguity (what does an accidental keystroke, or a translated
  affirmative in a non-English locale, mean?) at the one call site (`isAffirmative()`)
  responsible for interpreting consent.
- Making the NO_COLOR lookup injectable, defaulting to "unset" for tests, was chosen
  over reading the real environment variable in both constructors because a test's
  result must not depend on incidental state of the machine running it.

### Consequences

- `src/cli/Console.{h,cpp}` implement `Console`, `ConsoleOperations`, `ConsoleStream`,
  `sanitizeForDisplay()`, `isAffirmative()`, and `noColorEnvSet()` per the description
  above.
- `tests/ConsoleTests.cpp` covers the sanitizer's four stripped character classes,
  `isAffirmative()`'s consent rules (including EOF and bare-Enter refusal), NO_COLOR
  presence detection, `colorEnabled()`'s four-way decision (console × VT-capable ×
  `--no-color` × `NO_COLOR`), the destructor's exactly-once `restoreOutputMode()` call,
  `writeLine()`'s sanitize-then-append-newline behavior, and `confirm()`'s
  `assumeYes`-bypass / EOF / bare-Enter / negative-answer behavior.
- `docs/TODO.md` M6's `Console` line is checked off, pointing at #54 and this ADR.
- #55 (JSON output) is expected to reuse `sanitizeForDisplay()`'s character-class
  decisions for its own untrusted-string escaping, rather than defining a second,
  possibly divergent, set of characters to guard against.
- #56 (dispatch) is responsible for constructing the real `Console`, wiring
  `AppOptions::noColor` into it, calling `confirm()` at the appropriate point in the
  `fix` flow, and enforcing the alias-collision exclusion `Console` itself does not
  know about.
