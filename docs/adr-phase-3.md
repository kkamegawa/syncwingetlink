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

---

## ADR-0015 — Hand-rolled symbolic-link reparse parsing, its error/nullopt split, and NT-namespace decoding

- **Date**: 2026-07-27
- **Affected**: `core/LinkInspector`
- **Status**: Accepted

### Decision

1. **The symbolic-link `REPARSE_DATA_BUFFER` layout is hand-rolled, not included from a
   header.** `<Windows.h>`/`<winioctl.h>` declare `FSCTL_GET_REPARSE_POINT` and
   `IO_REPARSE_TAG_SYMLINK`, but the struct describing what that control code returns for
   a symbolic link lives only in the DDK-only `ntifs.h`. `core/LinkInspector.cpp` reads
   the buffer as plain byte offsets (documented publicly in [MS-FSCC] 2.1.2.4) instead of
   overlaying a struct, mirroring `tests/TempDirectory.h`'s existing
   `MountPointReparseBuffer` for the reverse (write) direction. `SYMLINK_FLAG_RELATIVE`
   (`0x1`) and `MAXIMUM_REPARSE_DATA_BUFFER_SIZE` (16 KiB) are likewise not in any public
   SDK header and are defined locally with the same citation.
2. **`decodeSymbolicLinkTarget()` is a pure function operating on `std::span<const
   std::byte>`, separate from the Win32 I/O that produces that buffer.**
   `readSymbolicLinkTarget()` performs the actual `CreateFileW` +
   `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` call and hands the result to the pure
   decoder. This split is what lets every malformed-buffer case (truncated header, an
   inconsistent `ReparseDataLength`, a misaligned or oversized substitute-name
   offset/length) be a deterministic unit test that constructs the bytes directly,
   without creating a real reparse point or holding symlink privilege.
3. **An unrecognized reparse tag is `std::nullopt`, not an error - for both functions.**
   `decodeSymbolicLinkTarget()` returns `nullopt` the moment `ReparseTag` is not
   `IO_REPARSE_TAG_SYMLINK`, without validating anything else in the buffer (a different
   tag's data has a different, unvalidated shape). `readSymbolicLinkTarget()` extends the
   same `nullopt` outcome to `ERROR_NOT_A_REPARSE_POINT` (an ordinary file). Only a
   buffer whose tag **is** `IO_REPARSE_TAG_SYMLINK` but is otherwise malformed throws
   `LinkInspectionError` with `win32ErrorCode() == ERROR_INVALID_DATA` - the closest
   existing Win32 error code for "the data is invalid," used even though no real Win32
   call actually returned it, since no more specific code exists for a content-level
   validation failure.
4. **NT-namespace (`\??\`) decoding is new code, distinct from
   `paths::fromExtendedLengthPath`.** A pre-implementation review of the M4 Wiki plan
   caught this plan defect before #44 started: the existing helper recognizes only the
   Win32 extended-length forms (`\\?\`, `\\?\UNC\`), not the NT device prefix
   (`\??\`, `\??\UNC\...`) an absolute `SubstituteName` uses. `LinkInspector.cpp` adds
   `stripNtNamespacePrefix()` for that prefix specifically, then still runs the result
   through `paths::fromExtendedLengthPath()` afterward in case a target is already
   expressed in extended-length form (observed in practice for some absolute substitute
   names) - the two prefixes are stripped by two different, non-overlapping code paths.
5. **The extended-length prefix is stripped before `lexically_normal()`, not after.**
   `decodeSymbolicLinkTarget()` calls `paths::fromExtendedLengthPath()` first and
   `lexically_normal()` second. `std::filesystem::path`'s normalization behavior for a
   `\\?\`-prefixed root-name is not exercised elsewhere in this codebase, so avoiding the
   question entirely - normalizing only after the prefix is already a plain Win32 form -
   was preferred over relying on unverified standard-library behavior.

### Reason

- Issue #45's acceptance criteria require that "no parser read can exceed the bytes
  returned by `DeviceIoControl`." Keeping the parser pure and buffer-based (rather than
  reading through a `reinterpret_cast` struct overlay) is what makes every offset/length
  check independently verifiable against `reparseBuffer.size()`, including a final
  direct check re-derived from the buffer itself rather than only from intermediate
  derived sizes.
- Distinguishing "not a symbolic link" (`nullopt`) from "malformed symbolic-link data"
  (`LinkInspectionError`) matches the M4 classification contract in ADR-0014: a
  different reparse tag is legitimately `LinkEntryKind::OtherReparsePoint`, not a
  failure, while a `IO_REPARSE_TAG_SYMLINK` buffer that cannot be parsed at all is a
  genuine I/O-layer problem the caller cannot recover a target from.

### Consequences

- `core/LinkInspector.h` gains `decodeSymbolicLinkTarget()` and
  `readSymbolicLinkTarget()`, both returning `std::optional<std::filesystem::path>`.
- `tests/LinkInspectorTests.cpp` gains `ReparseTargetDecoderTests` (buffer-level: drive,
  UNC, relative, already-extended-length, and unrecognized-tag targets, plus every
  malformed-buffer case) and `SymbolicLinkTargetReaderTests` (the two `Win32` outcomes
  reachable without symlink privilege: a nonexistent path, and an ordinary file). Testing
  a real symbolic link's `Ok`/`Broken`/`Mismatch` outcomes is filesystem-backed work
  owned by #46, per the M4 Wiki plan's test-plan split.
- `readSymbolicLinkTarget()` is not yet called from anywhere - `classifyLink()` from #44
  still takes an already-built `LinkObservation`. Wiring `GetFileAttributesW` classification
  and file-identity comparison around these two functions into the production
  `inspectLink()` adapter is #46.
