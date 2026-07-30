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

---

## ADR-0022 — JSON schema, string-escaping rules, and the explicit surrogate policy

- **Date**: 2026-07-30
- **Affected**: `cli/Json`, `docs/PLAN.md` §8, `docs/TODO.md` M6, the Wiki page
  `plan/syncwingetlink/m6-command-line-interface`
- **Status**: Accepted

### Decision

1. **A `schemaVersion` field, not implicit versioning.** Both the `scan` and `fix`
   documents start with `{"schemaVersion":1, ...}`, following `rules.json`'s own
   precedent (`docs/rules.md`) for a stable, explicitly-versioned document shape rather
   than an unversioned one a script would have no reliable way to detect a future
   breaking change against.
2. **`escapeJsonString()` implements RFC 8259's string-escaping rules directly** (the
   six named two-character escapes, `\uXXXX` for every other C0 control character,
   ordinary UTF-8 for everything else), rather than reusing a general-purpose
   wide-to-UTF-8 conversion function anywhere else in this codebase. Every existing
   `toUtf8()` helper (`rules/RuleSetSelector.cpp`, `cli/ArgParser.cpp`,
   `cli/Console.cpp`) calls `WideCharToMultiByte(CP_UTF8, 0, ...)` with no
   `WC_ERR_INVALID_CHARS` flag, which silently substitutes U+FFFD for an invalid
   sequence and has no JSON-escaping behavior at all - reusing it here would have left
   the surrogate policy and the control-character escaping implicit and inconsistent
   with RFC 8259, rather than the explicit, tested policy this ADR records.
3. **An unpaired UTF-16 surrogate becomes U+FFFD (REPLACEMENT CHARACTER), encoded as its
   own valid three-byte UTF-8 sequence.** Windows file names are technically WTF-16, not
   strict UTF-16, so an unpaired surrogate is a real (if rare) possibility in a package
   identifier or executable file name. The alternative failure modes - producing
   ill-formed UTF-8 a downstream JSON parser would reject outright, or throwing and
   aborting the entire `--json` output over one untrusted string - were both rejected in
   favor of a documented, lossy-but-valid substitution. A valid surrogate *pair* is
   combined into the one non-BMP code point it represents and encoded as 4-byte UTF-8,
   never left as two separate 3-byte "surrogate" encodings (which would themselves be
   invalid UTF-8, this being exactly the mistake CESU-8 makes).
4. **`toJsonString(std::wstring_view)` and a path-taking overload were found to be
   ambiguous and had to be split into two differently-named functions**
   (`toJsonString`/`toJsonPathString`), discovered as a compile error while implementing
   this issue: `std::filesystem::path`'s converting constructor accepts any
   `wchar_t`-sequence `Source`, so a plain `std::wstring` argument is an equally valid
   implicit conversion target for *both* an overload taking `std::wstring_view` and one
   taking `const std::filesystem::path&` - neither conversion is preferred by overload
   resolution, so every call site passing a bare `std::wstring` (which is what
   `cli::sanitizeForDisplay()` and `LinkStatus`/`LinkEntryKind`/`SymlinkRepairOutcome`
   name lookups all return) failed to compile. This is a narrow API-naming lesson, not a
   design change: the two functions are documented as distinct on this basis so a future
   caller does not attempt to reintroduce the overload.
5. **JSON serialization reuses `cli::sanitizeForDisplay()` (`Console.h`, ADR-0021) for
   every untrusted string field** (`executable`, `alias`, `linkPath`, `existingTarget`,
   each `AliasCollision` executable path) before escaping. JSON and console output share
   one sanitization boundary rather than defining a second, possibly divergent, one for
   the same category of untrusted input.
6. **`toJsonScanResult()`/`toJsonFixResult()` produce only the JSON text.** Neither
   function writes anything, decides where diagnostics go, or knows about `--json`'s
   stdout-purity requirement (the security contract's "`--json` stream purity" rule) -
   that remains dispatch's (#56) responsibility once a real `AppOptions`/`Console` pair
   exists to enforce it against.
7. **`fix`'s JSON output serializes `SymlinkRepairResult` per candidate, plus the
   collisions dispatch excluded before running `repairLink()` at all** - collisions
   never appear inside `results`, matching how `scan`'s `collisions` array is already
   reported separately from `repairItems` (`core/Model.h`'s `AliasCollision`, ADR-0017).

### Reason

- An RFC-8259-direct escaper, rather than one built on the codebase's existing lenient
  `toUtf8()` helpers, is what makes the surrogate policy a deliberate, tested decision
  instead of an accidental side effect of whichever conversion function happened to be
  reused.
- Substituting U+FFFD for an unpaired surrogate keeps `--json` output usable and valid
  even against the rare untrusted input that could otherwise break it, at the
  well-precedented cost (the same one Unicode's own replacement-character convention
  exists for) of losing the exact original byte sequence in that one field.

### Consequences

- `src/cli/Json.{h,cpp}` implement `escapeJsonString()`, `toJsonString()`,
  `toJsonPathString()`, `toJsonBool()`, `toJson(const RepairItem&)`,
  `toJson(const AliasCollision&)`, `toJson(const SymlinkRepairResult&)`,
  `toJsonScanResult()`, and `toJsonFixResult()`.
- `docs/PLAN.md` §8 gains the full `--json` output schema (both `scan` and `fix` shapes)
  under a new "`--json` output schema" subsection.
- `tests/JsonTests.cpp` covers RFC 8259 escaping (named escapes, `\uXXXX` for other C0
  controls, ordinary and non-BMP UTF-8 encoding, valid surrogate pairs, unpaired high
  and low surrogates including one at the end of the string), and domain serialization
  for every type `cli::Json` renders, including the sanitize-before-escape property and
  both wrapper documents' empty-input shape.
- `docs/TODO.md` M6's `--json output` line is checked off, pointing at #55 and this ADR.
- #56 (dispatch) is responsible for actually calling `toJsonScanResult()`/
  `toJsonFixResult()` from real `scan`/`fix` runs, enforcing that stdout carries only
  that text when `--json` is set, and routing every other message to stderr instead.

---

## ADR-0023 — Rules-file input bounds: size/count/field-length caps, and the match-time `regex_error` guard

- **Date**: 2026-07-30
- **Affected**: `rules/RuleSet`, `rules/RuleSetSelector`, `docs/TODO.md` M6, the Wiki
  page `plan/syncwingetlink/m6-command-line-interface`
- **Status**: Accepted

### Decision

1. **Three new bounds, all defined once in `rules/RuleSet.h` as `inline constexpr`
   values** so every caller and test shares one source of truth:
   - `kMaxRulesFileBytes` (1 MiB) - the largest rules file `loadRuleSetFromFile()` will
     read into memory at all.
   - `kMaxRuleCount` (1000) - the largest number of rules `RuleSet`'s constructor will
     accept.
   - `kMaxRuleFieldLength` (4096 characters) - the longest a single rule's `name`,
     `pattern`, or `replacement` may be.

   All three are generous relative to any legitimate rules file - the embedded default
   rule set (`rules/DefaultRules.cpp`) has 2 rules, none of whose fields exceed 100
   characters - while still bounding memory use and match-time cost against a hostile
   or merely corrupted file. A rules file is untrusted input from the moment it is
   read, whether it came from `--rules` or the user's own
   `%LOCALAPPDATA%\syncwingetlink\rules.json`.
2. **The file-size cap is enforced two ways in `loadRuleSetFromFile()`, with the read
   itself as the actual bound.** A `std::filesystem::file_size()` pre-check, before the
   file is even opened, cheaply rejects an absurdly large file without allocating
   anything for it. But the read that follows does not trust that pre-check as the
   only gate: it reads at most `kMaxRulesFileBytes + 1` bytes into a fixed-size buffer
   (`stream.read(...)`, checked via `gcount()`), so a `file_size()` query failure (e.g.
   a race with the file being deleted) or a race that grows the file between the
   pre-check and the read can never cause more than that bound to be pulled into
   memory - **corrected from this issue's own first version**, which checked size only
   via the pre-check and then read the whole file unboundedly
   (`buffer << stream.rdbuf()`) regardless of what that pre-check found, a gap a
   Copilot review of this issue's PR caught before merge.
3. **The rule-count cap is checked before the per-rule validation loop begins** in
   `RuleSet(std::vector<AliasRule>)`, so an oversized rule list is rejected without
   first compiling any of its patterns. The per-field length caps are checked at the
   start of each rule's iteration, before the existing name-emptiness/uniqueness checks
   and before pattern compilation - a rule that fails a length cap never reaches
   `std::wregex`'s constructor.
4. **A new error kind, `RegexEvaluationFailed`, is distinct from the existing
   `InvalidRegex`.** `InvalidRegex` covers a pattern that fails to *compile*
   (`RuleSet`'s constructor, `std::wregex`'s own constructor throwing).
   `RegexEvaluationFailed` covers a pattern that compiled successfully but whose
   compiled `std::wregex` throws `std::regex_error` later, while *matching* input
   inside `resolve()` - confirmed to be a real, fast-triggering condition under MSVC's
   STL: a classic catastrophic-backtracking pattern (`^(a+)+$`) matched against a
   30-character non-matching input throws in well under 100ms in this codebase's own
   test (`tests/RuleSetTests.cpp`'s
   `aPatternThatFailsAtMatchTimeReportsRegexEvaluationFailed`), rather than hanging or
   silently returning an incorrect answer. `resolve()`'s only other outcomes remain
   `nullopt` (no match, or a match producing an invalid alias) or a successful
   `AliasRuleMatch` - it never throws for any reason other than this one.
5. **Both caps and the match-time guard are enforced regardless of which rules tier is
   in play** (`--rules`, the user rules file, or - vacuously, since it is small and
   fixed - the embedded defaults): `RuleSet`'s constructor and `resolve()` are the
   single implementation both `RuleSet::parse()` and every caller share, so there is
   only one place these rules could ever be bypassed.

### Reason

- A single named constant per bound (rather than a magic number repeated at each check
  site) is what let this issue's own tests assert exact boundary behavior (`exactly
  the limit is accepted`, `one over the limit is rejected`) without hardcoding a
  second copy of each number that could drift from the implementation.
- Checking file size before reading avoids allocating a buffer for the full contents
  of an arbitrarily large file - the cheapest possible rejection for the most
  attacker-cheap attack (supplying a huge file).
- Splitting `RegexEvaluationFailed` from `InvalidRegex` matters because the two failures
  happen at completely different times relative to `RuleSet` construction - a caller
  that only ever expected `RuleSet`'s constructor to throw would otherwise be surprised
  by `resolve()`, a `const` query method, also being able to throw. Naming the two
  kinds differently makes that surprising possibility visible in the type itself rather
  than only in a comment.
- The catastrophic-backtracking pattern was verified empirically (not assumed) before
  writing this ADR, specifically because the M6 review that first flagged this risk
  (before any of #53-#55's code existed) could not yet confirm MSVC's std::regex
  actually throws here rather than merely running slowly; the passing test in this
  issue is that confirmation.

### Consequences

- `rules/RuleSet.h` gains `kMaxRulesFileBytes`, `kMaxRuleCount`, `kMaxRuleFieldLength`,
  `RuleSetErrorKind::LimitExceeded`, and `RuleSetErrorKind::RegexEvaluationFailed`.
- `rules/RuleSet.cpp`'s constructor and `resolve()`, and `rules/RuleSetSelector.cpp`'s
  `loadRuleSetFromFile()`, enforce the bounds described above.
- `tests/RuleSetTests.cpp` gains coverage for the rule-count boundary (exactly at the
  limit accepted, one over rejected), each field's length boundary, and the
  match-time `regex_error` guard. `tests/RuleSetSelectorTests.cpp` gains coverage for
  the file-size boundary (exactly at the limit accepted, one byte over rejected).
- #56 (dispatch) maps `RuleSetErrorKind::LimitExceeded` and
  `RuleSetErrorKind::RegexEvaluationFailed` onto exit code 3, alongside every other
  `RuleSetErrorKind`, once its exit-code mapping exists.

---

## ADR-0024 — main.cpp, the total exit-code map, the scan/fix pipeline, and the ComApartment ownership move

- **Date**: 2026-07-30
- **Affected**: `main.cpp` (new), `cli/Dispatch` (new), `core/WingetComSource`,
  `core/ComApartment`, `docs/TODO.md` M6, the Wiki page
  `plan/syncwingetlink/m6-command-line-interface`
- **Status**: Accepted

### Decision

1. **`main.cpp` is a thin shim, exactly as ADR-0002 requires**: `SetDefaultDllDirectories
   (LOAD_LIBRARY_SEARCH_SYSTEM32)` as its first statement, one process-wide
   `core::ComApartment` constructed before anything else, `argv[1..]` copied into a
   `std::vector<std::wstring>`, one call to `cli::run()`, and a top-level `try`/`catch`
   as a last-resort net. `/DEPENDENTLOADFLAG:0x800` is added to
   `syncwingetlink.vcxproj`'s own link settings, not the core or tests project, since
   it is a property of the shipped binary. Both are cheap defense-in-depth ahead of the
   M8 unsigned single-exe release, not a fix for a demonstrated vulnerability - most of
   this process's current imports are already KnownDLLs.
2. **`WingetComSource::Impl` no longer constructs its own `ComApartment`.** Both this
   class's own header comment and `core/ComApartment.h`'s class comment have carried a
   forward note since M2 (`docs/adr-phase-2.md` ADR-0009) that it owned one only
   because `main.cpp` did not exist yet. Now that `main.cpp` constructs the single
   process-wide apartment before any core call - including the one that constructs a
   `WingetComSource` - the nested one was redundant. `rules/RuleSet.cpp`'s `parse()`
   keeps constructing its own independently: its need for an initialized apartment
   (`winrt::Windows::Data::Json`, ADR-0011) applies even to a `--source fs` or
   `test-rule` invocation that never touches `WingetComSource` at all, so it was never
   solely a stand-in for the process-wide one the way `WingetComSource`'s was.
3. **`cli::run(const std::vector<std::wstring>&)` is the single testable entry point**,
   deliberately decoupled from `wmain`'s exact signature (mirroring `cli::parseArguments`'s
   own `std::vector<std::wstring>` choice, ADR-0020 point 1). It parses argv, handles
   `--help`/`--version` immediately, then dispatches `scan`/`fix`/`test-rule`, catching
   every exception type its own dependencies can throw
   (`ArgParseError`/`PackageSourceError`/`RuleSetError`/`SymlinkServiceError`/
   `LinkInspectionError`, and a generic `std::exception` fallback for anything else,
   such as a `std::filesystem::filesystem_error`) and mapping each to the documented
   exit code. `main.cpp`'s own `try`/`catch` is a defensive backstop for anything that
   still escapes despite that contract, not a normal path.
4. **`exitCodeFor()` is three separate, exhaustive `switch` statements with no
   `default` case** - one per error-kind enum (`PackageSourceErrorKind`,
   `RuleSetErrorKind`, `SymlinkServiceErrorKind`). Omitting `default` is deliberate: if
   a future change adds a new enumerator to any of these three enums without updating
   the matching `exitCodeFor()` overload, the switch no longer covers every case and
   `/W4 /WX` turns the resulting "not all control paths return a value" (C4715)
   diagnostic into a build failure - the totality this ADR documents is
   self-enforcing, not just asserted in a comment or a test that could go stale.
5. **Every `PackageSourceErrorKind` maps to exit code 4, every `RuleSetErrorKind` maps
   to exit code 3, and `SymlinkServiceErrorKind::InsufficientPermission` maps to exit
   code 2** while its three sibling kinds (`DeleteFailed`/`CreateFailed`/
   `VerificationFailed`) map to exit code 10 - a single failing item in an otherwise
   successful batch is the "some repairs failed" case the Wiki page's exit-code table
   already documents, not the permission case.
6. **`scan` builds one inventory (`buildRepairCandidates()`) that `fix` reuses
   unchanged**: enumerate via `createPackageSource(options, onDegrade)`, filter via
   `PackageFilter`, select rules via `selectRuleSet()`, resolve each executable's alias
   via `resolveAlias()`, and inspect each resulting `Links\<alias>.exe` via
   `inspectLink()`. `detectAliasCollisions()` runs once over the full inventory;
   `scan` reports every item (including a colliding alias's own status) plus the
   collision list separately, while `fix` filters colliding aliases out of its own
   repair set entirely before ever calling `repairLink()` - the security contract's
   collision-exclusion rule, enforced at exactly one point common to both commands
   rather than duplicated in each.
7. **A declined confirmation is executed as a `DryRun`, not skipped.** When the user is
   prompted (only for a pre-fetched `Missing`/`Broken` status, under the interactive
   default - `--dry-run` and `--yes` both skip prompting entirely) and declines,
   `fix` still calls `repairLink()` for that candidate, but in `RepairMode::DryRun`
   rather than `Execute`. This reuses `SymlinkService`'s existing
   `WouldCreate`/`WouldReplaceBroken` outcome vocabulary to represent "what would have
   happened, had the user agreed" instead of inventing a fifth, dispatch-only "declined"
   outcome that every consumer (console text, JSON schema) would then need to know
   about separately.
8. **Ctrl+C is handled with `SetConsoleCtrlHandler` setting a process-wide
   `std::atomic<bool>`, checked once per loop iteration in `runFix()`, never mid-item.**
   The handler returns `TRUE` for `CTRL_C_EVENT`/`CTRL_BREAK_EVENT`, suppressing the
   default immediate-termination behavior so the main thread's own loop can stop
   cooperatively at its next check instead - an in-flight `repairLink()` call is never
   interrupted, matching the Wiki page's "not cancellable mid-operation" note. An
   interrupted batch's exit code is 10 (`PartialFailure`), the same code an
   otherwise-complete batch with a failed item uses, since both describe "did not
   finish everything it set out to do."
9. **`test-rule`, `--help`, and `--version` have minimal but real, functional output in
   this issue** - `test-rule` prints "`<name> -> rule "<rule>" -> <alias>`" (or the
   no-match/invalid-fallback cases), `--help` prints the option/exit-code summary, and
   `--version` prints a hardcoded `"syncwingetlink 0.1.0"`. None of these is final: #40
   (M3) owns `test-rule`'s actual presentation design, and #57 owns `--help`/
   `--version`'s final polish and a non-duplicated version source. This issue only
   needed each `AppCommand` value to be handled by *something*, since an unhandled one
   would leave those commands non-functional rather than merely unpolished.

### Reason

- A single shared `buildRepairCandidates()` was chosen over separately duplicating the
  enumerate/filter/resolve/inspect sequence in `runScan()` and `runFix()` because the
  two commands must see identical link-state observations for the same invocation -
  any divergence between two copies of this logic would be a correctness bug (a
  candidate `fix` considers repairing must be the same one `scan` would have reported).
- Reusing `RepairMode::DryRun` for a declined confirmation, rather than adding a new
  outcome, keeps `SymlinkRepairOutcome` and the `--json` schema (`docs/PLAN.md`,
  ADR-0022) exactly as already documented - a JSON consumer parsing `fix` output does
  not need a schema update to understand what a declined item's outcome means.
- Exit code 10 for both an interrupted batch and a batch with a failed item reflects
  that a caller checking the exit code cares about "did everything I asked for
  happen," not the specific reason it did not - the per-item console/JSON output
  already carries the more specific reason.

### A bug found and fixed during this issue's own manual verification

While hand-testing the built `syncwingetlink.exe` (see Consequences below),
`--help`'s output rendered as one unreadable line instead of the intended multi-line
text. The cause: `Console::writeLine()` sanitizes its argument via
`sanitizeForDisplay()` (ADR-0021), which strips every C0 control character -
including `\n` - by design, so that an untrusted package id or alias can never forge
an extra output line. `printHelp()`'s original implementation passed one large string
literal containing embedded `\n` characters to a single `writeLine()` call, and
`sanitizeForDisplay()` correctly (per its own contract) stripped every one of them.
The fix was in `printHelp()`, not in `sanitizeForDisplay()`: the help text is now an
array of individual lines, each written through its own `writeLine()` call.
`sanitizeForDisplay()`'s behavior is unchanged and remains correctly documented and
tested by #54's `ConsoleTests.cpp`.

### Consequences

- `src/main.cpp` (new, in the executable project only) and `src/cli/Dispatch.{h,cpp}`
  (new, in `syncwingetlink.core`) implement the above.
- `src/core/WingetComSource.cpp`'s `Impl` no longer has a `ComApartment` member;
  `src/core/ComApartment.h`'s class comment is updated to match the new ownership.
- `src/syncwingetlink.vcxproj`/`.filters` register `main.cpp` and add the
  `/DEPENDENTLOADFLAG:0x800` link option.
- **`syncwingetlink.exe` links successfully for the first time** in this project's
  history - every prior M0-M5 build produced only `LNK1561` ("an entry point must be
  defined") for the executable project, which every prior issue's `task.md` entry
  documented as a known, pre-existing condition. That condition is now resolved in all
  four configurations (`Debug|Release` × `x64|ARM64`).
- `tests/DispatchTests.cpp` covers `exitCodeFor()`'s totality over all three error-kind
  enums. The rest of `cli::run()`'s behavior - argument parsing through to exit code,
  across `scan`/`fix`/`test-rule`/`--help`/`--version`, `--json`, `--dry-run`,
  confirmation accept/decline/EOF, `InsufficientPermission` guidance text, and alias
  collision exclusion - was verified by hand against a real build, driven with
  `--source fs` and `--packages-dir`/`--links-dir` pointed at a scratch directory tree
  (not against the real, shared `%LOCALAPPDATA%\Microsoft\WinGet` paths). This is not
  covered by an automated test: unlike `core/SymlinkService`'s injectable
  `SymlinkServiceOperations` seam, `cli::Dispatch` does not mock out package
  enumeration, alias resolution, or console I/O, so exercising the full pipeline
  deterministically would need infrastructure this issue does not add. The manual
  transcript is recorded in `docs/task.md`'s entry for this issue.
- #40 (M3) and #57 (M6) each still have real work: `test-rule`'s presentation and
  `--help`/`--version`'s final polish/single version source, respectively - both
  build on top of the minimal, functional versions this issue ships rather than
  leaving those commands unhandled.

---

## ADR-0025 — A single version constant, and help-text polish

- **Date**: 2026-07-30
- **Affected**: `cli/Dispatch`, `docs/TODO.md` M6, the Wiki page
  `plan/syncwingetlink/m6-command-line-interface`
- **Status**: Accepted

### Decision

1. **`cli::Version.h` defines exactly one constant, `kVersion`**, and `printVersion()`
   is the only place that reads it. #56 had shipped `--version`'s output as a bare
   string literal directly inside `printVersion()` - functionally correct, but exactly
   the kind of "second hardcoded literal" this issue's acceptance criteria called out.
   Extracting it to a named, header-exposed constant means any future caller that
   needs the version string (a `--json` output field, `test-rule`'s eventual output,
   a log line) has one place to read it from rather than a temptation to copy the
   literal again.
2. **`kVersion` is *not* build-time-unified with `src/app.manifest`'s
   `assemblyIdentity` version attribute.** Achieving that would require either a
   custom pre-build step that rewrites the manifest from an MSBuild property, or a
   compiled `VERSIONINFO` resource queried at runtime via `GetFileVersionInfoW` -
   both real, valid designs, but new build-system infrastructure this project does not
   have yet and that a version-string display feature does not, by itself, justify
   adding. The two values are kept in sync by convention (a version bump updates both
   by hand), stated plainly in `cli/Version.h`'s own comment rather than left as an
   implicit assumption. A future M8 release-tooling issue is the natural place to
   revisit this if the project ever wants the stronger guarantee.
3. **`--help`'s text was expanded, not restructured**: a one-line description of what
   the tool does, `--help`/`-h` and `--version` documented explicitly as their own
   option-table rows (previously merged onto one line, ambiguous about which flags
   were aliases of which), `NO_COLOR` mentioned alongside `--no-color`, and the
   exit-code section reformatted as one code per line for readability. The
   line-per-`writeLine()` structure #56 already established (to avoid
   `sanitizeForDisplay()` stripping embedded newlines - see that issue's ADR-0024
   entry) is unchanged.

### Reason

- A named constant costs nothing and removes the one duplication risk explicitly
  named in this issue's scope; a full build-time version-unification system would cost
  real effort for a benefit (guaranteeing `app.manifest` and `--version` never drift)
  this project has not yet needed enough to justify.

### Consequences

- `src/cli/Version.h` (new) defines `kVersion`; `src/cli/Dispatch.cpp`'s
  `printVersion()` reads it instead of a bare literal.
- `src/cli/Dispatch.cpp`'s `printHelp()` text is expanded per point 3 above.
- `tests/DispatchTests.cpp` gains `VersionTests`, pinning `kVersion` to the documented
  first-release value `"0.1.0"` and asserting it is non-empty - regression coverage
  for the single-source-of-truth property, since `printVersion()` itself is not
  exported and was instead re-verified by hand (`docs/task.md`'s entry for this
  issue).
- `docs/TODO.md` M6's `--help`/`--version` line is checked off, pointing at #57 and
  this ADR. **M6 is now complete**: every item in `docs/TODO.md`'s M6 section is
  checked off, and `syncwingetlink.exe` builds, links, and runs end-to-end across
  `scan`/`fix`/`test-rule`/`--help`/`--version` (ADR-0020 through ADR-0025).
