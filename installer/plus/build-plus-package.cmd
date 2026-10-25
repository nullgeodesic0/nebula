@echo off
rem Runs build-plus-package.ps1 (PowerShell scripts may be blocked from running directly); arguments pass through.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-plus-package.ps1" %*
exit /b %errorlevel%
