@echo off
REM Compatibility wrapper. PowerShell owns the Windows build logic.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-mcp.ps1" %*
exit /b %ERRORLEVEL%
