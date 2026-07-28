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
