# Contributing to syncwingetlink

Thanks for your interest in contributing to syncwingetlink! This document summarizes the
workflow and rules.

📖 日本語版は [`CONTRIBUTING_ja.md`](./CONTRIBUTING_ja.md) を参照してください。

## Before you start

- Design: [`docs/PLAN.md`](./docs/PLAN.md)
- Work breakdown (TODO): [`docs/TODO.md`](./docs/TODO.md)
- Architecture decisions: [`docs/adr.md`](./docs/adr.md)
- Guide for AI coding agents: [`AGENTS.md`](./AGENTS.md)

Please read the above before starting work.

## Types of contribution

- 🐛 Bug reports (Issues)
- 💡 Feature proposals (Issues / Discussions)
- 🔧 Code contributions (Pull Requests)
- 📝 Documentation improvements
- 🌐 Translations / additional alias replacement rules

## Filing an issue

When reporting a bug, please include:

- Steps to reproduce
- Expected vs. actual behavior
- Environment (output of `winget --info`, Windows version, architecture)
- Relevant logs / error messages

## Development environment

### Requirements

- Windows 11 24H2 (build 26100) or later
- **Visual Studio 2026** with the "Desktop development with C++" workload, including
  C++/WinRT and the **C++ AddressSanitizer / testing** components
  - Platform toolset **v145** is required. Visual Studio 2022 ships v143 and cannot build
    this solution.
  - Install the ARM64 build tools too if you intend to build the ARM64 configuration.
- Windows SDK 10.0.26100.0
- Developer Mode enabled (needed to test symlink creation)

CMake is not used; see [`docs/adr.md`](./docs/adr.md) ADR-0001.

### Build and test

Use a **Developer PowerShell for VS 2026** so `msbuild` and `vstest.console` are on `PATH`.

```powershell
# Build (x64 Debug)
msbuild syncwingetlink.sln -p:Configuration=Debug -p:Platform=x64 -m

# Test
vstest.console.exe build\x64\Debug\syncwingetlink.tests.dll /Platform:x64

# Run without side effects
.\build\x64\Debug\syncwingetlink.exe scan
```

Unit tests use **MSTest** — the Microsoft Unit Testing Framework for C++ (`CppUnitTest.h`).
They also appear in the Visual Studio Test Explorer, so you can run and debug them from
the IDE.

## Coding standards

- **Standard**: C++20. Prefer STL / `std::filesystem`.
- **Encoding**: internally UTF-16 (`std::wstring`), long-path support with `\\?\`.
- **Naming**: types `PascalCase`, functions/variables `camelCase`, constants
  `kPascalCase`, member variables prefixed with `m_`.
- **Formatting**: follow the repository `.clang-format`. Resolve `.clang-tidy` warnings by
  default — naming rules are enforced there.
- **Project files**: when adding or removing a source file, update both the `.vcxproj` and
  its `.vcxproj.filters`. New logic belongs in the `syncwingetlink.core` static library,
  not in the executable project, so that it remains unit-testable.
- **Dependencies**: keep additions minimal; justify them in the PR and confirm MIT
  compatibility.
- **Side effects**: `scan` must stay read-only. Destructive operations must honor
  `--dry-run`.

For detailed design constraints, see the "Key design constraints" section in
[`AGENTS.md`](./AGENTS.md).

## Pull request workflow

1. Fork the repository and create a branch
   - `feature/<topic>` / `fix/<topic>` / `docs/<topic>`
2. Implement your change and add corresponding tests
   - Unit tests are required for `[core]` logic (`AliasResolver` / `RuleSet` /
     `LinkInspector` / the package-enumeration switch)
3. Build and test locally
4. Use [Conventional Commits](https://www.conventionalcommits.org/) for commit messages
   - Example: `feat(core): add WingetComSource for installed package enumeration`
5. Open a PR and include:
   - the corresponding item in `docs/TODO.md`
   - a summary of changes and design decisions
   - what was tested and the results (note anything unverified)
   - whether there are breaking changes

### PR size

Keep each PR small so it is easy to review. Split large features into multiple PRs.

## Using AI coding agents

If you use GitHub Copilot / Codex / Claude, always feed [`AGENTS.md`](./AGENTS.md) to the
agent, and make sure it loads the **`cpp-msbuild` skill** before touching C++:

| Tool | Path |
|---|---|
| GitHub Copilot | `.github/skills/cpp-msbuild/SKILL.md` (**canonical**) |
| Claude | `.claude/skills/cpp-msbuild/SKILL.md` |
| Codex | `.codex/skills/cpp-msbuild/SKILL.md` |

Only `.github/skills/` is tracked in git. The other two are **generated locally and
gitignored**, so run the sync script once after cloning:

```powershell
./tools/Sync-Skills.ps1      # or: tools/sync-skills.sh
```

**Edit only the canonical copy**, then re-run the script. `--check` / `-Check` reports
whether a local mirror is stale without writing anything.

In particular:

- Do not expand scope unilaterally (see non-goals in `docs/PLAN.md`)
- Write original code instead of copying from other repositories (avoid license leakage)
- Confirm generated code is MIT-compatible
- Do not use `*_ja.md` files as agent input; they are translations only

## License and copyright

- This project is published under the [MIT License](./LICENSE).
- Contributed code is considered to be provided under the MIT License.
- Do not import copyleft (e.g. GPL) code.
- Do not commit secrets (tokens, personal paths, internal information).

## Code of conduct

Please communicate constructively and respectfully so everyone can collaborate.

---

If you have questions, feel free to open an Issue or start a Discussion. Thanks for
contributing! 🙌
