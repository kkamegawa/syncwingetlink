# syncwingetlink

> A native CLI tool that detects and recreates the command-alias symlinks winget is supposed to create for portable packages (Windows 11 24H2+).

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)

📖 日本語版は [`README_ja.md`](./README_ja.md) を参照してください。

Runtime diagnostics (warnings, errors, `--help` text) are English-only for the first
release; this repository's own documentation, including `README_ja.md`, is how
Japanese is served instead.

## What is this?

When you install a portable package with winget, a command-alias symlink is normally
created at `%LOCALAPPDATA%\Microsoft\WinGet\Links\<alias>.exe`, and because that folder
is on your `PATH`, you can invoke the tool from the CLI.

However, in some environments this symlink is not created or becomes broken. As a result
you can only launch the tool by its long real file name, such as
`codex_0.x_x86_64-pc-windows-msvc.exe`, and the short alias like `codex` does not work.

**syncwingetlink** enumerates installed portable packages, compares them against the
links that should exist in the `Links` folder, detects the missing/broken ones, and
recreates them after user confirmation.

## Features

- 🔎 **Detect**: enumerate installed portable packages and classify link state as
  `Ok / Missing / Broken / Mismatch`
- 🔗 **Repair**: create missing/broken symlinks after confirmation (`--dry-run` supported)
- 🧩 **Alias regex rules**: customizable rules to strip platform suffixes, e.g.
  `codex-x86_64-pc-windows-msvc.exe → codex.exe`
- 🖥️ **CLI / TUI**: CLI by default, interactive checklist with `--tui`
- ⚡ **winget COM API first**: authoritative enumeration via
  `Microsoft.Management.Deployment`, with automatic fallback to filesystem scanning

## Requirements

- Windows 11 24H2 (build 26100) or later
- x64 / arm64
- Creating symlinks requires **Developer Mode** enabled (recommended) or administrator
  privileges

## Installation

> Coming soon. Once a release is published, you can grab a single exe from GitHub
> Releases.

```powershell
# (example, after publishing)
winget install <publisher>.syncwingetlink
```

## Usage

```powershell
# Detect only (read-only)
syncwingetlink scan

# Repair missing/broken links (with confirmation prompt)
syncwingetlink fix

# See what would happen (no side effects)
syncwingetlink fix --dry-run

# Select and batch-create in the interactive TUI
syncwingetlink fix --tui

# Check which replacement rule applies to a given file name
syncwingetlink test-rule "codex-x86_64-pc-windows-msvc.exe"
```

### Main options

| Option | Description |
|---|---|
| `--source com\|fs\|auto` | Package enumeration source (default `auto`: COM first, FS fallback) |
| `--dry-run` | Show the plan without executing (`fix`) |
| `--yes`, `-y` | Skip confirmation and execute |
| `--rules <path>` | Path to a replacement-rules JSON |
| `--tui` | Interactive TUI mode |
| `--json` | Emit results as JSON (for scripting) |
| `--help` / `--version` | Help / version |

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success (nothing to fix or fixed) |
| 1 | Fix needed but not performed |
| 2 | Insufficient permission (Developer Mode off & non-admin) |
| 3 | Argument / config error |
| 4 | Package enumeration failed (explicit `--source com`/`--source fs` could not enumerate at all) |
| 10 | Some repairs failed |

## Alias replacement rules

You can define regex rules (in JSON) that derive the alias name from the real file name.
See [`docs/rules.md`](./docs/rules.md) for details.

```json
{
  "version": 1,
  "rules": [
    {
      "name": "strip-rust-target-triple",
      "pattern": "^(.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
      "replacement": "$1.exe",
      "flags": ["ignorecase"]
    }
  ]
}
```

## Building (for developers)

Requires **Visual Studio 2026** (platform toolset v145) and Windows SDK 10.0.26100.0.
Run from a Developer PowerShell for VS 2026:

```powershell
msbuild syncwingetlink.sln -p:Configuration=Release -p:Platform=x64 -m
vstest.console.exe build\x64\Release\syncwingetlink.tests.dll /Platform:x64
```

Unit tests use MSTest (the Microsoft Unit Testing Framework for C++) and are also runnable
from the Visual Studio Test Explorer.

See [`docs/PLAN.md`](./docs/PLAN.md) for the design, [`docs/TODO.md`](./docs/TODO.md) for
the work breakdown, and [`docs/adr.md`](./docs/adr.md) for architecture decisions.

## Repository setup (after forking)

This repository deliberately ships placeholders instead of hard-coded URLs and e-mail
addresses, so no identifying information is committed. Replace them once with either
script — they are equivalent:

```powershell
./tools/Set-RepositoryPlaceholders.ps1 -Owner <owner> -SecurityContact <email>
```

```bash
tools/set-repository-placeholders.sh --owner <owner> --security-contact <email>
```

Both accept `-WhatIf` / `--dry-run` to preview the changes. They replace `OWNER` in
`.github/ISSUE_TEMPLATE/config.yml` and `<SECURITY_CONTACT_EMAIL>` in `SECURITY.md`.

## Contributing

Contributions are welcome! See [`CONTRIBUTING.md`](./CONTRIBUTING.md). If you use AI
coding agents, read [`AGENTS.md`](./AGENTS.md) first.

Security issues should be reported privately — see [`SECURITY.md`](./SECURITY.md).

## License

[MIT License](./LICENSE)

## Disclaimer

This tool creates and deletes symlinks. `scan` is read-only, but when using `fix` it is
recommended to verify with `--dry-run` first. It never modifies winget's own database.
