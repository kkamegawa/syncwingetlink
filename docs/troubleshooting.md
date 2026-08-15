<!-- SPDX-License-Identifier: MIT -->

# Troubleshooting

This page covers the most common failure modes reported by `syncwingetlink`, especially
when package enumeration is forced to use the winget COM API.

## COM activation reports `APPMODEL_ERROR_NO_PACKAGE`

**Symptom**

```text
The winget PackageManager COM server rejected typed activation from this unpackaged process
(APPMODEL_ERROR_NO_PACKAGE, HRESULT 0x80073d54)
hint: This host rejected typed WinRT activation from an unpackaged process; re-run with --source fs, or use --source auto to fall back automatically. See https://github.com/kkamegawa/syncwingetlink/blob/main/docs/troubleshooting.md
```

**Cause**

On some hosts, the winget COM server is installed and `winget` itself works, but typed
WinRT activation of `PackageManager` is rejected for this unpackaged desktop process.
With `--source com`, this is a terminal failure (exit code `4`). With `--source auto`,
the same COM failure can appear first and then be followed by a successful filesystem
fallback.

**Fix**

Re-run with `--source fs`, or use the default `--source auto` mode so the tool can fall
back automatically.

## `--verbose` says `used: filesystem (degraded: ...)`

**Symptom**

```text
verbose: package source - requested: auto, used: filesystem (degraded: <reason>)
```

**Cause**

`--source auto` first tries COM and then degrades to the filesystem scan when COM package
enumeration fails with a `PackageSourceError`. This is expected on hosts where COM
activation is unavailable.

**Fix**

No action is required when the command otherwise succeeds. If you want to bypass COM
entirely on that host, pass `--source fs`.

## Exit code 2: insufficient permission creating symlinks

**Symptom**

`fix` reports an insufficient-permission error and exits with code `2`.

**Cause**

Creating symlinks without elevation requires Developer Mode (or equivalent privilege) to
be enabled.

**Fix**

Enable Developer Mode, or re-run the command from an elevated shell.

`fix` now performs a startup check before repair begins. If Developer Mode is disabled
or cannot be determined, the CLI prints a warning first. On a Japanese UI OS that
warning and the optional elevation prompt are shown in Japanese; on every other OS they
fall back to English. Pass `--silent` to suppress the "restart elevated?" question and
print only the warning text.

For `fix --tui`, declining elevation or suppressing the prompt with `--silent` exits
with code `2` without opening the editable checklist.

## Exit code 3: invalid `rules.json`

**Symptom**

The command reports a rules parse/validation error and exits with code `3`.

**Cause**

The replacement-rules JSON is malformed or does not match the documented schema.

**Fix**

Validate the file against the format described in [`rules.md`](./rules.md), then retry.

## No packages found

**Symptom**

`scan` or `fix` completes, but no installed portable packages are reported.

**Cause**

The host may genuinely have no user-scope portable packages, or the package scan is
pointing at the wrong directory.

**Fix**

If your packages are in a non-default location, pass `--packages-dir <path>`. Machine-
scope support is out of scope for the first release.
