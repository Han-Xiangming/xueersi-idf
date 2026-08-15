@echo off
title rsgain ReplayGain Scan
setlocal

rem ============================================================
rem  rsgain-scan.bat - scan & tag ReplayGain 2.0 (ID3v2.3 TXXX)
rem
rem  Card layout (category folders, NOT album folders):
rem    Music
rem      +- Chinese
rem      |    +- xxx.mp3
rem      +- English
rem           +- xxx.mp3
rem
rem  Always uses -p no_album (track gain only): the player only
rem  reads track gain; album gain is meaningless here.
rem  (easy mode writes ID3v2.3/v2.4 TXXX or RVA2 - the player reads all.)
rem
rem  Usage: rsgain-scan.bat [music folder]
rem         (no argument = interactive prompt)
rem ============================================================

if "%~1"=="" (
    set /p MUSIC_DIR=Enter music folder path, e.g. D:\Music:
    if not defined MUSIC_DIR (
        echo [ERROR] no path given
        pause
        exit /b 1
    )
) else (
    set "MUSIC_DIR=%~1"
)

if not exist "%MUSIC_DIR%" (
    echo [ERROR] folder not found: "%MUSIC_DIR%"
    pause
    exit /b 1
)

set "RSGAIN=E:\tools\rsgain-3.7-win64\rsgain.exe"
if exist "%RSGAIN%" (
    set "RUN_RSGAIN=%RSGAIN%"
) else (
    where rsgain >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] rsgain not found
        echo checked: %RSGAIN%
        echo install via scoop: scoop install extras/rsgain
        echo or download: https://github.com/complexlogic/rsgain/releases
        pause
        exit /b 1
    )
    set "RUN_RSGAIN=rsgain"
)

echo ============================================================
echo   Scan folder : %MUSIC_DIR%
echo   Layout      : category folders -^> track gain only (no_album)
echo   Tags        : ReplayGain 2.0, -18 LUFS target
echo   Incremental : -S skips files that already have ReplayGain
echo ============================================================
echo.

"%RUN_RSGAIN%" easy -p no_album -S -m MAX "%MUSIC_DIR%"

echo.
echo Done. You can put the card back into the player.
pause