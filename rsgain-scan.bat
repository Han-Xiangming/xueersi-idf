@echo off
rem Compatibility wrapper: the real scanner is rsgain-scan.ps1.
rem All options are forwarded, e.g. rsgain-scan.bat D:\Music -Incremental
setlocal
set "PS=powershell"
where pwsh >nul 2>nul && set "PS=pwsh"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0rsgain-scan.ps1" %*
exit /b %errorlevel%
