# Architecture Decision Records — M4 Phase

This file continues the chronological record in [`adr-phase-2.md`](./adr-phase-2.md).

---

## ADR-0014 — Link classification contract, invalid-observation handling, and the M5 re-inspection rule

- **Date**: 2026-07-27
- **Affected**: `core/Model.h`, `core/LinkInspector`, `docs/PLAN.md` §6, `docs/TODO.md` M4,
  `docs/TODO.md` M5
- **Status**: Accepted

### Decision

1. **Classification contract.** `classifyLink()` (a free function in the
   `syncwingetlink` namespace, declared in `core/LinkInspector.h` - there is no
   `LinkInspector` class) implements exactly the six-row table the M4 Wiki plan and
   issue #44 specify:

   | Observation | Status | Existing target |
   |---|---|---|
   | No entry | `Missing` | None |
   | Regular file | `Mismatch` | None |
   | Reparse point that is not a symbolic link | `Mismatch` | None |
   | Symbolic link whose target is absent | `Broken` | Decoded target |
   | Symbolic link that resolves to the expected file | `Ok` | Decoded target |
   | Symbolic link that resolves to another existing file | `Mismatch` | Decoded target |

   `Broken` is limited to a symbolic link whose decoded target does not currently exist;
   an existing but different target is `Mismatch`, not `Broken`. `docs/PLAN.md` §6 still
   describes `Broken` as covering both cases - that sentence is corrected in #48, once the
   full inspector (not just the classification contract) exists to point the corrected
   text at.
2. **`classifyLink()` is pure and filesystem-independent.** It takes a `LinkObservation`
   (the entry kind, an optional decoded target, and a `TargetRelation`) plus the
   already-known `PackageExe`/alias/link path, and returns a complete `RepairItem`. It
   performs no I/O, so every row above - and every invalid combination below - is a
   deterministic unit test with no filesystem fixture or symlink privilege.
3. **An invalid `LinkObservation` throws `std::invalid_argument`, not
   `LinkInspectionError`.** The two error paths are deliberately different:
   - `LinkInspectionError` (added in this issue, populated starting in #45/#46) carries
     the failed Win32 operation, path, and `GetLastError()` code. It represents a real
     operation that was attempted against the filesystem and failed - access denial, a
     sharing violation, malformed reparse data, or an expected package executable that
     disappears mid-inspection.
   - An invalid observation (e.g. a decoded target reported for a regular file, or a
     symbolic link left at `TargetRelation::NotApplicable`) is not something a
     Win32 call failed to do - no operation was attempted at all. It is the *caller*
     violating `classifyLink()`'s precondition, which is a programming-contract
     violation. Forcing it through `LinkInspectionError` would require inventing a fake
     Win32 error code for an error Win32 never raised, which is worse than a distinct,
     honest exception type.
4. **M5 must re-inspect an entry immediately before any mutation.** `classifyLink()`
   reflects a single point-in-time observation; nothing in M4 keeps that observation
   valid after the caller returns. `docs/TODO.md` M5 (symlink creation) must call the
   Win32 probe again immediately before deleting or replacing an entry, rather than
   acting on a `RepairItem` computed earlier in the same run.

### Reason

- Issue #44's review comments asked explicitly that invalid observation combinations
  "fail explicitly rather than being normalized" - silently coercing an invalid
  combination to some default status would hide a bug in whatever produces
  `LinkObservation` values (the Win32 adapter added in #45/#46).
- Keeping `classifyLink()` free of any Win32 or filesystem dependency is what lets M4's
  core classification logic be tested exhaustively (all six table rows, plus every
  invalid combination) without Developer Mode, symlink privilege, or a temp-directory
  fixture - the same reasoning that already applies to `AliasResolver` and `RuleSet`.
- The re-inspection requirement is called out here, in the model's own ADR, rather than
  left implicit until M5 is implemented, since a `RepairItem` computed during `scan` and
  a `RepairItem` acted upon during `fix` can legitimately describe different points in
  time (another process could touch `Links` in between).

### Consequences

- `core/Model.h` gains `LinkEntryKind { None, RegularFile, SymbolicLink,
  OtherReparsePoint }` and a `RepairItem::entryKind` field recording which of the four
  was observed, independent of the derived `LinkStatus`.
- `core/LinkInspector.h` declares `TargetRelation`, `LinkObservation`,
  `LinkInspectionError`, and `classifyLink()`. The Win32 probe that produces a real
  `LinkObservation` (`inspectLink()`), the reparse-buffer decoder it depends on, and
  `detectAliasCollisions()` are added in #45, #46, and #47 respectively - this entry
  covers only the model and the pure classifier.
- `docs/PLAN.md` §6's conflicting `Broken` sentence, and `docs/TODO.md` M4's checklist,
  are corrected/completed in #48 once the full milestone is implemented, not piecemeal
  across each sub-issue.
