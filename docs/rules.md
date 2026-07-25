# Alias replacement rules

syncwingetlink derives the alias name (`<alias>.exe`) to create under `Links` from the
real file name of a portable package. To strip platform suffixes and versions, it uses
**regex replacement rules**.

📖 日本語版は [`rules_ja.md`](./rules_ja.md) を参照してください。

Examples:

```
codex-x86_64-pc-windows-msvc.exe   →   codex.exe
restic_0.15.2_windows_amd64.exe    →   restic.exe
```

## Alias resolution priority

The alias name is determined in the following priority order:

1. **COM metadata** (when the winget COM API returns a `PortableCommandAlias`-equivalent)
2. **The regex replacement rules in this document**
3. **The raw file name** (if no rule matches)

## Rule file location and priority

Load priority (higher first):

1. JSON explicitly specified via `--rules <path>`
2. `%LOCALAPPDATA%\syncwingetlink\rules.json` (user settings)
3. default rules embedded in the binary

## JSON schema

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

| Field | Required | Description |
|---|---|---|
| `version` | ✔ | Schema version. Currently `1`. |
| `rules` | ✔ | Array of rules. **Evaluated in order from the top**; the first match applies. |
| `rules[].name` | ✔ | Rule identifier (used in `test-rule` output and logs). |
| `rules[].pattern` | ✔ | Regex against the real file name (ECMAScript grammar). |
| `rules[].replacement` | ✔ | Replacement string. Reference captures with `$1`, `$2`, … |
| `rules[].flags` | ✖ | Optional. Currently supports `ignorecase`. |

> **Note**: the first release assumes `std::regex` (ECMAScript) and uses **numbered
> captures (`$1`)** by default. Because `std::regex` does not support named captures,
> `(?<name>...)` and `${name}` are planned for the future (RE2 adoption under
> consideration).

## Evaluation rules

- Rules are **evaluated in order from the top**, and **only the first matching rule**
  applies.
- If no rule matches, the real file name becomes the alias name as-is.
- If multiple exes resolve to the **same alias**, it is warned as a collision and no
  auto-creation happens (the user chooses).

## Verifying: `test-rule`

You can check which rule applies to a file name and what alias it yields.

```powershell
syncwingetlink test-rule "codex-x86_64-pc-windows-msvc.exe"
```

Example output:

```
input:        codex-x86_64-pc-windows-msvc.exe
matched rule: strip-rust-target-triple
alias:        codex.exe
```

## Samples

### Strip a Rust target triple

```json
{
  "name": "strip-rust-target-triple",
  "pattern": "^(.+?)[-_](x86_64|aarch64|i686)-pc-windows-(msvc|gnu)(\\.exe)$",
  "replacement": "$1.exe",
  "flags": ["ignorecase"]
}
```

### Strip version + architecture

```json
{
  "name": "strip-version-and-arch",
  "pattern": "^(.+?)[-_]v?\\d+\\.\\d+[^\\\\/]*?(windows|win)?[-_]?(amd64|x64|arm64)?\\.exe$",
  "replacement": "$1.exe",
  "flags": ["ignorecase"]
}
```

### Fixed mapping for a specific tool

```json
{
  "name": "map-kubelogin",
  "pattern": "^kubelogin.*\\.exe$",
  "replacement": "kubectl-oidc_login.exe",
  "flags": ["ignorecase"]
}
```

> Even for a fixed mapping where a prefix match would suffice, note that `pattern` is
> evaluated as a regex (`.` and similar must be escaped).
