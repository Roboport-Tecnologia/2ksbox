@echo off
rem Max Payne (D3D8) on the d3dpt-vga Direct3D HAL (doc 15, M7c) with no
rem wrapper DLL in the game folder: XP's own d3d8.dll on top of our DX7-level
rem DDI. Run headless by
rem   GAME_ISO=DINO-MAP.iso CPU=pentium3 SHOTS=30 SHOT_KEYS="2:ret,6:ret" tools/xp-driver-test.sh <image> bat tools/xp-maxpayne.bat
rem (CPU=pentium3: under KVM -cpu host the game's CPUID-dispatched JPEG decoder
rem mis-decodes its level data, see CLAUDE.md). A D3D8.DLL left next to the
rem EXE by the M4 track (a wrapper retired in M16 step 7) is renamed away.
rem xp-driver-test.sh puts the game disc on D: (the image's cd.ini may say E:
rem from an xp-game-test.sh run, whose discs come up in reverse order).
cd /d "C:\Arquivos de programas\Max Payne"
if exist D3D8.DLL ren D3D8.DLL D3D8.PT
if exist DDRAW.DLL ren DDRAW.DLL DDRAW.PT
echo D:\disk1\Levels> cd.ini
dir /b > E:\mpdir.txt
start /wait MaxPayne.exe
echo exit %errorlevel% > E:\mpexit.txt
