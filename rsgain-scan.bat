@echo off
rem Compatibility wrapper: the real scanner is rsgain-scan.ps1.
rem All options are forwarded, e.g. rsgain-scan.bat D:\Music -Mode Smart
rem Modes: -Mode Full (default), -Mode Smart, -Mode Incremental.
rem Double-click with no arguments prompts for the folder and a mode menu.
rem Window handling lives in the .ps1: when it detects it was started by
rem double-clicking from Explorer it stops at a "Press Enter to close" prompt.
setlocal
set "PS=powershell"
where pwsh >nul 2>nul && set "PS=pwsh"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0rsgain-scan.ps1" %*
exit /b %errorlevel%
