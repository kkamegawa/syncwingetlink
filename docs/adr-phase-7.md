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

`Debug|x64` and `Release|x64` build clean at `/W4 /WX`; `vstest.console.exe` reports all
tests passing for both, including the new test (executed for real where Developer Mode
or elevation is available in the environment; otherwise it logs and returns per the
project's existing `Inconclusive`-style skip convention — see the test itself).
`Release|ARM64` cross-builds clean (cross-built, not run).
