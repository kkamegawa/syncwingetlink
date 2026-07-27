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

1. **The regex replacement rules in this document**
2. **The raw file name** (if no rule matches, or if the matching rule's replacement is not
   itself a well-formed alias file name — see "Evaluation rules" below)

An earlier draft of this document also listed winget COM API metadata (a
`PortableCommandAlias`-equivalent) as the first-priority tier. That API does not exist —
the COM API's package metadata has no per-file alias field, so syncwingetlink never had
COM-provided aliases to prefer over a rule match. Alias resolution therefore starts from
the regex tier.

## Rule file location and priority

Load priority (higher first):

1. JSON explicitly specified via `--rules <path>`
2. `%LOCALAPPDATA%\syncwingetlink\rules.json` (user settings)
3. default rules embedded in the binary

Once one of these is selected, its rules are used **as a complete replacement** for any
lower tier — they are not merged. A user `rules.json` containing only your own custom
rules means the embedded defaults (Rust target-triple stripping, etc.) no longer apply at
all; a file name they would otherwise have rewritten falls straight through to the raw
file name instead.

An explicit `--rules <path>` must exist and parse — a missing, unreadable, or invalid file
is a configuration error (exit code 3), never silently ignored. The user rules file at
`%LOCALAPPDATA%\syncwingetlink\rules.json` is optional: if it is simply **absent**, that is
the normal, unconfigured state and the embedded defaults are used. But if it **exists** and
is unreadable or invalid, that is treated exactly like a broken `--rules` file — a
configuration error, not a silent fallback to the embedded defaults. Silently ignoring a
rules file you wrote yourself would hide a mistake in it.

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
- A rule's `pattern` is matched against the **entire** real file name — a pattern must
  match from start to end (equivalent to anchoring with `^` and `$`, though writing the
  anchors explicitly is still recommended for clarity), not just some substring within it.
- The alias a matching rule produces must itself be a well-formed alias file name: a
  non-empty `.exe` file name (case-insensitive), with no path separator, and not made up
  entirely of dots (e.g. an empty capture producing just `.exe`). If the first matching
  rule's replacement fails this check, that counts as **no match at all** — evaluation
  does not fall through to try a later rule; it falls straight through to the raw file
  name instead, so which rule (if any) "wins" a given file name never depends on what
  other rules happen to be configured.
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
