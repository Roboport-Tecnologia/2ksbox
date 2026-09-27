@echo off
rem FIFA 2000 on the d3dpt-vga Direct3D HAL (doc 15, M7c), run headless by
rem   GAME_ISO=FIFA2000.ISO SHOTS=24 tools/xp-driver-test.sh <image> bat tools/xp-fifa2000.bat
rem The game folder on that image still carries the WineD3D-in-guest set
rem from the 2026-09-03 experiments (a stack retired by ADR-018, M15 step 6:
rem DDRAW.DLL / WINED3D.DLL / WINEDD.DLL next to the EXE and in THRASH\).
rem They are renamed away so the game loads the system ddraw.dll -> our HAL.
rem The install path is the Brazilian XP's; the registry dump shows the
rem renderer choice (Thrash Driver dx = THRASH\dx6z.dll, Hardware Acceleration 1).
reg query "HKLM\Software\EA Sports\FIFA 2000" /s > E:\fifareg.txt
cd /d "C:\Arquivos de programas\EA SPORTS\FIFA 2000"
ren DDRAW.DLL DDRAW.WINE
ren WINED3D.DLL WINED3D.WINE
ren WINEDD.DLL WINEDD.WINE
cd THRASH
ren DDRAW.DLL DDRAW.WINE
ren WINED3D.DLL WINED3D.WINE
ren WINEDD.DLL WINEDD.WINE
cd ..
rem a DINPUT.DLL on E:\ (the disc's DINPUT\ shim) goes next to the EXE: it merges what
rem Windows reports pressed into the state, which is what makes the match take
rem keys (doc 15). Silent unless E:\DILOG is staged, then it also writes
rem dinput_log.txt with what the game asks DirectInput for and what it gets.
if exist E:\DINPUT.DLL copy /y E:\DINPUT.DLL . > nul
if exist E:\DINPUT.DLL copy /y E:\DINPUT.DLL THRASH > nul
if exist E:\DILOG set D3DPT_DINPUT_LOG=1
dir /b > E:\fifadir.txt
dir /b THRASH >> E:\fifadir.txt
start /wait fifa2000.exe
echo exit %errorlevel% > E:\fifaexit.txt
