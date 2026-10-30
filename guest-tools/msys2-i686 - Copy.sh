# Sourced by the guest-tools build scripts (build-wrappers.sh,
# build-driver.sh, build-driver9x.sh) after they set ROOT. On Linux and
# macOS it does nothing. In MSYS2 on Windows (docs/build-windows.md,
# "Building on Windows") it makes the shell the one those scripts and
# qemu-3dfx's own build expect:
#
# - MSYSTEM=MINGW32. qemu-3dfx's conf_wrapper builds the guest wrappers
#   natively only there (it asks `uname`, which MSYS2 derives from MSYSTEM,
#   and `gcc -v` for an i686 target), and its Makefiles refuse any other.
# - MSYS2's i686 toolchain (/mingw32/bin) first on PATH, so plain `gcc`,
#   `objdump` and `windres` are the guest's. The x86_64 tools behind it
#   still answer for what the guest build runs on the host: python3,
#   gendef.
# - The target-prefixed names the scripts call (i686-w64-mingw32-objdump,
#   -nm, -ar, -windres ...), which MSYS2's binutils does not install: shims
#   in guest-tools/tools/msys2-bin. gcc has its prefixed name already.
# - perl's core_perl on PATH, for qemu-3dfx's `shasum`.
# - Linux's i686 runtime instead of MSYS2's. MSYS2 builds its 32-bit
#   runtime for the Pentium 4: libmingwex's printf and dtoa, libgcc's
#   double-to-unsigned conversion, msvcrt's stat and time helpers and
#   libwinpthread all contain SSE2, and a guest program linking them faults
#   on a Pentium III (check_isa: "ANISTEST.EXE contains 11 SSE2+/POPCNT
#   instructions"; the cross toolchain in MSYS2's msys repo is built the
#   same way). So the i686 gcc here links the runtime the Linux ISO links
#   (Arch's mingw-w64-crt, -winpthreads and gcc's libgcc, pinned below by
#   version and sha256) through -B and -L, which put its
#   crt2.o, libmingwex, libgcc and the rest ahead of MSYS2's (checked on
#   Linux: the same link takes every one of them from the given folders).
#   Headers stay MSYS2's own. MSYS2's gcc must be the libgcc's version.
#
# Idempotent: build-wrappers.sh runs the other two, which source it again.

case "${MSYSTEM:-}" in
  "") ;;
  MINGW64|MINGW32)
    if [ ! -x /mingw32/bin/i686-w64-mingw32-gcc.exe ]; then
      echo "no MSYS2 i686 toolchain (/mingw32/bin/i686-w64-mingw32-gcc): scripts/build-windows.sh --msys2-deps" >&2
      exit 1
    fi
    export MSYSTEM=MINGW32
    for _t in curl sha256sum zstd; do
      command -v "$_t" >/dev/null || { echo "not found: $_t -- scripts/build-windows.sh --msys2-deps" >&2; exit 1; }
    done

    _rt_gcc=16.2.0
    _rt="$ROOT/guest-tools/tools/i686-runtime/gcc-$_rt_gcc-crt-14.0.0"
    if [ "$(/mingw32/bin/gcc -dumpversion)" != "$_rt_gcc" ]; then
      echo "MSYS2's i686 gcc is $(/mingw32/bin/gcc -dumpversion), and the Pentium III-safe runtime pinned in" >&2
      echo "guest-tools/msys2-i686.sh is gcc $_rt_gcc's: pin the matching Arch packages there" >&2
      exit 1
    fi
    if [ ! -f "$_rt/.complete" ]; then
      echo "==> the i686 runtime the Linux ISO links (Arch's mingw-w64 packages, once)" >&2
      rm -rf "$_rt" && mkdir -p "$_rt/dl" "$_rt/x" "$_rt/lib" "$_rt/gcc"
      while read -r _pkg _sum; do
        _f="$_rt/dl/${_pkg#*/}.pkg.tar.zst"
        curl -fsSL -o "$_f" "https://archive.archlinux.org/packages/m/$_pkg.pkg.tar.zst" \
          || { echo "could not download $_pkg from archive.archlinux.org" >&2; exit 1; }
        echo "$_sum  $_f" | sha256sum -c --quiet - \
          || { echo "$_pkg: checksum mismatch" >&2; exit 1; }
      done <<'PKGS'
mingw-w64-crt/mingw-w64-crt-14.0.0-1-any a809d9b6608235e5b693f1ebda97aa6c3539783c84509cb8db3306ef56ad0a4f
mingw-w64-winpthreads/mingw-w64-winpthreads-14.0.0-1-any 21a4dd95e8a5f0b802d8585e59024f2cfbbd435cee31ed31b511159d9d10579b
mingw-w64-gcc/mingw-w64-gcc-16.2.0-2-x86_64 9fde71b655105ed8704fb1494a39f3b94d0a138df9dface881588e6cef4886f1
PKGS
      tar --zstd -C "$_rt/x" -xf "$_rt/dl/mingw-w64-crt-14.0.0-1-any.pkg.tar.zst" usr/i686-w64-mingw32/lib
      tar --zstd -C "$_rt/x" -xf "$_rt/dl/mingw-w64-winpthreads-14.0.0-1-any.pkg.tar.zst" usr/i686-w64-mingw32/lib
      _g=usr/lib/gcc/i686-w64-mingw32/$_rt_gcc
      tar --zstd -C "$_rt/x" -xf "$_rt/dl/mingw-w64-gcc-16.2.0-2-x86_64.pkg.tar.zst" \
        "$_g/libgcc.a" "$_g/libgcc_eh.a" "$_g/crtbegin.o" "$_g/crtend.o"
      cp -r "$_rt/x/usr/i686-w64-mingw32/lib/." "$_rt/lib/"
      cp "$_rt/x/$_g/"* "$_rt/gcc/"
      rm -rf "$_rt/dl" "$_rt/x"
      touch "$_rt/.complete"
    fi

    _msys2_shims="$ROOT/guest-tools/tools/msys2-bin"
    mkdir -p "$_msys2_shims"
    for _t in objdump nm ar ranlib windres dlltool strip; do
      printf '#!/bin/sh\nexec /mingw32/bin/%s "$@"\n' "$_t" > "$_msys2_shims/i686-w64-mingw32-$_t"
      chmod +x "$_msys2_shims/i686-w64-mingw32-$_t"
    done
    # the compilers, both names conf_wrapper and our scripts use, linking the
    # runtime above (-B for crt2.o / crtbegin.o, -L for the libraries). The
    # -L folders go *after* the caller's arguments: ld searches -L in command
    # line order, and a build that names its own library folder must win
    # (the wine9x build, since retired, lost its own libpthread.a to Arch's
    # winpthreads that way: "undefined reference to _msize_int"). Any -L
    # still precedes gcc's own.
    for _t in gcc i686-w64-mingw32-gcc; do
      printf '#!/bin/sh\nexec /mingw32/bin/%s -B"%s/gcc/" -B"%s/lib/" "$@" -L"%s/gcc" -L"%s/lib"\n' \
        "$_t" "$_rt" "$_rt" "$_rt" "$_rt" > "$_msys2_shims/$_t"
      chmod +x "$_msys2_shims/$_t"
    done
    case ":$PATH:" in
      *":$_msys2_shims:"*) ;;
      *) export PATH="$_msys2_shims:/mingw32/bin:$PATH:/usr/bin/core_perl" ;;
    esac
    _msys2_missing=""
    for _t in make which xxd shasum nasm xorriso gendef python3; do
      command -v "$_t" >/dev/null || _msys2_missing="$_msys2_missing $_t"
    done
    if [ -n "$_msys2_missing" ]; then
      echo "not found:$_msys2_missing -- scripts/build-windows.sh --msys2-deps" >&2
      exit 1
    fi
    unset _t _msys2_missing _rt _rt_gcc _pkg _sum _f _g
    ;;
  *)
    echo "MSYS2 $MSYSTEM shell: build from the MINGW64 one (docs/build-windows.md)" >&2
    exit 1
    ;;
esac
