# Architecture Decision Records — Windows API audit fixes

This file continues the chronological record in
[`adr-phase-6.md`](./adr-phase-6.md). It covers the three fixes tracked by issue #129
and its sub-issues (#130, #131, #132), which followed from a Copilot-assisted audit of
Win32 API usage across `core/`, `cli/`, and `tui/` (originally written up in draft PR
#128, which deferred implementation).

---

## ADR-0034 — Directory reparse points under `Links\` are classified `Mismatch` without decoding

- **Date**: 2026-08-01
- **Affected**: `src/core/LinkInspector.cpp`, `src/core/LinkInspector.h`, issue #130
- **Status**: Accepted

### Decision

`inspectLink()` classifies any reparse point carrying `FILE_ATTRIBUTE_DIRECTORY` as
`LinkEntryKind::OtherReparsePoint` (→ `LinkStatus::Mismatch` per the existing
`classifyLink()` contract, ADR-0014) **without decoding it as a symbolic link**, checked
immediately after `GetFileAttributesW` confirms the entry is a reparse point and before
`readSymbolicLinkTarget()` is called.

### Rationale

`Links\<alias>.exe` is only ever expected to hold a *file* symbolic link — the only kind
`SymlinkService::repairLink()` creates (`CreateSymbolicLinkW` without
`SYMBOLIC_LINK_FLAG_DIRECTORY`). Before this change, `inspectLink()` did not distinguish
a directory reparse point from a file one: both were decoded via
`readSymbolicLinkTarget()`. A directory symbolic link with a missing target therefore
decoded successfully and was classified `Broken` — and repairing `Broken` calls
`DeleteFileW`, which cannot remove a directory entry (`RemoveDirectoryW` is required).
The resulting `ERROR_ACCESS_DENIED` was then surfaced as
`SymlinkServiceErrorKind::InsufficientPermission`, printing Developer Mode guidance for
what was actually "wrong API for this entry kind," not a permission problem.

Classifying by `FILE_ATTRIBUTE_DIRECTORY` before attempting to decode avoids ever
reaching that misdiagnosis: `Mismatch` is never touched by `repairLink()` (ADR-0014's
existing contract), so a directory reparse point under `Links\` is left alone regardless
of whether its target exists.

### Consequences

- `LinkInspector.h`'s `inspectLink()` contract comment gains a bullet documenting this
  case.
- `tests/TempDirectory.h` gains `createDirectorySymlink()`, mirroring the existing
  `createFileSymlink()` helper and its Developer-Mode-or-elevation skip convention.
- `tests/LinkInspectorTests.cpp` gains
  `directorySymbolicLinkIsMismatchEvenWithAMissingTarget`, covering the specific
  dangerous shape (missing target) that would otherwise have decoded to `Broken`.
- No change to `SymlinkService` or its delete path was necessary — the fix is entirely in
  classification, before any repair decision is made.

### Verification

`Debug|x64` and `Release|x64` build clean at `/W4 /WX`; `vstest.console.exe` reports
405/406 passing for both, including the new test (executed for real where Developer Mode
or elevation is available in the environment; otherwise it logs and returns per the
project's existing `Inconclusive`-style skip convention — see the test itself). The one
failure, `nonAsciiBrokenSymbolicLinkIsBroken`, is pre-existing and unrelated — confirmed
by running the same test against an unmodified `main` worktree, where it fails
identically (see `docs/task.md`'s entry for this issue). `Release|ARM64` cross-builds
clean (cross-built, not run).

---

## ADR-0035 — `SymlinkService::deleteEntry` deletes by handle, closing the inspect-to-delete TOCTOU race

- **Date**: 2026-08-01
- **Affected**: `src/core/SymlinkService.cpp`, `src/core/SymlinkService.h`, issue #131
- **Status**: Accepted

### Decision

The production `SymlinkServiceOperations::deleteEntry` (built by
`makeProductionOperations()`) no longer deletes by name with a single `DeleteFileW(
linkPath)` call. It now:

1. Opens `linkPath` once via `CreateFileW(..., DELETE | FILE_READ_ATTRIBUTES, ...,
   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, ...)`. `FILE_READ_ATTRIBUTES`
   is required for step 2's `GetFileInformationByHandleEx` call to succeed - without it,
   that call could itself fail with `ERROR_ACCESS_DENIED` and be misdiagnosed as
   `InsufficientPermission` by `buildError()`, blaming Developer Mode/elevation for what
   would actually be an access-rights gap in this open call.
   `FILE_FLAG_BACKUP_SEMANTICS` is required to open a directory at all via `CreateFileW`;
   without it, a raced replacement of `linkPath` by a plain directory would fail the open
   itself with a non-deterministic error rather than being caught deterministically by
   step 2's re-verification (caught during Copilot review of the PR implementing this
   ADR).
2. Re-verifies, via `GetFileInformationByHandleEx(FileAttributeTagInfo)` on that same
   handle, that the entry is still a file symbolic link (`IO_REPARSE_TAG_SYMLINK`, not
   `FILE_ATTRIBUTE_DIRECTORY`). A mismatch returns `ERROR_INVALID_DATA` without deleting
   anything.
3. Deletes by handle via `SetFileInformationByHandle(FileDispositionInfo)`, never by a
   second by-name call.

`SymlinkServiceError::operation()` reports `"deleteEntry"` for a delete failure, replacing
the literal `"DeleteFileW"` — the failing operation is no longer that single Win32 call.
This is a breaking change to a documented, publicly-visible string on `SymlinkService.h`'s
error contract (`tests/SymlinkServiceTests.cpp`'s
`deleteFailurePreventsCreationAndReportsDeleteFailed` asserted the old literal and is
updated in the same commit).

### Rationale

`repairLink()` only reaches `deleteEntry` after its own fresh `inspectLink()` call
classifies the entry `Broken` with `LinkEntryKind::SymbolicLink` (enforced by
`repairLink()`'s own invariant check — a `Broken` status paired with any other entry kind
is rejected as a programming-contract violation before any Win32 call is attempted). The
previous implementation then deleted **by name**: nothing prevented the filesystem entry
at `linkPath` from being replaced between that inspection and the `DeleteFileW` call — a
racing process could delete-and-recreate it, or replace it with an unrelated file or
directory, and `DeleteFileW` would delete whatever now occupies that path, not the
specific object `inspectLink()` actually classified.

Opening the entry once and operating on the resulting handle for both the re-verification
and the delete closes this window: once the handle is open, the object it refers to
cannot be silently swapped out from under the delete the way a second path-based call
could be. The re-verification step additionally guards against a delete succeeding on an
object that changed shape (e.g. into a directory) between inspection and this call, even
though PR/issue #130 (ADR-0034) already prevents a directory reparse point from ever
reaching `Broken` status via `inspectLink()` in the first place — the re-verification is
defense in depth against a race inspectLink() itself cannot observe, not a duplicate of
ADR-0034's classification fix.

### Consequences

- `SymlinkService.h` gains updated contract comments on `SymlinkServiceErrorKind`,
  `SymlinkServiceError::operation()`, `SymlinkServiceOperations::deleteEntry`, and
  `repairLink()`'s own `Throws` documentation — all previously named `"DeleteFileW"`
  literally.
- `tests/SymlinkServiceTests.cpp` gains
  `deleteReVerificationFailureIsReportedAsDeleteFailedWithoutCreation`, and the existing
  `deleteFailurePreventsCreationAndReportsDeleteFailed` is updated for the new operation
  label. The real handle-based delete path is already exercised end-to-end (not just
  mocked) by the pre-existing `productionRepairLinkReplacesARealBrokenLink`, which drives
  the two-argument `repairLink()` overload through `kProductionOperations` — no new
  production-facing integration test was needed to cover the new code path, only the new
  error-classification branch, which `FakeOperations` covers directly. A test that
  reproduces the actual race (a second thread or process mutating the filesystem entry
  between `inspectLink()` and `deleteEntry`) was not attempted — it would be inherently
  timing-dependent and is not needed to verify the fix's logic, which is deterministic
  given any observed mismatch.
- No change to `LinkInspector` or `classifyLink()` was necessary — this fix is entirely in
  the delete implementation, downstream of classification.

### Verification

`Debug|x64` and `Release|x64` build clean at `/W4 /WX`; `vstest.console.exe` reports
406/407 passing for both — one more total test than ADR-0034's 405/406, since this layer
adds one new test — with the same pre-existing, unrelated `nonAsciiBrokenSymbolicLinkIsBroken`
failure and no others. `productionRepairLinkReplacesARealBrokenLink` — which exercises
the new handle-based delete against a real broken symlink — passed on this host (symlink
privilege was available). `Release|ARM64` cross-builds clean (cross-built, not run).
