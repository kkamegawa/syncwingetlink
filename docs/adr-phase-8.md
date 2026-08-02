# Architecture Decision Records — M9 Phase

This file continues the chronological record in
[`adr-phase-7.md`](./adr-phase-7.md). `adr-phase-7.md` covers ADR-0034 – ADR-0036 (the
Windows API audit fixes tracked by issue #129) and is already 223 lines, past the
200-line split threshold `adr.md` records; M9's single ADR starts a new file rather than
extending it further.

---

## ADR-0037 — `docs/com-api.md` documents observed implementation behavior, not design intent

- **Date**: 2026-08-02
- **Affected**: `docs/com-api.md`, `docs/TODO.md` M9, the Wiki page
  `plan/syncwingetlink/m9-com-api-documentation`
- **Status**: Accepted

### Decision

1. **`docs/com-api.md` is rewritten against the current implementation**
   (`src/core/WingetComSource.cpp`, `ComApartment.cpp`, `PackageSourceError.cpp`,
   `PackageSourceFactory.cpp`, `props/syncwingetlink.winget-projection.targets`), not
   against `docs/PLAN.md` §3's original design intent. The two had diverged since M6
   (issue #56) changed apartment ownership from `WingetComSource` to `main.cpp`, and the
   document had not been updated since 2026-07-27.
2. **Every claim in the rewritten document is backed by a named file, or by a live run
   recorded in this ADR's Verification section and marked as such.** Where a claim from
   the previous version of the document (the `packageQuery` capability requirement, the
   `CreateCompositePackageCatalog` option) could not be confirmed from source or from a
   live run, it is corrected to state what is actually known, per `AGENTS.md` §2 rule 5
   ("be honest") and rule 4 ("do not fill gaps by guessing").
3. **The document now covers out-of-proc/in-proc differences explicitly** — the one M9
   checklist sub-topic the previous version only mentioned in passing. This codebase
   activates every COM class with `CLSCTX_LOCAL_SERVER` only, with no in-process
   fallback, and the document now states the consequences that follow from that choice.
4. **A genuinely new, reproducible finding surfaced during verification is documented as
   an observed data point, not generalized into a rule**: on the machine and App
   Installer version this ADR was verified against, `winrt::create_instance<PackageManager>`
   (the exact call `WingetComSource` makes) failed with
   `HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)` (`0x80073D54`), even though the real
   `winget` CLI worked normally and a bare `CoCreateInstance` of the same CLSID
   requesting only `IUnknown` succeeded. See Verification for the full reproduction. This
   ADR does not assert whether this is a permanent constraint of unpackaged out-of-proc
   activation for this specific interface or a version-dependent regression — that
   determination was out of scope for a documentation milestone and is left for a
   follow-up investigation if the project chooses to pursue one.

### Reason

- `docs/TODO.md` M9's checklist item names four sub-topics (activation, capabilities,
  out-of-proc/in-proc differences, fallback behavior); the existing document answered
  the first, third (only in passing), and fourth already, and had already drifted from
  the code answering the first.
- A documentation milestone whose only output is prose is worth the same evidentiary
  standard as a code change: an unverifiable claim in a reference document is
  indistinguishable from a wrong one until someone tries to rely on it.
- The project's own build pins the SDK but deliberately does not pin the winget winmd
  (ADR-0008); a document that asserts fixed behavior without noting the version tested
  against would misrepresent that design choice as more stable than it is.

### Verification

Performed on this machine (Windows 11, VS 2026 / MSBuild 18.8.2, platform toolset v145)
in the `docs/66-com-api-documentation` branch, before any documentation was rewritten:

```
msbuild syncwingetlink.sln -p:Configuration=Release -p:Platform=x64 -m
```

Build succeeded clean (core, exe, tests all link). Installed App Installer version at
build/verification time:

```
(Get-AppxPackage -Name Microsoft.DesktopAppInstaller).Version
→ 1.30.80.0
```

(Newer than the `1.29.280.0` the CLSID table was originally confirmed against in
`docs/adr-phase-2.md` ADR-0009 — expected and harmless for the CLSIDs themselves, which
are unchanged; see the activation finding below for where a version difference did
matter.)

```
.\build\x64\Release\syncwingetlink.exe scan --source com --verbose
→ Failed to activate the winget PackageManager COM server
→ exit code 4

.\build\x64\Release\syncwingetlink.exe scan --source auto --verbose
→ warning: --source auto fell back to a filesystem scan: Failed to activate the winget PackageManager COM server
→ verbose: effective Links directory: C:\Users\...\AppData\Local\Microsoft\WinGet\Links
→ verbose: effective Packages directory: C:\Users\...\AppData\Local\Microsoft\WinGet\Packages
→ verbose: package source - requested: auto, used: filesystem (degraded: Failed to activate the winget PackageManager COM server)
→ verbose: rule source - embedded defaults
→ (21 packages enumerated, matching the machine's actual installed portable packages; exit code 0)

.\build\x64\Release\syncwingetlink.exe scan --source fs --verbose
→ (identical package list to --source auto above; exit code 0)
```

`--source com` failed and `--source auto` degraded cleanly to an identical, correct FS
result — exercising exactly the degrade path documented in "Failure and fallback."

**Root-cause narrowing of the `--source com` failure.** `winget list` (the real,
installed winget CLI) was confirmed working normally on the same machine at the same
time, ruling out a broken App Installer install. Two independent throwaway probes (not
part of the product, not committed, deleted after use) isolated the failure precisely:

1. PowerShell `[Type]::GetTypeFromCLSID('C53A4F16-787E-42A4-B304-29EFFB4BF597')` +
   `[Activator]::CreateInstance(...)` — requests only `IUnknown`/`IDispatch` — **succeeded**.
2. A minimal C# program P/Invoking `CoCreateInstance` with the same CLSID and
   `CLSCTX_LOCAL_SERVER`, but requesting the *typed* `IPackageManager` interface
   (`B375E3B9-F2E0-5C93-87A7-B67497F7E593`, the GUID the generated projection assigns as
   `PackageManager`'s default interface — exactly what `winrt::create_instance<PackageManager>`
   requests) — **failed** with HRESULT `0x80073D54`, decoded via
   `System.ComponentModel.Win32Exception` as Win32 error 15700
   (`APPMODEL_ERROR_NO_PACKAGE`, "the process has no package identity").

So the failure is specific to activating the typed WinRT interface out-of-process from
this unpackaged caller, not to CLSID registration or the server process itself. Under
`mapHresultToKind`, `0x80073D54` does not match any of the seven named HRESULTs and
therefore classifies as `PackageSourceErrorKind::Unknown` — confirmed by the CLI's own
message text above, which matches the `"Failed to activate the winget PackageManager COM
server"` throw site in `WingetComSource.cpp` for exactly that kind.

**Not verified, and stated as such rather than guessed at:**

- Whether this failure reproduces on other machines, other Windows builds, or the
  `1.29.280.0` App Installer version ADR-0009 was originally verified against — only one
  environment was available for this verification.
- Whether App Installer publishes any first-party statement of `packageQuery`-capability
  or integrity-level requirements for this interface; Microsoft Learn search during this
  verification returned no first-party page making that claim for
  `Microsoft.Management.Deployment`, so the previous document's `packageQuery` sentence
  is removed rather than kept on the strength of a search that did not confirm it.
- ARM64 behavior — this verification ran on x64 only, consistent with every prior
  milestone's "cross-built, not run" disclosure for ARM64.

Debug/Release × x64/ARM64 build and `vstest.console.exe` results for this and the two
following M9 layers are recorded in their own `docs/task.md` entries, since layers 2 and
3 change no COM behavior and layer 3's comment-only source edits still go through the
full matrix per the project's Definition of Done.

### Consequences

- `docs/com-api.md` §"Capabilities / permissions" and §"Known caveats" cite this ADR
  rather than asserting the `packageQuery` requirement as fact; a reader who needs to
  know whether `--source com` works in their environment is told to check
  (`scan --source com --verbose`) rather than assured.
- `docs/adr-phase-2.md` ADR-0009 gets a dated amendment note (not a rewrite) pointing
  readers here for the current apartment-ownership arrangement and the removed
  `PackageExe::metadataAlias` field.
- `docs/TODO.md` M9's checklist item is checked, citing this ADR and issue #66.
- The `APPMODEL_ERROR_NO_PACKAGE` finding is not, by itself, treated as a defect to fix
  in this milestone — M9 is scoped to documentation only (three doc/comment-only stacked
  layers under issue #11). Whether it warrants a follow-up investigation issue is left to
  the project owner.
