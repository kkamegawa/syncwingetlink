#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Fails when an unrecorded dependency, or an unpinned/un-allow-listed GitHub Action,
# appears in the tracked source tree. Bash twin of Test-DependencyInventory.ps1 -- keep
# both in sync; see that script's header comment for the full rationale (issue #22,
# #164; docs/adr-phase-9.md ADR-0043).
#
# Two checks, both scoped to `git ls-files` so untracked scratch files never trip them:
#
#   1. Dependency tripwire -- flags any tracked dependency manifest, .gitmodules,
#      vendored source tree, checked-in binary, or MSBuild <PackageReference> that is
#      not recorded in .github/dependency-inventory.json.
#   2. GitHub Actions pin check -- every `uses:` in .github/workflows/*.yml must be
#      pinned to a full 40-character commit SHA, and its owner/repo must appear in the
#      inventory's githubActions allow-list.
#
# Zero external tools beyond git, awk, sed and grep (all POSIX/ubiquitous) -- no jq,
# matching the project's prefer-no-dependencies policy (docs/adr.md ADR-0005).
#
# Usage:
#   tools/test-dependency-inventory.sh

set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

inventory_path=".github/dependency-inventory.json"
if [ ! -f "$inventory_path" ]; then
    echo "error: dependency inventory not found: $inventory_path" >&2
    exit 1
fi

# Extracts every `"repo": "..."` value from a top-level JSON array section, given the
# file is formatted the way .github/dependency-inventory.json is (2-space top-level
# indent, array closed by a line starting with exactly two spaces then ']').
extract_repos_in_section() {
    local file="$1" section_key="$2"
    awk -v key="\"${section_key}\":" '
        $0 ~ "^  " key {
            in_section = 1
            if ($0 ~ /\[\]/) { in_section = 0 }
            next
        }
        in_section && /^  \]/ { in_section = 0; next }
        in_section && /"repo":/ {
            line = $0
            sub(/.*"repo":[ \t]*"/, "", line)
            sub(/".*/, "", line)
            print line
        }
    ' "$file"
}

# Extracts every quoted string from a top-level JSON array-of-strings section.
extract_strings_in_section() {
    local file="$1" section_key="$2"
    awk -v key="\"${section_key}\":" '
        $0 ~ "^  " key {
            in_section = 1
            if ($0 ~ /\[\]/) { in_section = 0 }
            next
        }
        in_section && /^  \]/ { in_section = 0; next }
        in_section {
            line = $0
            gsub(/^[ \t]*"/, "", line)
            gsub(/",?[ \t]*$/, "", line)
            if (line != "") print line
        }
    ' "$file"
}

in_list() {
    local needle="$1" hay="$2" item
    for item in $hay; do
        [ "$item" = "$needle" ] && return 0
    done
    return 1
}

allowed_action_repos="$(extract_repos_in_section "$inventory_path" "githubActions" | tr '[:upper:]' '[:lower:]')"
agent_tooling_repos="$(extract_repos_in_section "$inventory_path" "agentToolingDependencies")"
acknowledged_paths="$(extract_strings_in_section "$inventory_path" "acknowledgedPaths")"
has_agent_tooling=0
[ -n "$agent_tooling_repos" ] && has_agent_tooling=1

tracked_files="$(git ls-files)"

violations=()

# ---- Check 1: dependency tripwire ------------------------------------------------
manifest_basenames="vcpkg.json vcpkg-configuration.json packages.config packages.lock.json conan.lock CMakeLists.txt CMakePresets.json package.json requirements.txt go.mod Cargo.toml .gitmodules"
agent_manifest_basenames="apm.yml apm.lock.yaml"
vendor_dir_names="third_party thirdparty vendor external extern"
binary_extensions=".lib .dll .winmd .nupkg .a .so .exe"

while IFS= read -r file; do
    [ -z "$file" ] && continue
    basename="${file##*/}"

    extension=""
    case "$basename" in
        *.*) extension=".${basename##*.}" ;;
    esac
    extension_lc="$(printf '%s' "$extension" | tr '[:upper:]' '[:lower:]')"

    if in_list "$basename" "$agent_manifest_basenames"; then
        if [ "$has_agent_tooling" -eq 0 ]; then
            violations+=("Unacknowledged agent-tooling manifest: $file (record it under agentToolingDependencies)")
        fi
        continue
    fi

    if printf '%s\n' "$acknowledged_paths" | grep -qxF "$file"; then
        continue
    fi

    is_manifest=0
    in_list "$basename" "$manifest_basenames" && is_manifest=1

    is_conanfile=0
    case "$basename" in conanfile.*) is_conanfile=1 ;; esac

    is_vendor_dir=0
    IFS='/' read -ra segments <<< "$file"
    for seg in "${segments[@]}"; do
        seg_lc="$(printf '%s' "$seg" | tr '[:upper:]' '[:lower:]')"
        if in_list "$seg_lc" "$vendor_dir_names"; then
            is_vendor_dir=1
            break
        fi
    done

    is_binary=0
    in_list "$extension_lc" "$binary_extensions" && is_binary=1

    is_project_file=0
    case "$extension_lc" in .vcxproj|.props|.targets) is_project_file=1 ;; esac

    if [ "$is_manifest" -eq 1 ] || [ "$is_conanfile" -eq 1 ]; then
        violations+=("Untracked dependency manifest: $file")
    elif [ "$is_vendor_dir" -eq 1 ]; then
        violations+=("Untracked vendored/third-party tree: $file")
    elif [ "$is_binary" -eq 1 ]; then
        violations+=("Checked-in binary (should be gitignored, or is an undeclared dependency): $file")
    elif [ "$is_project_file" -eq 1 ]; then
        if grep -qE '<PackageReference|packages\.config' "$file"; then
            violations+=("NuGet package reference found in $file (project uses vcpkg, not NuGet -- see docs/adr.md ADR-0007)")
        fi
    fi
done <<< "$tracked_files"

# ---- Check 2: GitHub Actions pinning ----------------------------------------------
# Derived from $tracked_files, not a glob, so this stays scoped to `git ls-files` like
# the header comment promises: an untracked scratch workflow file never trips it, and
# .yaml (not just .yml) workflows are covered too.
workflow_count=0
workflow_list="$(printf '%s\n' "$tracked_files" | grep -E '^\.github/workflows/.*\.ya?ml$' || true)"
while IFS= read -r workflow; do
    [ -z "$workflow" ] && continue
    workflow_count=$((workflow_count + 1))
    workflow_name="$(basename "$workflow")"

    while IFS= read -r line; do
        ref="$(printf '%s\n' "$line" \
            | grep -oE '^[[:space:]]*(-[[:space:]]*)?uses:[[:space:]]*[^[:space:]#]+' \
            | sed -E 's/^[[:space:]]*(-[[:space:]]*)?uses:[[:space:]]*//' || true)"
        [ -z "$ref" ] && continue

        case "$ref" in
            ./*) continue ;;
            docker://*)
                violations+=("$workflow_name: docker:// action reference is not allowed: $ref")
                continue
                ;;
        esac

        if [[ "$ref" != *"@"* ]]; then
            violations+=("$workflow_name: action reference has no @<sha> pin: $ref")
            continue
        fi

        repo_path="${ref%@*}"
        pin="${ref##*@}"
        owner="$(printf '%s' "$repo_path" | cut -d/ -f1)"
        repo="$(printf '%s' "$repo_path" | cut -d/ -f2)"
        if [ -z "$owner" ] || [ -z "$repo" ]; then
            violations+=("$workflow_name: unrecognized action reference: $ref")
            continue
        fi
        owner_repo_lc="$(printf '%s/%s' "$owner" "$repo" | tr '[:upper:]' '[:lower:]')"

        if ! printf '%s' "$pin" | grep -qE '^[0-9a-f]{40}$'; then
            violations+=("$workflow_name: '$owner_repo_lc' is not pinned to a full 40-character commit SHA: $ref")
        fi
        if ! printf '%s\n' "$allowed_action_repos" | grep -qxF "$owner_repo_lc"; then
            violations+=("$workflow_name: action repo '$owner_repo_lc' is not in .github/dependency-inventory.json's githubActions allow-list")
        fi
    done < "$workflow"
done <<< "$workflow_list"

if [ "${#violations[@]}" -gt 0 ]; then
    echo "::error::Dependency inventory check failed with ${#violations[@]} finding(s):"
    for v in "${violations[@]}"; do
        echo "::error::$v"
    done
    echo "::error::See the cpp-msbuild skill (.github/skills/cpp-msbuild/SKILL.md, section 5) and .github/dependency-inventory.json."
    exit 1
fi

tracked_count="$(printf '%s\n' "$tracked_files" | grep -c . || true)"
echo "Dependency inventory check passed: ${tracked_count} tracked file(s), ${workflow_count} workflow file(s), zero findings."
exit 0
