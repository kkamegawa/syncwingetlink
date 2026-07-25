# AGENTS.md

> This file is the shared guide for AI coding agents (GitHub Copilot / OpenAI Codex /
> Claude, etc.). Human contributors should start with [`README.md`](./README.md) and
> [`CONTRIBUTING.md`](./CONTRIBUTING.md).
> Agents **must read this file before generating or editing any code**.

> **⚠️ Language policy for agents**
> The canonical documentation is written in **English**. Localized copies exist with a
> `_ja.md` suffix (e.g. `README_ja.md`, `docs/PLAN_ja.md`). **Agents must NOT read,
> parse, or edit any `*_ja.md` file.** They are human-facing translations only and are
> not a source of truth. Always rely on the English `.md` files. If you change an
> English doc, do not attempt to sync the `_ja.md` counterpart unless a human explicitly
> asks for it.

---

## 1. Project overview

**syncwingetlink** is a native CLI tool that detects and recreates the command-alias
symlinks that winget is supposed to create for portable packages under
`%LOCALAPPDATA%\Microsoft\WinGet\Links`. When those symlinks are missing or broken,
it enumerates installed packages, compares them against the `Links` folder, and, after
user confirmation, recreates the missing/broken links.

- **Language / API**: C++20 + Win32 API (+ C++/WinRT)
- **UI**: CLI (default) / interactive TUI via the `--tui` flag
- **Target OS**: Windows 11 24H2 (build 26100) or later, x64 / arm64
- **License**: MIT (to be published as OSS on GitHub)
- **Data source**: winget COM API (`Microsoft.Management.Deployment`) first, with a
  filesystem-scan fallback when COM is unavailable

See [`docs/PLAN.md`](./docs/PLAN.md) for the full design and
[`docs/TODO.md`](./docs/TODO.md) for the work breakdown.

---

## 2. Core principles for agents

1. **Follow the design.** Respect the architecture and the non-goals in `docs/PLAN.md`.
   Do not expand scope on your own (e.g. machine-scope support and automatic PATH
   registration are out of scope for the first release).
2. **Work in small steps.** One task = one small PR. Progress by the milestones in
   `docs/TODO.md`.
3. **Add tests.** Logic marked `[core]` (`AliasResolver` / `RuleSet` / `LinkInspector` /
   the package-enumeration switch) must have unit tests.
4. **Do not fill gaps by guessing.** If something is unclear, state it as "needs
   confirmation" in the TODO or PR description instead of deciding silently.
5. **Be honest.** If something cannot be implemented, or an API does not behave as
   expected, document the limitation in comments or the PR rather than hiding it.
6. **OSS quality.** Since this is published, watch for copyright, license, and secret
   leakage (see §8).

---

## 3. Repository layout

```
syncwingetlink/
├─ AGENTS.md                   # ← this file
├─ README.md                   # user-facing docs (English, canonical)
├─ README_ja.md                # Japanese translation (agents: do not read)
├─ CONTRIBUTING.md             # contribution guide (English, canonical)
├─ CONTRIBUTING_ja.md          # Japanese translation (agents: do not read)
├─ LICENSE                     # MIT
├─ SECURITY.md                 # vulnerability reporting / supported versions
├─ .github/skills/
│  └─ cpp-msbuild/SKILL.md     # ★C++ build & coding rules (CANONICAL; see §9)
├─ .claude/skills/             # local mirror (generated & gitignored, do not edit)
├─ .codex/skills/              # local mirror (generated & gitignored, do not edit)
├─ tools/                      # repo setup & skill sync scripts (pwsh + bash pairs)
├─ syncwingetlink.sln          # VS2026 solution (Debug|Release × x64|ARM64)
├─ Directory.Build.props       # shared MSBuild properties (toolset v145, SDK, out dirs)
├─ props/
│  └─ syncwingetlink.common.props # shared compiler/linker settings (C++20, /W4 /WX, CRT)
├─ src/
│  ├─ syncwingetlink.core.vcxproj # static library: cli/ core/ rules/ tui/
│  ├─ syncwingetlink.vcxproj   # executable: main.cpp only
│  ├─ app.manifest             # longPathAware / asInvoker
│  ├─ main.cpp                 # entry point, arg parsing, mode dispatch
│  ├─ cli/                     # ArgParser / Console (presentation layer)
│  ├─ core/                    # domain logic (Win32/COM dependencies isolated)
│  │  ├─ IPackageSource.h      # abstract IF for enumerating installed packages
│  │  ├─ WingetComSource.*     # COM API implementation (default source)
│  │  ├─ ComApartment.*        # process-wide CoInitializeEx RAII, shared by WingetComSource
│  │  ├─ PackageSourceError.*  # typed enumeration-failure exception + HRESULT mapping
│  │  ├─ ExecutableScanner.*   # shared *.exe walk used by WingetComSource and FsScanSource
│  │  ├─ FsScanSource.*        # filesystem-scan fallback
│  │  ├─ LinkInspector.*       # judges symlink state under Links
│  │  ├─ AliasResolver.*       # decides the alias name
│  │  ├─ SymlinkService.*      # create/delete/verify symlinks
│  │  ├─ Paths.* / Model.h
│  ├─ rules/                   # regex replacement rules (RuleSet / defaults)
│  └─ tui/                     # interactive UI for --tui
├─ tests/
│  └─ syncwingetlink.tests.vcxproj # MSTest (Microsoft Unit Testing Framework for C++)
└─ docs/
   ├─ PLAN.md                  # design document (English, canonical)
   ├─ PLAN_ja.md               # Japanese translation (agents: do not read)
   ├─ TODO.md                  # implementation checklist (English, canonical)
   ├─ TODO_ja.md               # Japanese translation (agents: do not read)
   ├─ rules.md                 # alias rule format & samples (English, canonical)
   ├─ rules_ja.md              # Japanese translation (agents: do not read)
   ├─ com-api.md               # COM API usage & notes (English, canonical)
   ├─ com-api_ja.md            # Japanese translation (agents: do not read)
   ├─ task.md                  # work log (English, canonical)
   └─ adr.md                   # architecture decision records (English, canonical)
```

### Layering principles
- `core/` **isolates Win32 / COM dependencies**. Write `AliasResolver` and `RuleSet` as
  pure logic with no filesystem dependency so they stay testable.
- `cli/` and `tui/` are thin presentation layers calling the same `core/` API.
- Package enumeration goes through the `IPackageSource` abstraction, so `WingetComSource`
  and `FsScanSource` are interchangeable.
- **All logic lives in the `syncwingetlink.core` static library.** The executable project
  contains `main.cpp` and nothing else. This is not stylistic: a C++ MSTest project is a
  DLL and cannot link an executable's object files, so anything placed directly in the
  executable becomes untestable. See `docs/adr.md` ADR-0002.

---

## 4. Build / test / run

> 📦 **The full C++ build, test, and coding rules live in the `cpp-msbuild` skill.**
> Canonical copy: [`.github/skills/cpp-msbuild/SKILL.md`](./.github/skills/cpp-msbuild/SKILL.md),
> mirrored to `.claude/skills/` and `.codex/skills/`.
> **Load it before writing, building, or testing any C++, editing a project file, or
> adding a dependency.** This section is only a summary.

```powershell
# From a Developer PowerShell for VS 2026
msbuild syncwingetlink.sln -p:Configuration=Debug -p:Platform=x64 -m
vstest.console.exe build\x64\Debug\syncwingetlink.tests.dll /Platform:x64
.\build\x64\Debug\syncwingetlink.exe scan
```

- Requires **Visual Studio 2026** (platform toolset `v145`). VS2022 ships `v143` and
  cannot build this solution.
- Output goes to `build\<Platform>\<Configuration>\`, not MSBuild's default location.
- ARM64 can be cross-built from x64, but ARM64 tests only *run* on an ARM64 host. Say
  "cross-built, not run" rather than claiming ARM64 was tested.
- If a full build is not possible locally, **confirm the tests pass** and note anything
  unverified in the PR.
- **CI does not exist yet.** `.github/workflows/` has no build workflow; it is an open
  item in `docs/TODO.md` M0. Do not assume a change is validated by CI.

---

## 5. Coding standards

> 📦 Full rules — naming, formatting, warnings, error handling, the library policy, and
> the dependency vulnerability gate — are in the **`cpp-msbuild` skill** (see §4).

The essentials:

- **Standard**: C++20. **Prefer the standard library and Windows APIs**; a third-party
  dependency is a last resort that must be justified in the PR, MIT-compatible, and free
  of known vulnerabilities.
- **Encoding**: internally UTF-16 (`std::wstring`), `\\?\` prefix for long paths, wide or
  UTF-8 console output so non-ASCII paths render correctly.
- **Naming**: types `PascalCase`, functions/variables `camelCase`, constants
  `kPascalCase`, members prefixed `m_`. Enforced by `.clang-tidy`.
- **Warnings**: `/W4 /WX`. Fix the code rather than widening the external-header exemption.
- **Project files**: update **both** the `.vcxproj` and its `.vcxproj.filters`. Put new
  logic in `syncwingetlink.core`, never directly in the executable project (see §3).
- **Explicit side effects**: honor `--dry-run` for any filesystem-mutating operation.
  `scan` must stay read-only.

---

## 6. Key design constraints (must read)

- **COM-first data source**: enumerate installed packages via the winget COM API first;
  keep direct sqlite (`PortableIndex`) reads as a last-resort fallback only (the schema
  is internal and may change).
- **Alias resolution priority**:
  1. COM metadata (when a `PortableCommandAlias`-equivalent is available)
  2. regex replacement rules in `rules/` (e.g. `codex-x86_64-pc-windows-msvc.exe → codex.exe`)
  3. the raw file name as-is
- **Permissions / Developer Mode**: pass
  `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE` when creating symlinks. If it fails when
  Developer Mode is off and the process is non-admin, diagnose the cause, state it
  clearly, and return exit code 2.
- **Never touch healthy links**: do not modify links judged `Ok`.
- **Alias collisions**: if multiple exes resolve to the same alias, warn instead of
  auto-creating.
- **Regex implementation constraint**: `std::regex` does not support named captures. The
  first release uses numbered captures (`$1`) as the default format. Discuss RE2 adoption
  in a PR.

### Exit codes
| Code | Meaning |
|---|---|
| 0 | Success (nothing to fix or fixed) |
| 1 | Fix needed but not performed (e.g. `--fail-on-missing`) |
| 2 | Insufficient permission (Developer Mode off & non-admin) |
| 3 | Argument / config error (e.g. invalid rules JSON) |
| 10 | Some repairs failed |

---

## 7. Git / PR conventions

- **Branches**: `feature/<topic>`, `fix/<topic>`, `docs/<topic>`.
- **Commits**: follow [Conventional Commits](https://www.conventionalcommits.org/).
  Example: `feat(core): add WingetComSource for installed package enumeration`
- **Every PR must include**:
  - the corresponding item in `docs/TODO.md`
  - a summary of changes and design decisions (especially around COM/FS/regex)
  - what was tested and the results (note anything unverified)
  - whether there are breaking changes
- **Keep PRs small**: maintain a reviewable granularity.
- **No secrets**: never commit tokens, personal paths, or internal information.

---

## 8. OSS / license notes (MIT, public)

- Any generated/added code must be **compatible with MIT**. Do not import copyleft (e.g.
  GPL) code verbatim.
- Avoid reusing snippets from other repositories; **write original code**. If you
  referenced something, note the source in the PR.
- Add an SPDX identifier to new files where appropriate
  (`// SPDX-License-Identifier: MIT`).
- Even when referencing winget internal implementation names (classes, functions), do not
  copy code — rely only on public specs and public APIs.
- Do not misuse trademarks or logos (Microsoft / winget, etc.).

---

## 9. Per-agent notes

- **All agents**: C++-specific rules are **not** in this file. Load the `cpp-msbuild`
  skill from your tool's own directory before touching C++:
  - GitHub Copilot → `.github/skills/cpp-msbuild/SKILL.md` (**canonical**)
  - Claude → `.claude/skills/cpp-msbuild/SKILL.md`
  - Codex → `.codex/skills/cpp-msbuild/SKILL.md`

  Only `.github/skills/` is tracked in git; the other two are **generated locally** and
  are gitignored. After cloning, materialise your tool's mirror:

  ```powershell
  ./tools/Sync-Skills.ps1      # or: tools/sync-skills.sh
  ```

  **Never edit a mirror** — edit `.github/skills/` and re-run the script.
  `--check` / `-Check` reports whether a local mirror is stale without writing.
- **GitHub Copilot**: this file is the source of truth for everything except the C++
  rules. Keep any additions consistent with it.
- **Codex / Claude**: before starting a task, read `docs/PLAN.md`, `docs/TODO.md`, and
  `docs/adr.md`, and name the target milestone in the PR description. Do not make large
  design changes unilaterally — propose them instead. If a change contradicts an existing
  ADR, stop and get approval rather than silently overriding it; if it merely extends the
  design, add a new ADR entry.
- **All agents**: when in doubt, prefer to **narrow scope / leave a question** rather than
  over-implement.
- **All agents**: never use `*_ja.md` files as input; they are translations only.

---

## 10. Definition of Done (excerpt)

- [ ] Builds and runs on Windows 11 24H2 (x64/arm64)
- [ ] Enumerates portable packages via the COM API, with automatic FS fallback when COM
      is unavailable
- [ ] `scan` classifies missing/broken/ok read-only
- [ ] `fix` creates symlinks via a confirmation prompt; `--dry-run` has no side effects
- [ ] Regex rules derive `codex-x86_64-pc-windows-msvc.exe → codex.exe`
- [ ] `--tui` allows interactive selection and batch creation
- [ ] On Developer Mode off, permission error is stated and exit code 2 is returned
- [ ] `AliasResolver` / `RuleSet` / `LinkInspector` have MSTest unit tests
- [ ] **All unit tests pass** (`vstest.console.exe` reports green — a successful build is
      not sufficient evidence)
- [ ] **No dependency has a known vulnerability**, and every dependency is MIT-compatible
      and justified

Per-change completion criteria are in the `cpp-msbuild` skill, section 1.

---

_This file is the project's source of truth for scope, architecture, and process. C++
build and coding rules live in the `cpp-msbuild` skill (see §4, §5, §9). Update whichever
is affected whenever the design changes._
