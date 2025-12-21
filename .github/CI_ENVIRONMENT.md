# CI Environment Setup

## GitHub Actions Environment

The CI uses:
- **OS:** `windows-latest` (Windows Server 2022)
- **Compiler:** MinGW-w64 GCC **12.2.0** (x86_64-posix-seh)
- **SDL2:** 2.30.9 (downloaded fresh)
- **SDRplay:** Stub headers only (`.github/sdrplay_api_stub.h`)

## Local CI Simulation

To test builds exactly as GitHub Actions will:

### 1. Install Matching GCC (One-time setup)

**Option A - Chocolatey:**
```powershell
choco install mingw --version=12.2.0 --force
```

**Option B - Manual Download:**
1. Go to https://github.com/niXman/mingw-builds-binaries/releases
2. Download: `x86_64-12.2.0-release-posix-seh-msvcrt-rt_v10-rev2.7z`
3. Extract to `C:\mingw64-12.2.0\`
4. Add to PATH: `C:\mingw64-12.2.0\bin`

**Verify:**
```powershell
gcc --version  # Should show 12.2.0
```

### 2. Run CI Build Locally

```powershell
# Test exactly as CI will build
.\test_ci_local.ps1 -Build

# Clean up CI artifacts (restore real SDK headers)
.\test_ci_local.ps1 -Clean
```

### 3. Development Workflow

```powershell
# Before pushing to GitHub:
.\test_ci_local.ps1 -Build    # Verify CI will succeed

# After CI passes:
.\test_ci_local.ps1 -Clean    # Restore local development environment
.\build.ps1                    # Build with real SDRplay SDK
```

## Differences from Local Build

| Aspect | Local (`build.ps1`) | CI (`ci_build.ps1`) |
|--------|---------------------|---------------------|
| GCC Version | 15.2.0 (WinLibs) | **12.2.0** |
| SDRplay Headers | Real SDK | **Stub only** |
| SDL2 | `libs/SDL2/` | Downloaded fresh |
| Tools Built | All | 8 executables |
| Can Run Executables | ✅ Yes | ❌ No (stub) |

## Why Matching Versions Matter

- **GCC 15.2 vs 12.2:** Newer compiler may accept code that older rejects
- **Real vs Stub SDK:** Real SDK has all symbols, stub is minimal
- **Testing with CI script:** Catches linking issues before push

## Rate Limiting Concerns

GitHub Actions limits workflow runs. Test locally first to avoid:
- ❌ Multiple failed builds consuming quota
- ❌ Delayed feedback cycle (wait for CI to finish)
- ✅ Instant local feedback
- ✅ Guaranteed CI success before push
