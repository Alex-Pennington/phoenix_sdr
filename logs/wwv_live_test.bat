@echo off
REM WWV Live Sync Test - Records WWV and verifies timing against GPS
REM Phoenix Nest MARS Suite - Phase 3 Integration Test
REM
REM Usage: wwv_live_test.bat [duration_sec] [frequency_mhz]
REM   duration_sec: Recording duration (default: 180 = 3 minutes)
REM   frequency_mhz: 5, 10, 15, or 20 (default: 10)

setlocal enabledelayedexpansion

set DURATION=%1
if "%DURATION%"=="" set DURATION=180

set FREQ=%2
if "%FREQ%"=="" set FREQ=10

set CENTER_FREQ=%FREQ%000000
set TONE_FREQ=1000
if "%FREQ%"=="5" set TONE_FREQ=600

set OUTFILE=wwv_live_%FREQ%mhz.iqr
set TIMESTAMP_FILE=wwv_timestamp.txt

echo =============================================
echo WWV Live Sync Test - Phase 3 GPS Integration
echo =============================================
echo.
echo Settings:
echo   Frequency: %FREQ% MHz (center: %CENTER_FREQ% Hz)
echo   Tone freq: %TONE_FREQ% Hz
echo   Duration:  %DURATION% seconds
echo   Output:    %OUTFILE%
echo.

REM Get GPS time before recording
echo Step 1: Getting GPS time reference...
bin\gps_time.exe COM6 3 > gps_before.txt 2>&1
if errorlevel 1 (
    echo WARNING: GPS not available, using PC time
    for /f "tokens=*" %%a in ('powershell -command "Get-Date -Format 'yyyy-MM-ddTHH:mm:ss.fff'"') do set START_TIME=%%a
) else (
    REM Extract GPS time from output
    for /f "tokens=1" %%a in ('findstr /r "^>>>" gps_before.txt') do set GPS_LINE=%%a
    echo GPS synchronized
)

REM Record timestamp at start
echo Step 2: Recording start timestamp...
powershell -command "Get-Date -Format 'yyyy-MM-ddTHH:mm:ss.fff'" > %TIMESTAMP_FILE%
set /p START_TIME=<%TIMESTAMP_FILE%
echo   Start: %START_TIME%

REM Start SDR recording
echo.
echo Step 3: Recording WWV at %FREQ% MHz for %DURATION% seconds...
echo   (Recording in progress - please wait)
REM Note: Replace with actual SDR command when available
REM bin\phoenix_sdr.exe -c %CENTER_FREQ% -s 48000 -g 40 -d %DURATION% -o %OUTFILE%
echo   [SDR recording command would go here]
echo   Simulating %DURATION% second wait...
timeout /t 5 /nobreak > nul

REM Record timestamp at end
powershell -command "Get-Date -Format 'yyyy-MM-ddTHH:mm:ss.fff'" >> %TIMESTAMP_FILE%
echo   Recording complete.

REM Check if recording exists
if not exist %OUTFILE% (
    echo ERROR: Recording file not created
    echo This script requires SDR hardware to be connected
    echo For manual testing, use existing recordings:
    echo   bin\wwv_sync.exe wwv_600_48k.iqr 0 300
    goto :end
)

REM Analyze with wwv_sync
echo.
echo Step 4: Analyzing WWV signal...
if "%FREQ%"=="5" (
    bin\wwv_sync.exe %OUTFILE% 0 %DURATION% -f 600 > wwv_analysis.txt
) else (
    bin\wwv_sync.exe %OUTFILE% 0 %DURATION% > wwv_analysis.txt
)

REM Extract offset from analysis
for /f "tokens=7" %%a in ('findstr /c:"Offset from file start" wwv_analysis.txt') do set WWV_OFFSET=%%a
echo   WWV offset: %WWV_OFFSET% ms

REM Verify with GPS
echo.
echo Step 5: Verifying against GPS reference...
bin\wwv_gps_verify.exe -t %START_TIME% -o %WWV_OFFSET% -p COM6

:end
echo.
echo =============================================
echo Test complete. Files created:
echo   %TIMESTAMP_FILE% - Recording timestamps
echo   gps_before.txt  - GPS reference before recording
echo   wwv_analysis.txt - WWV sync analysis
echo =============================================

endlocal
