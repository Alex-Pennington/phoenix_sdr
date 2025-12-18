# Phoenix SDR - Build and Release Guide

> ✅ **CURRENT** - Release workflow documentation is accurate

This document describes the versioning, build, and release workflow for Phoenix SDR.

## Version Format

```
MAJOR.MINOR.PATCH+BUILD.COMMIT[-dirty]
```

**Examples:**
- `0.3.0+67.abc1234` - Clean build #67 from commit abc1234
- `0.3.0+67.abc1234-dirty` - Build with uncommitted local changes

| Component | Meaning |
|-----------|---------|
| MAJOR | Breaking changes, major features |
| MINOR | New features, backward compatible |
| PATCH | Bug fixes |
| BUILD | Auto-incremented every build |
| COMMIT | Git short hash (7 chars) |
| -dirty | Uncommitted changes present |

---

## Local Development

### Building

```powershell
.\build.ps1                      # Debug build, auto-increments BUILD
.\build.ps1 -Release             # Optimized build
.\build.ps1 -Clean               # Delete build/ and bin/ directories
```

### Incrementing Version

```powershell
.\build.ps1 -Increment patch     # 0.3.0 → 0.3.1, resets BUILD to 0
.\build.ps1 -Increment minor     # 0.3.0 → 0.4.0
.\build.ps1 -Increment major     # 0.3.0 → 1.0.0
```

### Version File

The version is stored in `include/version.h` and auto-updated by the build script:

```c
#define PHOENIX_VERSION_MAJOR   0
#define PHOENIX_VERSION_MINOR   3
#define PHOENIX_VERSION_PATCH   0
#define PHOENIX_VERSION_BUILD   67
#define PHOENIX_VERSION_STRING  "0.3.0"
#define PHOENIX_VERSION_FULL    "0.3.0+67.abc1234"
#define PHOENIX_GIT_COMMIT      "abc1234"
#define PHOENIX_GIT_DIRTY       false
```

---

## Creating a Release

### Prerequisites

1. All changes committed (no dirty state)
2. Tests passing
3. On the branch you want to release from

### Release Commands

```powershell
.\deploy_release.ps1             # Increment BUILD only
.\deploy_release.ps1 -Patch      # 0.3.0 → 0.3.1 (bug fix release)
.\deploy_release.ps1 -Minor      # 0.3.0 → 0.4.0 (feature release)
.\deploy_release.ps1 -Major      # 0.3.0 → 1.0.0 (breaking changes)
.\deploy_release.ps1 -DryRun     # Preview without pushing
```

### What `deploy_release.ps1` Does

1. **Validates** - Checks for uncommitted changes (fails if dirty)
2. **Updates** - Writes new version to `include/version.h`
3. **Commits** - Creates commit with message `v0.3.1 build 1`
4. **Amends** - Re-commits with correct git hash embedded
5. **Pushes** - Pushes to origin
6. **Tags** - Creates and pushes tag `v0.3.1+1.abc1234`

The tag push triggers GitHub Actions to build and publish the release.

---

## GitHub Actions CI/CD

### Continuous Integration (`ci.yml`)

**Triggers:**
- Push to `main` or `feature/**` branches
- Pull requests to `main`

**Steps:**
1. Checkout code
2. Install MinGW (GCC compiler)
3. Download SDL2 libraries
4. Download KissFFT
5. Create SDRplay API stub headers
6. Build project
7. Run tests

**Purpose:** Verify every push compiles successfully.

### Release Build (`release.yml`)

**Triggers:**
- Push of tags matching `v*` (e.g., `v0.3.0+5.abc1234`)
- Manual workflow dispatch

**Steps:**
1. Same setup as CI
2. Build with `-Release` flag (optimizations)
3. Package release:
   - All `.exe` files
   - `SDL2.dll`
   - Documentation
4. Create zip archive
5. Upload as GitHub artifact
6. Create GitHub Release with attached zip

**Prerelease Detection:**
Tags containing `-alpha`, `-beta`, or `-rc` are marked as prereleases.

---

## Workflow Diagram

```
Local Development                    GitHub
─────────────────                    ──────

.\build.ps1                          Push to main/feature/*
    │                                       │
    ▼                                       ▼
Build #++ locally                    ci.yml runs
    │                                   │
git add & commit                        ├── Build
    │                                   └── Test
    ▼
.\deploy_release.ps1 -Patch
    │
    ├── Update version.h
    ├── Commit "v0.3.1 build 1"
    ├── Create tag v0.3.1+1.abc1234
    └── Push tag ─────────────────────► release.yml runs
                                            │
                                            ├── Build -Release
                                            ├── Package zip
                                            └── Create GitHub Release
                                                     │
                                                     ▼
                                            phoenix_sdr-v0.3.1-win64.zip
                                            available for download
```

---

## SDRplay API Note

GitHub CI builds use a **stub SDRplay API** for compilation. The resulting executables require users to install the real SDRplay API from [sdrplay.com](https://www.sdrplay.com/api/) for the software to function with SDR hardware.

---

## Quick Reference

| Task | Command |
|------|---------|
| Debug build | `.\build.ps1` |
| Release build | `.\build.ps1 -Release` |
| Clean build | `.\build.ps1 -Clean` |
| Bump patch & release | `.\deploy_release.ps1 -Patch` |
| Bump minor & release | `.\deploy_release.ps1 -Minor` |
| Preview release | `.\deploy_release.ps1 -DryRun` |
