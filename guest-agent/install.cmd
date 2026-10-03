@echo off
rem 2ksbox's guest tools for Windows 11: the virtio drivers, and the agent
rem that shares the clipboard with the host and maps its shared folder.
rem Asks for administrator rights, then runs install.ps1 beside it.
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
pause
