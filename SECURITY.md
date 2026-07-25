# Security Policy

## Supported versions

syncwingetlink has not had its first release yet. Until `1.0.0` is published, only the
default branch is supported, and fixes are applied there.

| Version | Supported |
|---|---|
| `main` (unreleased) | ✅ |
| Any pre-release build | ❌ |

Once releases begin, the most recent minor release will be supported, and this table will
be updated accordingly.

## Reporting a vulnerability

**Please do not report security vulnerabilities through public GitHub issues.**

Report privately using either of the following:

- GitHub's [private vulnerability reporting](https://docs.github.com/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
  on this repository (**Security** → **Report a vulnerability**), which is the preferred
  channel.
- Email to `<SECURITY_CONTACT_EMAIL>`.

> `<SECURITY_CONTACT_EMAIL>` is a placeholder. Replace it before publishing the repository
> using the script in `tools/` (see the repository setup section of `README.md`).

Please include:

- The affected version or commit.
- A description of the issue and its impact.
- Steps to reproduce, or a proof of concept.
- Your environment: Windows version and build, architecture, and `winget --info` output.

### What to expect

- **Acknowledgement** within 5 business days.
- An initial **assessment** — including whether the report is accepted — within
  10 business days.
- Progress updates at least every 14 days while the issue is open.
- Public disclosure coordinated with you after a fix is available. Credit is given unless
  you prefer to remain anonymous.

## Scope

This tool creates, deletes, and inspects filesystem symbolic links under the current
user's `%LOCALAPPDATA%\Microsoft\WinGet\Links` directory, and queries installed packages
through the winget COM API. Reports are especially welcome for:

- Symlink or path handling that could be abused for **path traversal**, **link
  following**, or writing outside the intended `Links` directory.
- **Time-of-check to time-of-use (TOCTOU)** races between inspecting a link and
  recreating it.
- Improper handling of the alias replacement rules file that could lead to unintended
  file operations.
- Privilege issues around symlink creation, including any path that would require or
  silently obtain elevation.

The following are **out of scope**:

- Vulnerabilities in winget, the Windows Package Manager, or the Windows Package Manager
  COM server itself. Report those to their respective maintainers.
- Issues that require the attacker to already have administrator privileges or write
  access to the user's profile.
- Missing hardening that has no demonstrated security impact.

## Security practices in this repository

- The project is MIT licensed and contains no secrets. Tokens, personal paths, and
  internal information must never be committed.
- `scan` is strictly read-only. Any operation that mutates the filesystem must honour
  `--dry-run`.
- Released binaries are currently **unsigned**. Verify checksums published alongside a
  release before use.
