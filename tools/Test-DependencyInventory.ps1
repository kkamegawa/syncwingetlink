#!/usr/bin/env pwsh
# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
    Fails when an unrecorded dependency, or an unpinned/un-allow-listed GitHub Action,
    appears in the tracked source tree.

.DESCRIPTION
    This is the automated half of the dependency-vulnerability gate (issue #22, #164;
    see docs/adr-phase-9.md ADR-0043 for why it is shaped this way). No scanner
    understands vcpkg.json, so instead of pretending one does, this script enforces the
    project's actual invariant: the dependency set stays exactly what
    .github/dependency-inventory.json says it is.

    Two checks, both scoped to `git ls-files` so untracked scratch files never trip them:

    1. Dependency tripwire -- flags any tracked dependency manifest, .gitmodules, vendored
       source tree, checked-in binary, or MSBuild <PackageReference> that is not recorded
       in .github/dependency-inventory.json (as an acknowledged path, or, for the apm
       lockfile pair, via a populated agentToolingDependencies section).
    2. GitHub Actions pin check -- every `uses:` in .github/workflows/*.yml must be pinned
       to a full 40-character commit SHA, and its owner/repo must appear in the
       inventory's githubActions allow-list. Repos are allow-listed without a pinned SHA
       on purpose: Dependabot's routine SHA bumps stay green, while introducing a new
       third-party action -- the decision that actually deserves review -- fails here.

    Zero external tools beyond git, matching the project's prefer-no-dependencies policy
    (docs/adr.md ADR-0005).

.EXAMPLE
    ./tools/Test-DependencyInventory.ps1
#>
#Requires -Version 7.0
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    $inventoryPath = Join-Path $repoRoot '.github/dependency-inventory.json'
    if (-not (Test-Path $inventoryPath)) {
        throw "Dependency inventory not found at $inventoryPath"
    }
    $inventory = Get-Content -Raw -Path $inventoryPath | ConvertFrom-Json

    $acknowledgedPaths = @($inventory.acknowledgedPaths)
    $hasAgentTooling = @($inventory.agentToolingDependencies).Count -gt 0
    $allowedActionRepos = @($inventory.githubActions | ForEach-Object { $_.repo.ToLowerInvariant() })

    $trackedFiles = (git ls-files) | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' }

    $violations = [System.Collections.Generic.List[string]]::new()

    # ---- Check 1: dependency tripwire -------------------------------------------
    $manifestBasenames = @(
        'vcpkg.json', 'vcpkg-configuration.json', 'packages.config', 'packages.lock.json',
        'conan.lock', 'CMakeLists.txt', 'CMakePresets.json', 'package.json',
        'requirements.txt', 'go.mod', 'Cargo.toml', '.gitmodules'
    )
    $agentManifestBasenames = @('apm.yml', 'apm.lock.yaml')
    $vendorDirNames = @('third_party', 'thirdparty', 'vendor', 'external', 'extern')
    $binaryExtensions = @('.lib', '.dll', '.winmd', '.nupkg', '.a', '.so', '.exe')

    foreach ($file in $trackedFiles) {
        $basename = Split-Path -Leaf $file
        $segments = $file -split '/'
        $extension = [System.IO.Path]::GetExtension($file).ToLowerInvariant()

        if ($agentManifestBasenames -contains $basename) {
            if (-not $hasAgentTooling) {
                $violations.Add("Unacknowledged agent-tooling manifest: $file (record it under agentToolingDependencies)")
            }
            continue
        }

        if ($acknowledgedPaths -contains $file) {
            continue
        }

        $isManifest = $manifestBasenames -contains $basename
        $isConanfile = $basename -like 'conanfile.*'
        $isVendorDir = [bool]($segments | Where-Object { $vendorDirNames -contains $_.ToLowerInvariant() } | Select-Object -First 1)
        $isBinary = $binaryExtensions -contains $extension
        $isProjectFile = $extension -in @('.vcxproj', '.props', '.targets')

        if ($isManifest -or $isConanfile) {
            $violations.Add("Untracked dependency manifest: $file")
        }
        elseif ($isVendorDir) {
            $violations.Add("Untracked vendored/third-party tree: $file")
        }
        elseif ($isBinary) {
            $violations.Add("Checked-in binary (should be gitignored, or is an undeclared dependency): $file")
        }
        elseif ($isProjectFile) {
            $content = Get-Content -Raw -Path $file
            if ($content -match '<PackageReference' -or $content -match 'packages\.config') {
                $violations.Add("NuGet package reference found in $file (project uses vcpkg, not NuGet -- see docs/adr.md ADR-0007)")
            }
        }
    }

    # ---- Check 2: GitHub Actions pinning -----------------------------------------
    # Derived from $trackedFiles, not Get-ChildItem, so this stays scoped to `git
    # ls-files` like the header comment promises: an untracked scratch workflow file
    # never trips it, and .yaml (not just .yml) workflows are covered too.
    $workflowFiles = $trackedFiles | Where-Object { $_ -like '.github/workflows/*.yml' -or $_ -like '.github/workflows/*.yaml' }
    $usesPattern = [regex]'^\s*(?:-\s*)?uses:\s*([^\s#]+)'

    foreach ($workflow in $workflowFiles) {
        $workflowName = Split-Path -Leaf $workflow
        foreach ($line in Get-Content -Path $workflow) {
            $match = $usesPattern.Match($line)
            if (-not $match.Success) {
                continue
            }
            $ref = $match.Groups[1].Value.Trim()

            if ($ref.StartsWith('./')) {
                continue
            }
            if ($ref.StartsWith('docker://')) {
                $violations.Add("${workflowName}: docker:// action reference is not allowed: $ref")
                continue
            }

            $atIndex = $ref.LastIndexOf('@')
            if ($atIndex -lt 0) {
                $violations.Add("${workflowName}: action reference has no @<sha> pin: $ref")
                continue
            }

            $repoPath = $ref.Substring(0, $atIndex)
            $pin = $ref.Substring($atIndex + 1)
            $repoSegments = $repoPath -split '/'
            if ($repoSegments.Count -lt 2) {
                $violations.Add("${workflowName}: unrecognized action reference: $ref")
                continue
            }
            $ownerRepo = "$($repoSegments[0])/$($repoSegments[1])".ToLowerInvariant()

            if ($pin -notmatch '^[0-9a-f]{40}$') {
                $violations.Add("${workflowName}: '$ownerRepo' is not pinned to a full 40-character commit SHA: $ref")
            }
            if ($allowedActionRepos -notcontains $ownerRepo) {
                $violations.Add("${workflowName}: action repo '$ownerRepo' is not in .github/dependency-inventory.json's githubActions allow-list")
            }
        }
    }

    if ($violations.Count -gt 0) {
        Write-Host "::error::Dependency inventory check failed with $($violations.Count) finding(s):"
        foreach ($v in $violations) {
            Write-Host "::error::$v"
        }
        Write-Host "::error::See the cpp-msbuild skill (.github/skills/cpp-msbuild/SKILL.md, section 5) and .github/dependency-inventory.json."
        exit 1
    }

    Write-Host "Dependency inventory check passed: $($trackedFiles.Count) tracked file(s), $($workflowFiles.Count) workflow file(s), zero findings."
    exit 0
}
finally {
    Pop-Location
}
