@echo off
title Clear MP3 cover art
setlocal
rem Launcher for clear-cover.ps1 (PowerShell handles Unicode filenames,
rem e.g. Japanese track names; a pure cmd script would mangle them).
rem Usage: clear-cover.bat [music folder]

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

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clear-cover.ps1" "%MUSIC_DIR%"
if errorlevel 1 (
    echo [ERROR] script failed
    pause
    exit /b 1
)
pause