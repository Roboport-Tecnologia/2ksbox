@echo off
rem guest-tools\build-wddm.cmd: the WDDM display driver for Windows 7
rem (M18 step 2, ADR-022) into build\wddm\x86: d3dptkmd.sys and its INF.
rem
rem It builds with the Enterprise WDK for Windows 10 2004 (10.0.19041, VS
rem 2019 Build Tools 16.7): the last kit that targets Windows 7 and still
rem builds 32-bit kernel drivers. The EWDK is one ISO, mounted, nothing
rem installed. docs/build-windows.md "The WDDM driver" has where to get it.
rem
rem   guest-tools\build-wddm.cmd               finds a mounted EWDK
rem   set EWDK=E:                              ... or names its drive
rem   set EWDK_ISO=D:\...\EWDK_vb_...iso       ... or mounts the ISO first
rem
rem From MSYS2 or Git Bash: cmd //c guest-tools\\build-wddm.cmd
setlocal
set "ROOT=%~dp0.."
for %%r in ("%ROOT%") do set "ROOT=%%~fr"
set "OUT=%ROOT%\build\wddm\x86"

if not defined EWDK call :find_ewdk
if not defined EWDK if defined EWDK_ISO (
  echo ==^> mounting %EWDK_ISO%
  powershell -NoProfile -Command "Mount-DiskImage -ImagePath '%EWDK_ISO%' | Out-Null"
  call :find_ewdk
)
if not defined EWDK (
  echo build-wddm: no EWDK 10.0.19041 mounted; mount the ISO or set EWDK_ISO ^(docs/build-windows.md^) 1>&2
  exit /b 1
)
echo ==^> EWDK at %EWDK%

rem SetupBuildEnv refuses a shell that already ran it
set WindowsSystemKit=
call "%EWDK%\BuildEnv\SetupBuildEnv.cmd" x86 >nul 2>nul
@echo off
if not defined WDKContentRoot exit /b 1

if not exist "%OUT%" mkdir "%OUT%"
msbuild "%ROOT%\guest-tools\src\d3dptvid\wddm\km\d3dptkmd.vcxproj" -nologo -v:minimal ^
  -p:Configuration=Release -p:Platform=Win32 "-p:D3DPT_OUT=%OUT%"
if errorlevel 1 exit /b 1
msbuild "%ROOT%\guest-tools\src\d3dptvid\wddm\um\d3dptumd.vcxproj" -nologo -v:minimal ^
  -p:Configuration=Release -p:Platform=Win32 "-p:D3DPT_OUT=%OUT%"
if errorlevel 1 exit /b 1
copy /y "%ROOT%\guest-tools\src\d3dptvid\wddm\km\d3dptkmd.inf" "%OUT%\" >nul
echo ==^> %OUT%\d3dptkmd.sys, %OUT%\d3dptumd.dll
exit /b 0

:find_ewdk
for %%d in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
  if exist "%%d:\BuildEnv\SetupBuildEnv.cmd" if exist "%%d:\Program Files\Windows Kits\10\Include\10.0.19041.0\km\dispmprt.h" set "EWDK=%%d:"
)
exit /b 0
