@echo off
rem Windows entry point: runs build.ps1 without needing to change PowerShell's execution policy.
rem Usage: build.cmd "C:\path\to\extracted\game"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set RC=%ERRORLEVEL%
rem Keep the window open when started by double-click (no arguments).
if "%~1"=="" pause
exit /b %RC%
