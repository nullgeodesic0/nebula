@echo off
rem Runs new-plus-release-key.ps1 (PowerShell scripts may be blocked from running directly); arguments pass through.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0new-plus-release-key.ps1" %*
exit /b %errorlevel%
