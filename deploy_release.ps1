# Phoenix SDR - Deploy Release Script
# Pushes a release to GitHub via the release workflow
#
# Usage:
#   .\deploy_release.ps1              # Increment build only
#   .\deploy_release.ps1 -Patch       # Increment patch version
#   .\deploy_release.ps1 -Minor       # Increment minor version
#   .\deploy_release.ps1 -Major       # Increment major version
#   .\deploy_release.ps1 -DryRun      # Show what would happen without pushing

param(
    [switch]$Major,
    [switch]$Minor,
    [switch]$Patch,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$VersionFile = "include\version.h"

function Write-Status($msg) {
    Write-Host "[deploy] " -ForegroundColor Cyan -NoNewline
    Write-Host $msg
}

function Write-Error($msg) {
    Write-Host "[deploy] " -ForegroundColor Red -NoNewline
    Write-Host $msg
}

# Check we're in the right directory
if (-not (Test-Path $VersionFile)) {
    Write-Error "Cannot find $VersionFile - run from project root"
    exit 1
}

# Check for uncommitted changes (other than version.h)
$status = git status --porcelain | Where-Object { $_ -notmatch "version.h" }
if ($status) {
    Write-Error "Uncommitted changes detected. Commit or stash before releasing."
    git status --short
    exit 1
}

# Read current version
$content = Get-Content $VersionFile -Raw
$major = 0; $minor = 0; $patch = 0; $build = 0

if ($content -match 'PHOENIX_VERSION_MAJOR\s+(\d+)') { $major = [int]$matches[1] }
if ($content -match 'PHOENIX_VERSION_MINOR\s+(\d+)') { $minor = [int]$matches[1] }
if ($content -match 'PHOENIX_VERSION_PATCH\s+(\d+)') { $patch = [int]$matches[1] }
if ($content -match 'PHOENIX_VERSION_BUILD\s+(\d+)') { $build = [int]$matches[1] }

$oldVersion = "$major.$minor.$patch+$build"
Write-Status "Current version: $oldVersion"

# Increment version
if ($Major) {
    $major++
    $minor = 0
    $patch = 0
    $build = 0
    Write-Status "Incrementing MAJOR version"
} elseif ($Minor) {
    $minor++
    $patch = 0
    $build = 0
    Write-Status "Incrementing MINOR version"
} elseif ($Patch) {
    $patch++
    $build = 0
    Write-Status "Incrementing PATCH version"
}

# Always increment build
$build++

$versionString = "$major.$minor.$patch"

Write-Status "New version: $versionString+$build"

if ($DryRun) {
    Write-Status "[DRY RUN] Would update version.h, commit, tag, and push"
    Write-Status "[DRY RUN] Tag would be: v$versionString+$build.<commit>"
    exit 0
}

# Get current commit (before our changes)
$baseCommit = (git rev-parse --short HEAD).Trim()

# Update version.h with placeholder commit (will fix after commit)
$newContent = @"
/**
 * @file version.h
 * @brief Phoenix SDR version information
 * 
 * Version format: MAJOR.MINOR.PATCH+BUILD.COMMIT[-dirty]
 * Example: 0.2.5+9.abc1234 or 0.2.5+9.abc1234-dirty
 * 
 * Build number increments every build. Commit hash from git.
 */

#ifndef PHOENIX_VERSION_H
#define PHOENIX_VERSION_H

#define PHOENIX_VERSION_MAJOR   $major
#define PHOENIX_VERSION_MINOR   $minor
#define PHOENIX_VERSION_PATCH   $patch
#define PHOENIX_VERSION_BUILD   $build
#define PHOENIX_VERSION_STRING  "$versionString"
#define PHOENIX_VERSION_FULL    "$versionString+$build.PENDING"
#define PHOENIX_GIT_COMMIT      "PENDING"
#define PHOENIX_GIT_DIRTY       false

/* Build timestamp - set by compiler */
#define PHOENIX_BUILD_DATE      __DATE__
#define PHOENIX_BUILD_TIME      __TIME__

#include <stdio.h>

static inline void print_version(const char *tool_name) {
    printf("%s v%s (built %s %s)\n", 
           tool_name, PHOENIX_VERSION_FULL, 
           PHOENIX_BUILD_DATE, PHOENIX_BUILD_TIME);
}

#endif /* PHOENIX_VERSION_H */
"@

Set-Content -Path $VersionFile -Value $newContent -NoNewline
Write-Status "Updated version.h"

# Commit
git add $VersionFile
git commit -m "v$versionString build $build"
Write-Status "Committed"

# Get the actual commit hash
$commit = (git rev-parse --short HEAD).Trim()

# Update version.h with real commit hash
$content = Get-Content $VersionFile -Raw
$content = $content -replace 'PHOENIX_VERSION_FULL\s+"[^"]+"', "PHOENIX_VERSION_FULL    `"$versionString+$build.$commit`""
$content = $content -replace 'PHOENIX_GIT_COMMIT\s+"[^"]+"', "PHOENIX_GIT_COMMIT      `"$commit`""
Set-Content -Path $VersionFile -Value $content -NoNewline

# Amend commit with correct hash
git add $VersionFile
git commit --amend --no-edit
Write-Status "Amended with correct commit hash"

# Push
git push --force-with-lease
Write-Status "Pushed to origin"

# Get final commit hash (may have changed after amend)
$finalCommit = (git rev-parse --short HEAD).Trim()

# Create and push tag
$tag = "v$versionString+$build.$finalCommit"
git tag $tag -m $tag
git push origin $tag
Write-Status "Created and pushed tag: $tag"

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host " RELEASE DEPLOYED: $tag" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "GitHub Actions will now build and create the release."
Write-Host "Check: https://github.com/Alex-Pennington/phoenix_sdr/actions"
