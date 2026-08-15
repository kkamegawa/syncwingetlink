---
name: cpp-msbuild
description: >
  C++ build, test, and coding rules for Windows-native projects using MSBuild and the
  Microsoft C++ toolset (Visual Studio 2022 or later). Use whenever writing, editing,
  building, or testing C++ on Windows - MSBuild/vcxproj changes, MSTest unit tests,
  Win32/C++-WinRT code, adding a dependency, or deciding whether a task is done.
  Covers the msbuild and vstest.console commands, the /W4 /WX and clang-tidy rules, the
  standard-library-and-Windows-API-first policy, and the dependency vulnerability gate.
  Not applicable to CMake, cross-platform, or non-Windows builds.
license: MIT
---

# C++ / MSBuild guide

> **Scope and prerequisites**
> This skill applies to **Windows-native C++ projects built with MSBuild and the
> Microsoft C++ toolset (Visual Studio 2022 or later / toolset v143+)**. It covers
> Win32 API usage, C++/WinRT, MSTest, and vcpkg. It does **not** cover cross-platform
> CMake builds, GCC/Clang on Linux, or non-Windows targets.

Read the project's `AGENTS.md` (or equivalent top-level instruction file) first — it
remains the source of truth for project scope, architecture, layering, and git/PR
conventions. Apply this skill for anything C++.

Design decisions are typically recorded in `docs/adr.md`. If a rule here contradicts a
project ADR, the ADR wins and this file needs updating.

---

## 1. Definition of done

A C++ change is **not** done until all of the following hold. Do not report completion
otherwise; if something is blocked, say which item and why.

1. **The solution builds clean** for `x64` in both `Debug` and `Release`, with no new
   warnings. The build runs at `/W4 /WX`, so a warning is already a hard failure.
2. **All unit tests are green.** Run `vstest.console.exe` and read the actual result — a
   build that merely succeeds is not a passing test run. If tests cannot be run, say so
   explicitly rather than implying they passed.
3. **New domain/core logic has MSTest coverage.** Any module identified as requiring
   tests in the project's `AGENTS.md` or `docs/TODO.md` must have tests before the
   change is considered done.
4. **No known vulnerabilities in dependencies** (section 5).
5. **`ARM64` still builds** (cross-build is enough; see section 2).
6. **`.vcxproj` and `.vcxproj.filters` are both updated** if files were added or removed.

---

## 2. Build / test / run

Run from a **Developer PowerShell for VS 2026** (Preview or later), which puts `msbuild`
and `vstest.console` on `PATH`. VS 2026 may still be a pre-release build; the `vswhere`
command below already includes `-prerelease` to handle both Preview and RTM installs.
Outside the shell, locate the installation with:

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -property installationPath
```

```powershell
# Build (x64 Debug). Use -p:Platform=ARM64 for the other target.
msbuild <YourProject>.sln -p:Configuration=Debug -p:Platform=x64 -m

# Test
vstest.console.exe build\x64\Debug\<YourProject>.tests.dll /Platform:x64

# Run the application
.\build\x64\Debug\<YourProject>.exe <args>
```

- **Toolset**: platform toolset `v145`, which ships with **Visual Studio 2026** (Preview or
  later — VS 2026 may still be pre-release; use the `-prerelease` flag in `vswhere` as
  shown above). VS 2022 provides `v143` and cannot build solutions that require `v145`.
- **Output location**: `build\<Platform>\<Configuration>\`, redirected by
  `Directory.Build.props` — not MSBuild's default `<Platform>\<Configuration>\`.
- **Configurations**: `Debug|Release` × `x64|ARM64`. ARM64 can be **cross-built** from an
  x64 host, but ARM64 tests only *run* on an ARM64 host. If you only cross-built, say
  "cross-built, not run" rather than claiming ARM64 was tested.
- **Shipping builds** link the CRT statically via `-p:StaticRuntime=true`. Check the
  project's ADR for the authoritative decision on CRT linkage. If the decision is marked
  unverified, treat the current setting as the intended default, flag it explicitly in the
  PR as unverified, and do not change it without first resolving that ADR.
- **CI**: Check whether `.github/workflows/` contains a build workflow before assuming
  a change was validated by CI. Never assume CI validation if no workflow exists.

### Project structure

A typical Windows native project uses three MSBuild projects; the split is
load-bearing, not stylistic:

| Project | Type | Contents |
|---|---|---|
| `src/<ProjectName>.core.vcxproj` | StaticLibrary | domain/core logic, all reusable modules |
| `src/<ProjectName>.vcxproj` | Application | `main.cpp` only |
| `tests/<ProjectName>.tests.vcxproj` | DynamicLibrary | MSTest cases |

A C++ MSTest project **must be a DLL** and cannot link an executable's object files.
Anything placed directly in the executable project becomes permanently untestable, so
**all logic goes in the core static library**.

---

## 3. Coding standards

- **Standard**: C++20 (`/std:c++20`).
- **Encoding**: internally UTF-16 (`std::wstring`). Use the `\\?\` prefix for long-path
  support. Emit console output via wide APIs or `SetConsoleOutputCP(CP_UTF8)` so non-ASCII
  paths render correctly.
- **Naming**: types `PascalCase`, functions/variables `camelCase`, constants
  `kPascalCase`, member variables prefixed `m_`, abstract interfaces prefixed `I`.
  `.clang-tidy` enforces this through `readability-identifier-naming`, so a naming mistake
  appears as a lint error rather than a review comment.
- **Formatting**: `.clang-format` at the repository root (Microsoft base style, Allman
  braces, 100 columns).
- **Warnings**: `/W4 /WX`. SDK and C++/WinRT headers are treated as external and exempt.
  Do not widen that exemption to silence a warning in our own code — fix the code.
- **Error handling**: check every Win32 return value and call `GetLastError()` on failure.
  Translate failures into messages a user can act on. Handle COM `HRESULT`s and C++/WinRT
  exceptions; never let an `hresult_error` escape to `main`.
- **Side effects**: any operation that is read-only (e.g. a scan or dry-run) must stay
  strictly read-only. Every filesystem-mutating operation must honour a `--dry-run` flag
  or equivalent guard if the project exposes one.
- **Exit codes**: define and document exit codes in the project's `AGENTS.md`. Typical
  conventions: `0` success, non-zero for errors — see the project docs for specifics.

---

## 4. Prefer the standard library and Windows APIs

**Default to the C++ standard library and the Windows platform. A third-party dependency
is the last resort, not the first.** Before adding one, check this table, and if you still
need it, justify it in the PR (see section 5).

| Need | Use | Not |
|---|---|---|
| Filesystem traversal, paths | `<filesystem>` | Boost.Filesystem |
| String formatting | `<format>` | fmt, `sprintf` |
| Regular expressions | `<regex>` (ECMAScript grammar) | Boost.Regex, PCRE |
| Optional / variant / span | `<optional>`, `<variant>`, `<span>` | Boost equivalents |
| Known folders | `SHGetKnownFolderPath` | hard-coded `%LOCALAPPDATA%` |
| Path manipulation | `<filesystem>`, or `PathCch*` for Win32-specific edge cases | `Shlwapi` `PathCombine` (deprecated, `MAX_PATH`-bound) |
| Symlink create / inspect | `CreateSymbolicLinkW`, `GetFileAttributesW`, `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` | shelling out to `mklink` |
| JSON | `winrt::Windows::Data::Json` — ships in the Windows SDK, costs no dependency | nlohmann/json, RapidJSON |
| COM lifetime / smart pointers | C++/WinRT (`winrt::com_ptr`) | ATL, WRL, raw `AddRef`/`Release` |
| C++/WinRT itself | the **Windows SDK's** bundled headers and `cppwinrt.exe` | the `Microsoft.Windows.CppWinRT` NuGet package |

Notes and caveats:

- `std::regex` has **no named capture groups**. Numbered captures (`$1`) are a portable
  default. Do not introduce RE2 or another engine without a PR discussion and ADR entry.
- `winrt::Windows::Data::Json` requires an initialised apartment
  (`winrt::init_apartment()`). If a JSON path must work *before* WinRT is available or
  without it entirely, raise it rather than silently pulling in a header-only parser.
- Prefer RAII everywhere. Wrap raw `HANDLE`s in a move-only holder rather than calling
  `CloseHandle` on every branch.
- `recursive_directory_iterator` does **not** descend into a symlink or a junction unless
  `directory_options::follow_directory_symlink` is set — do not set it when the tree may
  contain either, and no extra reparse-point check is needed to prevent a traversal loop.
  Still use `directory_options::skip_permission_denied` so an inaccessible subdirectory
  does not abort the whole scan. Query `FILE_ATTRIBUTE_REPARSE_POINT` only when the
  decision is about the *entry itself* (e.g. skipping a package directory that is itself
  a junction before scanning under it), not as a substitute for the iterator's own guard.

---

## 5. Dependencies and vulnerabilities

The package manager is **[vcpkg](https://github.com/microsoft/vcpkg)** in manifest mode
(`vcpkg.json` with a pinned `builtin-baseline`). **NuGet is not used** — consult the
project's ADR for the rationale.

vcpkg may ship bundled with Visual Studio at `VC\vcpkg\vcpkg.exe`; check your
installation before bootstrapping separately.

### Prefer no dependencies

If the Windows SDK already supplies what the project needs — C++/WinRT headers,
`windows.data.json.h`, `cppwinrt.exe` — there may be **no `vcpkg.json` yet, and that
is correct.** Do not create an empty manifest for its own sake. Add `vcpkg.json` at the
moment a real dependency becomes necessary, and not before.

### Triplets must match the CRT

A triplet encodes both library and CRT linkage, and it **must** agree with the project's
`RuntimeLibrary` or the link fails:

| `StaticRuntime` | Triplet | CRT |
|---|---|---|
| `false` (default, and the MSTest DLL) | `<arch>-windows-static-md` | `/MD` |
| `true` (shipping build) | `<arch>-windows-static` | `/MT` |

Plain `<arch>-windows` (dynamic libraries) is deliberately unused — it would require
shipping DLLs next to the executable, defeating the single-file release goal.

### Before adding or updating any dependency

1. **Justify it.** State in the PR why the standard library and Windows APIs (section 4)
   are insufficient.
2. **Confirm the licence is MIT-compatible.** Never import copyleft (GPL/LGPL) code.
3. **Check for known advisories** against the exact port version, via the
   [GitHub Advisory Database](https://github.com/advisories) and the upstream project's
   own security page.
4. **Pin `builtin-baseline`** so port versions never move implicitly. Roll it
   deliberately, and repeat step 3 when you do.
5. **Record it in `.github/dependency-inventory.json`** (`nativeDependencies`: port,
   version, license, justification, and the ISO date step 3 was performed). CI fails the
   `Dependency Audit` workflow if a `vcpkg.json`/port appears that isn't recorded there —
   see below.

### What the vulnerability gate automates today, and what stays manual

**No dependency may ship with a known vulnerability**; this is a completion condition
(section 1). Be precise about how it is enforced — part of this is now automated, part
is not, and conflating the two overstates the gate:

- **Automated (`.github/workflows/dependency-audit.yml`, `tools/Test-DependencyInventory.ps1`
  / `.sh`; see `docs/adr-phase-9.md` ADR-0043)**: CI fails the moment a tracked
  dependency manifest (`vcpkg.json`, `conan.lock`, `CMakeLists.txt`, `package.json`,
  `.gitmodules`, a vendored/`third_party` tree, a checked-in binary, or an MSBuild
  `<PackageReference>`) appears that isn't recorded in
  `.github/dependency-inventory.json`, and the moment a GitHub Actions `uses:` is
  unpinned (not a full 40-character commit SHA) or references a repo absent from the
  inventory's allow-list. This guarantees the dependency set can't silently grow — it is
  a *presence* check, not a vulnerability scan.
- **Still manual**: whether a given vcpkg port version actually has a known CVE.
  **Dependabot does not support vcpkg**, and GitHub's dependency graph does not parse
  `vcpkg.json`; `.github/dependabot.yml` covers `github-actions` only, on purpose. No
  scanner evaluated to date (OSV-Scanner included — see ADR-0043) understands
  `vcpkg.json` either. So the actual advisory check is the manual procedure above (steps
  1-4), performed whenever a port is added or the baseline is rolled.
- Never write "dependencies scanned" or "no vulnerabilities" in a PR unless you actually
  performed the manual check above. If the dependency set is empty, say that instead — it
  is a stronger and more honest claim than implying a scan happened.
- If a vulnerability is found in a dependency that cannot be updated, **stop and report
  it**. Do not work around it silently.
- Report vulnerabilities in *this* project privately per `SECURITY.md`; never in a public
  issue.

---

## 6. Working on project files

- Adding or removing a source file means editing **both** the `.vcxproj` and the matching
  `.vcxproj.filters`. Visual Studio does this for you; if you edit XML by hand, do both.
- Shared settings belong in `Directory.Build.props` (MSBuild *properties*) or a
  project-specific props file under `props/` (compiler/linker `ItemDefinitionGroup`). Do
  not duplicate compiler switches into individual projects.
- New logic goes in the core static library project. The executable project holds
  `main.cpp` and nothing else.
- Add `// SPDX-License-Identifier: MIT` to new source files.
- Write original code. Do not copy from other repositories — rely on public specs and
  public APIs only, and note any reference you consulted in the PR.
