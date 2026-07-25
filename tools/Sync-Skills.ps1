#!/usr/bin/env pwsh
# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
    Mirrors the canonical agent skills into every vendor-specific location.

.DESCRIPTION
    Canonical source : .github/skills/
    Mirrors          : .claude/skills/  (Claude Code)
                       .codex/skills/   (OpenAI Codex)

    The skills are duplicated rather than symlinked because each tool discovers them by
    path, and symlinks do not survive a plain `git clone` on Windows without developer
    mode. Run this after editing anything under .github/skills/.

.PARAMETER Check
    Exit with code 1 if any mirror is out of date. Writes nothing. Intended for CI.

.PARAMETER WhatIf
    Report what would change without writing anything.

.EXAMPLE
    ./tools/Sync-Skills.ps1

.EXAMPLE
    ./tools/Sync-Skills.ps1 -Check
#>
#Requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceDir = Join-Path $repoRoot '.github/skills'
$mirrors = @('.claude/skills', '.codex/skills')

if (-not (Test-Path -LiteralPath $sourceDir)) {
    throw "Canonical skills directory not found: $sourceDir"
}

function Test-SameContent {
    param([string]$Left, [string]$Right)

    if (-not (Test-Path -LiteralPath $Right)) { return $false }

    $a = Get-FileHash -LiteralPath $Left -Algorithm SHA256
    $b = Get-FileHash -LiteralPath $Right -Algorithm SHA256
    return $a.Hash -eq $b.Hash
}

$stale = 0
$copied = 0

foreach ($mirror in $mirrors) {
    $targetDir = Join-Path $repoRoot $mirror

    foreach ($src in Get-ChildItem -LiteralPath $sourceDir -Recurse -File) {
        $rel = [System.IO.Path]::GetRelativePath($sourceDir, $src.FullName)
        $dst = Join-Path $targetDir $rel

        if (Test-SameContent -Left $src.FullName -Right $dst) { continue }

        $stale++

        if ($Check) {
            Write-Host "out of date: $mirror/$($rel -replace '\\', '/')"
            continue
        }

        if ($PSCmdlet.ShouldProcess("$mirror/$($rel -replace '\\', '/')", 'Copy skill')) {
            $parent = Split-Path -Parent $dst
            if (-not (Test-Path -LiteralPath $parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Copy-Item -LiteralPath $src.FullName -Destination $dst -Force
            Write-Host "copied: $mirror/$($rel -replace '\\', '/')"
            $copied++
        }
    }

    # Report files that exist only in the mirror -- usually a rename left behind.
    if (Test-Path -LiteralPath $targetDir) {
        foreach ($orphan in Get-ChildItem -LiteralPath $targetDir -Recurse -File) {
            $rel = [System.IO.Path]::GetRelativePath($targetDir, $orphan.FullName)
            if (-not (Test-Path -LiteralPath (Join-Path $sourceDir $rel))) {
                Write-Warning "$mirror/$($rel -replace '\\', '/') has no canonical source; delete it manually"
                $stale++
            }
        }
    }
}

if ($Check) {
    if ($stale -gt 0) {
        Write-Host ''
        Write-Host 'Skill mirrors are out of date. Run: ./tools/Sync-Skills.ps1'
        exit 1
    }
    Write-Host 'All skill mirrors are up to date.'
    exit 0
}

if ($copied -eq 0) {
    Write-Host 'All skill mirrors are up to date.'
}
else {
    Write-Host ''
    Write-Host "Synced $copied file(s). Review before committing:"
    Write-Host '  git status .claude .codex .github/skills'
}
