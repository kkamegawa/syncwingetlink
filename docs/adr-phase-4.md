# Architecture Decision Records — M5 Phase

This file continues the chronological record in [`adr-phase-3.md`](./adr-phase-3.md).

---

## ADR-0018 — SymlinkService state machine, mismatch policy, and the re-inspection/no-rollback rules

- **Date**: 2026-07-29
- **Affected**: `core/SymlinkService`, `docs/PLAN.md` §5, `docs/TODO.md` M5, the Wiki page
  `plan/syncwingetlink/m5-symlink-repair-service`
- **Status**: Accepted

### Decision

1. **`repairLink()` is a single, cohesive state machine covering all four `LinkStatus`
   values in both `RepairMode::Execute` and `RepairMode::DryRun`**, added in full in issue
   #49 rather than split across sub-issues by state. The Wiki plan's delivery sequence
   describes #49 as "missing-link creation" and #50 as "adds the delete-then-create path";
   in practice the two paths share one function and nearly all of their code (both end in
   the same create-then-verify steps), so implementing only the `Missing` branch in #49
   and leaving `Broken` unhandled would either throw a placeholder "not implemented" error
   from a fresh production entry point or duplicate the shared creation/verification logic
   — both worse than shipping the complete, fully-tested state machine at once. #50
   instead adds the *dedicated* stale-candidate and delete-failure regression coverage the
   Wiki's test plan calls for; #51 layers permission diagnosis on top; #52 completes
   dry-run completeness proof and documentation. This is a deliberate scope adjustment
   from the Wiki page's literal per-issue wording, not a contradiction of its design — the
   state table, error model, and public interface it specifies are implemented exactly as
   written.
2. **Fresh-state → outcome mapping** (identical for `Execute` and `DryRun`, differing only
   in whether mutation actually happens):

   | Fresh `LinkStatus` | `Execute` | `DryRun` |
   |---|---|---|
   | `Missing` | create, then verify → `Created` | `WouldCreate` |
   | `Broken` (`entryKind == SymbolicLink`) | delete, create, then verify → `ReplacedBroken` | `WouldReplaceBroken` |
   | `Ok` | `SkippedOk`, no mutation | `SkippedOk` |
   | `Mismatch` | `RefusedMismatch`, no mutation | `RefusedMismatch` |

   `Broken` paired with any `entryKind` other than `SymbolicLink` is impossible from a
   correct `inspectLink()` result (ADR-0014's classification table only produces `Broken`
   for a symbolic link with a missing target) and is therefore treated as a caller
   contract violation - `repairLink()` throws `std::invalid_argument`, mirroring
   `classifyLink()`'s own invalid-observation handling rather than inventing a `Win32`
   error code for something no Win32 call produced.
3. **`repairLink()` always re-inspects immediately before deciding anything**, using the
   candidate's executable/alias/link path but never its stored `status`/`entryKind`. A
   `RepairItem` computed during an earlier `scan` can describe a different point in time
   than the moment `fix` actually runs; only the fresh inspection governs. This closes the
   race the M4 ADR-0014 flagged as a known follow-up.
4. **No rollback is synthesized when a broken link's deletion succeeds but the subsequent
   creation fails.** The entry was already broken (its target did not exist); recreating
   it would require the same `CreateSymbolicLinkW` call that just failed, so a later retry
   is the only recovery path. The failure is reported as-is via
   `SymlinkServiceError(CreateFailed)`.
5. **A successful `Created`/`ReplacedBroken` outcome requires a second, post-create
   `inspectLink()` call reporting `Ok`.** A creation that "succeeds" at the Win32 level but
   is not confirmed `Ok` on re-inspection throws `SymlinkServiceError(VerificationFailed)`
   rather than being reported as success to the caller.
6. **Permission classification ships in #51, not #49.** `DeleteFailed`/`CreateFailed`
   failures in #49 are reported generically; `SymlinkServiceOperations::queryDeveloperMode`
   /`queryElevation` exist as part of the public seam from #49 onward but always report
   `Unknown` in production until #51 adds the real Developer Mode registry query and
   process-token elevation query. `ERROR_ACCESS_DENIED`/`ERROR_PRIVILEGE_NOT_HELD` are
   already classified as `InsufficientPermission` starting in #49 (the classification
   logic itself is a small, self-contained piece of the same error-handling code path),
   carrying whatever `queryDeveloperMode`/`queryElevation` currently report.

### Reason

- Splitting one cohesive create/verify code path across two PRs by fresh-state would force
  #49 to ship either dead/duplicated code or an intentionally-broken production entry
  point for `Broken` candidates, which conflicts with this project's practice of not
  shipping half-finished logic (see ADR-0014's own insistence on failing explicitly rather
  than normalizing). Reassigning #50's contribution to dedicated hardening tests instead
  keeps every sub-issue's PR reviewable and substantial without that cost.
- The re-inspection rule was already anticipated in ADR-0014 (M4) specifically to avoid
  guessing here; this ADR is where it is actually implemented.
- The no-rollback decision avoids masking a delete-then-recreate failure behind a second,
  equally likely to fail, synthetic recovery attempt - reporting the true failure is more
  actionable than hiding it behind a rollback that cannot itself be guaranteed to succeed.

### Consequences

- `core/SymlinkService.h` gains `RepairMode`, `SymlinkRepairOutcome`,
  `SymlinkRepairResult`, `DeveloperModeState`, `ElevationState`, `SymlinkServiceErrorKind`,
  `SymlinkServiceError`, `SymlinkServiceOperations`, and the two `repairLink()` overloads.
- `tests/SymlinkServiceTests.cpp` covers every table row and mode, input validation, the
  Broken-with-inconsistent-entry-kind contract violation, delete/create/verification
  failure propagation, the creation contract (target/link path order,
  `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`), the stale-candidate re-inspection rule,
  and a filesystem-backed `Missing → Created` test that logs and skips gracefully without
  symlink privilege, per the ADR-0016 precedent.
- #50 adds dedicated stale-candidate and delete-failure regression depth for the `Broken`
  path (already implemented here) plus a filesystem-backed broken-link replacement test.
- #51 adds the real Developer Mode/elevation queries and their deterministic test matrix,
  replacing the `Unknown`-only placeholders this ADR describes.
- #52 proves `DryRun` invokes zero mutation/permission-query callbacks across all four
  states and finalizes `docs/PLAN.md`/`docs/TODO.md`/`docs/task.md` for M5.

---

## ADR-0019 — Developer Mode and elevation queries: registry/token source, and the exit-code boundary M5 does not own

- **Date**: 2026-07-29
- **Affected**: `core/SymlinkService`, `docs/TODO.md` M6 (forward reference only)
- **Status**: Accepted

### Decision

1. **Developer Mode** is read from
   `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock`, value
   `AllowDevelopmentWithoutDevLicense`, via `RegGetValueW`. Value `1` is `Enabled`; any
   other successfully-read value is `Disabled`; **any** read failure - including the
   ordinary case of the key or value never having been created, which is exactly what an
   unmodified installation looks like - is `Unknown`, never `Disabled`. Confirmed against
   this development host, where the key does not exist and the query correctly reports
   `Unknown` (see `tests/SymlinkServiceTests.cpp`'s
   `productionPermissionQueriesReflectRealHostStateOnAGenuineFailure`, which logs this
   rather than asserting a fixed value for exactly that reason).
2. **Elevation** is read via `OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, ...)` +
   `GetTokenInformation(TokenElevation, ...)`. Unlike the registry read, this has no
   legitimate reason to fail for the current process, so the same test asserts it is never
   `Unknown` in practice, while still coding the `Unknown` fallback for defensiveness if
   either Win32 call ever does fail.
3. **Both queries are invoked only from `buildError()`**, at the moment a
   `DeleteFileW`/`CreateSymbolicLinkW` failure is already known to be
   `ERROR_ACCESS_DENIED` or `ERROR_PRIVILEGE_NOT_HELD` - never speculatively, and never to
   pre-empt or explain a failure that turns out to be something else.
4. **M5 still returns no process exit code.** This ADR documents the query
   implementation only. The mapping from `SymlinkServiceErrorKind::InsufficientPermission`
   to exit code 2, other per-item failures to the partial-failure exit code 10, and the
   four `(elevation, developerMode)` guidance messages the Wiki plan lists remain M6's
   responsibility, consuming `SymlinkServiceError::developerModeState()`/`elevationState()`
   as-is.

### Reason

- The registry key location is Microsoft's documented mechanism for Developer Mode and is
  the same setting the Settings app's toggle writes; no undocumented or reverse-engineered
  state is relied upon.
- Treating an absent registry value as `Unknown` rather than `Disabled` matters
  operationally: M6's guidance text for "non-elevated + Developer Mode disabled" (enable
  Developer Mode or run elevated) differs from "non-elevated + Developer Mode unknown"
  (check Developer Mode or run elevated) per the Wiki plan - collapsing "never configured"
  into "confirmed off" would overstate what was actually observed.
- Deriving the classification logic as two small, injectable-independent pure functions
  (`classifyDeveloperMode`/`classifyElevation` in `SymlinkService.cpp`'s anonymous
  namespace) keeps the Win32-calling wrappers thin, though the primary test coverage for
  the classification itself runs through the `SymlinkServiceOperations` seam (the
  3x3 matrix in `tests/SymlinkServiceTests.cpp`) rather than by exposing these two
  functions directly - consistent with the seam being the one documented, public way to
  test `SymlinkService` deterministically.

### Consequences

- `core/SymlinkService.cpp`'s `makeProductionOperations()` now wires
  `queryDeveloperModeFromRegistry` and `queryElevationFromProcessToken` in place of the
  `Unknown`-only placeholders #49 shipped.
- `tests/SymlinkServiceTests.cpp` gains the full 3x3 `DeveloperModeState` x
  `ElevationState` matrix (via the operations seam), confirmation that unrelated failures
  never query permission state at all, and one filesystem-backed test exercising the real
  registry/token code paths against this host's actual (non-elevated, no Developer Mode)
  state.
- M6, when it exists, is responsible for the exit-code mapping and guidance text; this ADR
  only guarantees the state values it will consume are accurate and honestly `Unknown`
  where genuinely unknown.
