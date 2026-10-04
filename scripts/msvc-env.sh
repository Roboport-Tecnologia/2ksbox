# Sourced, in MSYS2's MINGW64 shell on Windows: puts Visual Studio's x64
# C/C++ tools (cl, link, rc, the Windows SDK) on PATH with their INCLUDE,
# LIB and LIBPATH, as a "x64 Native Tools" prompt would (ADR-026). For the
# MSVC builds that are not cargo's, which finds Visual Studio itself: DXVK
# (scripts/configure-dxvk.sh --windows) and the Direct3D executor
# (scripts/build-d3dpt-exec.sh --windows).
#
#   . scripts/msvc-env.sh || exit 1
#
# vswhere finds the newest Visual Studio with the C++ tools, its
# vcvars64.bat is run once in cmd, and the environment it leaves is taken
# over. Visual Studio's directories go first on PATH, because MSYS2's
# /usr/bin has a `link` of its own (coreutils', "link: extra operand").
# Idempotent: a second source in the same shell does nothing.

msvc_env() {
  [ -n "${_2KSBOX_MSVC_ENV:-}" ] && return 0
  local vswhere vs bat out
  # Program Files (x86) by its folder id: a shell started with a stripped
  # environment has no ProgramFiles(x86)
  vswhere="$(cygpath -F 42)/Microsoft Visual Studio/Installer/vswhere.exe"
  [ -x "$vswhere" ] || vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  if [ ! -x "$vswhere" ]; then
    echo "msvc-env.sh: no vswhere.exe: install Visual Studio (or its Build Tools) with the C++ desktop workload (docs/build-windows.md)" >&2
    return 1
  fi
  vs="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')"
  bat="$vs\\VC\\Auxiliary\\Build\\vcvars64.bat"
  if [ -z "$vs" ] || [ ! -f "$(cygpath -u "$bat")" ]; then
    echo "msvc-env.sh: no Visual Studio with the x64 C++ tools (vswhere found '${vs:-nothing}')" >&2
    return 1
  fi
  # cmd's own quoting through MSYS2's argument conversion is fragile, so
  # the two commands go through a batch file
  local tmp; tmp="$(mktemp -d)"
  printf '@call "%s" >nul 2>&1\r\n@set\r\n' "$bat" > "$tmp/env.bat"
  out="$(cmd //c "$(cygpath -w "$tmp/env.bat")" < /dev/null | tr -d '\r')"
  rm -rf "$tmp"
  local line name value got=""
  while IFS= read -r line; do
    name="${line%%=*}"; value="${line#*=}"
    case "$name" in
      Path|PATH)
        # the compiler's and the SDK's directories, ahead of everything
        # already here (not the IDE's: its CMake ninja would shadow MSYS2's)
        local d add=""
        IFS=';' read -ra parts <<< "$value"
        for d in "${parts[@]}"; do
          d="$(cygpath -u "$d")"
          case "$d" in */VC/Tools/*|*"/Windows Kits/"*|*"/Microsoft SDKs/"*) add="${add:+$add:}$d" ;; esac
        done
        export PATH="$add:$PATH" ;;
      INCLUDE|LIB|LIBPATH|VCINSTALLDIR|VCToolsInstallDir|VCToolsVersion|VSINSTALLDIR|VisualStudioVersion|\
      WindowsSdkDir|WindowsSdkBinPath|WindowsSdkVerBinPath|WindowsSDKVersion|WindowsSDKLibVersion|\
      UniversalCRTSdkDir|UCRTVersion|VSCMD_ARG_HOST_ARCH|VSCMD_ARG_TGT_ARCH|Platform)
        export "$name=$value"; got=1 ;;
    esac
  done <<< "$out"
  if [ -z "$got" ] || ! command -v cl >/dev/null; then
    echo "msvc-env.sh: $bat left no cl on PATH" >&2
    return 1
  fi
  export _2KSBOX_MSVC_ENV=1
}
msvc_env
