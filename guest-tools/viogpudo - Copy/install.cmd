@echo off
rem 2ksbox's experimental display driver (track M24) from the drivers
rem disc's 2ksbox\experimental-display. Asks for administrator rights,
rem then runs viogpudo-install.ps1 beside it, which warns twice first.
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0viogpudo-install.ps1"
pause
