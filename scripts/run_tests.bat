@echo off
REM Compatibility wrapper. PowerShell owns the Windows test logic.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-tests.ps1" %*
exit /b %ERRORLEVEL%
