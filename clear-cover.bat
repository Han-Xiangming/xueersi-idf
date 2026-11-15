@echo off
title Clear MP3 cover art
setlocal
rem Launcher for clear-cover.ps1 (PowerShell handles Unicode filenames,
rem e.g. Japanese track names; a pure cmd script would mangle them).
rem Usage: clear-cover.bat [music folder]
rem The .ps1 keeps the window open when it detects it was started by
rem double-clicking from Explorer, so there is no pause here.

if "%~1"=="" (
    set /p MUSIC_DIR=Enter music folder path, e.g. D:\Music:
    if not defined MUSIC_DIR (
        echo [ERROR] no path given
        exit /b 1
    )
) else (
    set "MUSIC_DIR=%~1"
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clear-cover.ps1" "%MUSIC_DIR%"
exit /b %errorlevel%
