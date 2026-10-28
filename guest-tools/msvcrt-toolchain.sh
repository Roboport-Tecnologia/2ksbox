#!/usr/bin/env bash
# Sourced by the Windows guest build scripts. Select a classic MSVCRT
# toolchain without assuming that every MinGW-w64 GCC knows -mcrtdll.
#
# Sets MSVCRT_CRT_FLAG (a Bash array, possibly empty). The test executable
# must import msvcrt.dll; simply accepting a compiler flag is not enough.
select_msvcrt_crt() {
  local cc="$1" tmp imports
  local -a mode=()
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/2ksbox-msvcrt.XXXXXX")" || return 1

  if printf 'int main(void){return 0;}\n' | "$cc" -x c - -mcrtdll=msvcrt-os -o "$tmp/probe.exe" >/dev/null 2>&1; then
    mode=(-mcrtdll=msvcrt-os)
  elif ! printf 'int main(void){return 0;}\n' | "$cc" -x c - -o "$tmp/probe.exe" >/dev/null 2>&1; then
    echo "ERROR: cannot compile an MSVCRT probe with $cc" >&2
    rm -rf "$tmp"
    return 1
  fi

  if ! imports="$(i686-w64-mingw32-objdump -p "$tmp/probe.exe" 2>&1)"; then
    echo "ERROR: cannot inspect MinGW-w64 probe imports: $imports" >&2
    rm -rf "$tmp"
    return 1
  fi
  rm -rf "$tmp"

  if ! printf '%s\n' "$imports" | grep -Eiq 'DLL Name:[[:space:]]*msvcrt[.]dll([[:space:]]|$)' ||
     printf '%s\n' "$imports" | grep -Eiq 'DLL Name:[[:space:]]*(ucrtbase[.]dll|api-ms-win-crt-)'; then
    echo "ERROR: $cc does not produce a classic MSVCRT-only Windows executable" >&2
    echo "       Install/select an MSVCRT-targeting MinGW-w64 toolchain." >&2
    return 1
  fi

  MSVCRT_CRT_FLAG=("${mode[@]}")
}
