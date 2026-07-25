#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Mirrors the canonical agent skills into every vendor-specific location.
#
# Canonical source : .github/skills/
# Mirrors          : .claude/skills/  (Claude Code)
#                    .codex/skills/   (OpenAI Codex)
#
# The skills are duplicated rather than symlinked because each tool discovers them by
# path, and symlinks do not survive a plain `git clone` on Windows without developer
# mode. Run this after editing anything under .github/skills/.
#
# Usage:
#   tools/sync-skills.sh [--check] [--dry-run]
#
#   --check    exit non-zero if any mirror is out of date (for CI); writes nothing
#   --dry-run  report what would change; writes nothing

set -euo pipefail

check_only=0
dry_run=0

while [ $# -gt 0 ]; do
    case "$1" in
        --check)   check_only=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help)
            sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "error: unknown argument: $1" >&2
            exit 2 ;;
    esac
done

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
source_dir="$repo_root/.github/skills"
mirrors=(".claude/skills" ".codex/skills")

if [ ! -d "$source_dir" ]; then
    echo "error: canonical skills directory not found: $source_dir" >&2
    exit 1
fi

stale=0
copied=0

for mirror in "${mirrors[@]}"; do
    target_dir="$repo_root/$mirror"

    # Walk every file under the canonical directory.
    while IFS= read -r -d '' src; do
        rel="${src#"$source_dir"/}"
        dst="$target_dir/$rel"

        if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
            continue
        fi

        stale=$((stale + 1))

        if [ "$check_only" -eq 1 ]; then
            echo "out of date: $mirror/$rel"
            continue
        fi

        if [ "$dry_run" -eq 1 ]; then
            echo "would copy: .github/skills/$rel -> $mirror/$rel"
            continue
        fi

        mkdir -p "$(dirname "$dst")"
        cp "$src" "$dst"
        echo "copied: $mirror/$rel"
        copied=$((copied + 1))
    done < <(find "$source_dir" -type f -print0)

    # Report files that exist only in the mirror -- usually a rename left behind.
    if [ -d "$target_dir" ]; then
        while IFS= read -r -d '' orphan; do
            rel="${orphan#"$target_dir"/}"
            if [ ! -f "$source_dir/$rel" ]; then
                echo "warning: $mirror/$rel has no canonical source; delete it manually" >&2
                stale=$((stale + 1))
            fi
        done < <(find "$target_dir" -type f -print0)
    fi
done

if [ "$check_only" -eq 1 ]; then
    if [ "$stale" -gt 0 ]; then
        echo
        echo "Skill mirrors are out of date. Run: tools/sync-skills.sh"
        exit 1
    fi
    echo "All skill mirrors are up to date."
    exit 0
fi

if [ "$dry_run" -eq 1 ]; then
    [ "$stale" -eq 0 ] && echo "All skill mirrors are up to date."
    exit 0
fi

if [ "$copied" -eq 0 ]; then
    echo "All skill mirrors are up to date."
else
    echo
    echo "Synced $copied file(s). Review before committing:"
    echo "  git status .claude .codex .github/skills"
fi
