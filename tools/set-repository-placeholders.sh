#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Replaces the repository placeholders left in the template files.
#
# The repository intentionally ships with placeholders instead of hard-coded URLs and
# e-mail addresses, so that no identifying information is committed. Run this once after
# forking or before publishing.
#
# Replaced placeholders:
#   OWNER                     -> the GitHub owner (user or organisation)
#   <SECURITY_CONTACT_EMAIL>  -> the security contact address
#
# Usage:
#   tools/set-repository-placeholders.sh --owner <owner> [--security-contact <email>] [--dry-run]
#
# Examples:
#   tools/set-repository-placeholders.sh --owner contoso
#   tools/set-repository-placeholders.sh --owner contoso --security-contact security@example.com

set -euo pipefail

owner=""
security_contact=""
dry_run=0

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --owner)
            [ $# -ge 2 ] || { echo "error: --owner requires a value" >&2; exit 2; }
            owner="$2"; shift 2 ;;
        --security-contact)
            [ $# -ge 2 ] || { echo "error: --security-contact requires a value" >&2; exit 2; }
            security_contact="$2"; shift 2 ;;
        --dry-run)
            dry_run=1; shift ;;
        -h|--help)
            usage 0 ;;
        *)
            echo "error: unknown argument: $1" >&2; usage 2 ;;
    esac
done

if [ -z "$owner" ]; then
    echo "error: --owner is required" >&2
    usage 2
fi

if ! printf '%s' "$owner" | grep -Eq '^[A-Za-z0-9]([A-Za-z0-9]|-[A-Za-z0-9])*$'; then
    echo "error: '$owner' is not a valid GitHub owner name" >&2
    exit 2
fi

if [ -n "$security_contact" ] && ! printf '%s' "$security_contact" | grep -Eq '^[^@[:space:]]+@[^@[:space:]]+\.[^@[:space:]]+$'; then
    echo "error: '$security_contact' is not a valid e-mail address" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
changed=0

# replace_in <file> <placeholder> <value>
replace_in() {
    local file="$1" placeholder="$2" value="$3"

    if [ ! -f "$file" ]; then
        echo "warning: skipped (not found): $file" >&2
        return 0
    fi

    if ! grep -qF -- "$placeholder" "$file"; then
        return 0
    fi

    if [ "$dry_run" -eq 1 ]; then
        echo "would update $file"
        changed=$((changed + 1))
        return 0
    fi

    # Use awk with literal string handling so that regex metacharacters in the
    # placeholder (for example the angle brackets) need no escaping.
    local tmp
    tmp="$(mktemp)"
    awk -v ph="$placeholder" -v val="$value" '
        {
            out = ""
            rest = $0
            n = length(ph)
            while ((i = index(rest, ph)) > 0) {
                out = out substr(rest, 1, i - 1) val
                rest = substr(rest, i + n)
            }
            print out rest
        }
    ' "$file" > "$tmp"
    mv "$tmp" "$file"

    echo "updated $file"
    changed=$((changed + 1))
}

replace_in "$repo_root/.github/ISSUE_TEMPLATE/config.yml" "OWNER" "$owner"

if [ -n "$security_contact" ]; then
    replace_in "$repo_root/SECURITY.md" "<SECURITY_CONTACT_EMAIL>" "$security_contact"
fi

if [ "$changed" -eq 0 ]; then
    echo "Nothing to do -- no placeholders remained."
else
    echo
    echo "Done. Review the changes before committing:"
    echo "  git diff"
fi
