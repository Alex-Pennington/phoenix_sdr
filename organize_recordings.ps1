# organize_recordings.ps1
# Organizes existing .iqr and .meta files into recordings/YYYY_MM_DD folders
# Based on date from .meta files or file modification date

param(
    [switch]$WhatIf,  # Preview mode - don't actually move files
    [switch]$Force    # Move files even without .meta (use file date)
)

$ProjectRoot = $PSScriptRoot
if (-not $ProjectRoot) { $ProjectRoot = Get-Location }

Write-Host "===========================================`n" -ForegroundColor Cyan
Write-Host "Phoenix SDR - Recording Organizer" -ForegroundColor Cyan
Write-Host "===========================================`n" -ForegroundColor Cyan

# Create base recordings directory
$RecordingsDir = Join-Path $ProjectRoot "recordings"
if (-not (Test-Path $RecordingsDir)) {
    if ($WhatIf) {
        Write-Host "[WhatIf] Would create: $RecordingsDir" -ForegroundColor Yellow
    } else {
        New-Item -ItemType Directory -Path $RecordingsDir | Out-Null
        Write-Host "Created: $RecordingsDir" -ForegroundColor Green
    }
}

# Find all .iqr files in project root (not in subdirectories)
$IqrFiles = Get-ChildItem -Path $ProjectRoot -Filter "*.iqr" -File

if ($IqrFiles.Count -eq 0) {
    Write-Host "No .iqr files found in project root." -ForegroundColor Yellow
    exit 0
}

Write-Host "Found $($IqrFiles.Count) .iqr files to organize`n"

$Moved = 0
$Skipped = 0
$Errors = 0

foreach ($IqrFile in $IqrFiles) {
    $BaseName = $IqrFile.BaseName -replace "_(raw|48k)$", ""
    $MetaFile = Join-Path $ProjectRoot "$($IqrFile.BaseName).meta"
    
    $DateFolder = $null
    
    # Try to get date from .meta file
    if (Test-Path $MetaFile) {
        $MetaContent = Get-Content $MetaFile -Raw
        
        # Look for start_time_utc = 2025-12-13T...
        if ($MetaContent -match "start_time_utc\s*=\s*(\d{4})-(\d{2})-(\d{2})") {
            $Year = $Matches[1]
            $Month = $Matches[2]
            $Day = $Matches[3]
            $DateFolder = "${Year}_${Month}_${Day}"
            Write-Host "  $($IqrFile.Name): Date from meta -> $DateFolder" -ForegroundColor Gray
        }
    }
    
    # Fallback to file modification date
    if (-not $DateFolder) {
        if ($Force) {
            $FileDate = $IqrFile.LastWriteTime
            $DateFolder = $FileDate.ToString("yyyy_MM_dd")
            Write-Host "  $($IqrFile.Name): Date from file mod time -> $DateFolder" -ForegroundColor Yellow
        } else {
            Write-Host "  $($IqrFile.Name): No .meta file, skipping (use -Force to use file date)" -ForegroundColor Yellow
            $Skipped++
            continue
        }
    }
    
    # Create date folder
    $TargetDir = Join-Path $RecordingsDir $DateFolder
    if (-not (Test-Path $TargetDir)) {
        if ($WhatIf) {
            Write-Host "  [WhatIf] Would create: $TargetDir" -ForegroundColor Yellow
        } else {
            New-Item -ItemType Directory -Path $TargetDir | Out-Null
            Write-Host "  Created folder: $DateFolder" -ForegroundColor Green
        }
    }
    
    # Move .iqr file
    $TargetIqr = Join-Path $TargetDir $IqrFile.Name
    if (Test-Path $TargetIqr) {
        Write-Host "  SKIP: $($IqrFile.Name) already exists in $DateFolder" -ForegroundColor Yellow
        $Skipped++
        continue
    }
    
    if ($WhatIf) {
        Write-Host "  [WhatIf] Would move: $($IqrFile.Name) -> $DateFolder/" -ForegroundColor Cyan
    } else {
        try {
            Move-Item -Path $IqrFile.FullName -Destination $TargetIqr
            Write-Host "  Moved: $($IqrFile.Name) -> $DateFolder/" -ForegroundColor Green
            $Moved++
        } catch {
            Write-Host "  ERROR moving $($IqrFile.Name): $_" -ForegroundColor Red
            $Errors++
            continue
        }
    }
    
    # Move corresponding .meta file if it exists
    if (Test-Path $MetaFile) {
        $TargetMeta = Join-Path $TargetDir (Split-Path $MetaFile -Leaf)
        if (-not (Test-Path $TargetMeta)) {
            if ($WhatIf) {
                Write-Host "  [WhatIf] Would move: $(Split-Path $MetaFile -Leaf) -> $DateFolder/" -ForegroundColor Cyan
            } else {
                try {
                    Move-Item -Path $MetaFile -Destination $TargetMeta
                    Write-Host "  Moved: $(Split-Path $MetaFile -Leaf) -> $DateFolder/" -ForegroundColor Green
                } catch {
                    Write-Host "  ERROR moving meta file: $_" -ForegroundColor Red
                }
            }
        }
    }
}

Write-Host "`n==========================================="
Write-Host "Summary:"
Write-Host "  Moved:   $Moved files"
Write-Host "  Skipped: $Skipped files"
Write-Host "  Errors:  $Errors files"
if ($WhatIf) {
    Write-Host "`n  (WhatIf mode - no files were actually moved)" -ForegroundColor Yellow
    Write-Host "  Run without -WhatIf to perform the moves"
}
Write-Host "==========================================="
