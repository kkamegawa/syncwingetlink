#!/usr/bin/env pwsh
# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
    Replaces the repository placeholders left in the template files.

.DESCRIPTION
    The repository intentionally ships with placeholders instead of hard-coded URLs and
    e-mail addresses, so that no identifying information is committed. Run this script
    once after forking or before publishing.

    Replaced placeholders:
      OWNER                     -> the GitHub owner (user or organisation)
      <SECURITY_CONTACT_EMAIL>  -> the security contact address

.PARAMETER Owner
    GitHub owner (user or organisation) that hosts the repository.

.PARAMETER SecurityContact
    E-mail address for private vulnerability reports. Optional; when omitted, the
    security contact placeholder is left untouched.

.PARAMETER WhatIf
    Show which files would change without writing anything.

.EXAMPLE
    ./tools/Set-RepositoryPlaceholders.ps1 -Owner contoso

.EXAMPLE
    ./tools/Set-RepositoryPlaceholders.ps1 -Owner contoso -SecurityContact security@example.com
#>
#Requires -Version 7.0
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9](?:[A-Za-z0-9]|-(?=[A-Za-z0-9])){0,38}$')]
    [string]$Owner,

    [Parameter()]
    [ValidatePattern('^[^@\s]+@[^@\s]+\.[^@\s]+$')]
    [string]$SecurityContact
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

# Placeholder -> replacement, scoped to the files that actually contain it.
$replacements = @(
    [pscustomobject]@{
        Path        = Join-Path $repoRoot '.github/ISSUE_TEMPLATE/config.yml'
        Placeholder = 'OWNER'
        Value       = $Owner
    }
)

if ($SecurityContact) {
    $replacements += [pscustomobject]@{
        Path        = Join-Path $repoRoot 'SECURITY.md'
        Placeholder = '<SECURITY_CONTACT_EMAIL>'
        Value       = $SecurityContact
    }
}

$changed = 0

foreach ($item in $replacements) {
    if (-not (Test-Path -LiteralPath $item.Path)) {
        Write-Warning "Skipped (not found): $($item.Path)"
        continue
    }

    $original = Get-Content -LiteralPath $item.Path -Raw
    $updated = $original.Replace($item.Placeholder, $item.Value)

    if ($original -ceq $updated) {
        Write-Verbose "No occurrence of '$($item.Placeholder)' in $($item.Path)"
        continue
    }

    if ($PSCmdlet.ShouldProcess($item.Path, "Replace '$($item.Placeholder)'")) {
        # Preserve the file's existing encoding conventions: UTF-8 without BOM.
        [System.IO.File]::WriteAllText($item.Path, $updated, [System.Text.UTF8Encoding]::new($false))
        Write-Host "Updated $($item.Path)"
    }
    $changed++
}

if ($changed -eq 0) {
    Write-Host 'Nothing to do -- no placeholders remained.'
}
else {
    Write-Host "`nDone. Review the changes before committing:"
    Write-Host '  git diff'
}
