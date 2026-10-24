@echo off
rem Runs publish-plus-release.ps1 (PowerShell scripts may be blocked from running directly); arguments pass through.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish-plus-release.ps1" %*
exit /b %errorlevel%
