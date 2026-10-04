#!/usr/bin/env bash
# Build the Windows artefacts on Windows, in MSYS2's MINGW64 shell, in
# dependency order (ADR-026). This is the Windows counterpart of
# scripts/build.sh, which builds for the host it runs on. QEMU and the Rust
# that links into it are mingw (MSYS2's, its msvcrt and libstdc++), and
# the winit player test.sh runs; the launcher, player-mitsuami, the
# Direct3D executor, DXVK and the tools are MSVC; the
# guest-tools ISO is MSYS2's i686 toolchain (guest-tools/msys2-i686.sh).
# The cross build from Linux (scripts/win-cross.sh and its container) was
# retired on 2026-10-03. The package comes from here too
# (scripts/package-windows.sh, checked on Windows itself). Run what was
# built with scripts/win-run.sh.
#
#   scripts/build-windows.sh                everything this host can build
#   scripts/build-windows.sh qemu rust      only those stages
#   scripts/build-windows.sh --package      ... and then roll the zip
#   scripts/build-windows.sh wddm --publish the WDDM driver, then publish it for
#                                           Linux and macOS (gh, logged in)
#   scripts/build-windows.sh -f qemu        prepare QEMU's tree even if its stamp
#                                           says nothing changed
#   scripts/build-windows.sh --msys2-deps   (Windows) install what the build needs
#
# Stages, in the order they must run:
#
#   qemu    configure-qemu.sh --windows -> ninja: qemu-system-i386.exe,
#           qemu-img.exe, qemu-io.exe, libqemu-embed-i386.dll, into
#           build/win/qemu (with libdisc built for windows-gnu first)
#   rust    the winit player (test.sh's), cargo --target
#           x86_64-pc-windows-gnu on QEMU's ABI, and the tools launcherx,
#           discx and synthx with MSVC (scripts/cargo-msvc.sh, into
#           target/x86_64-pc-windows-msvc). Runs after `qemu`, because
#           the player links the embed DLL from build/win/qemu.
#   mitsuami (Windows only) cargo build --release in launcher-mitsuami/
#           (its own workspace): the launcher every package ships
#           (ADR-023), on WinUI 3, then player-mitsuami/ the same way
#           (track M22). Both are MSVC (cargo
#           +stable-x86_64-pc-windows-msvc, Visual Studio's C++ tools),
#           linked with a static C runtime so they need no vcruntime DLL,
#           and run on the Windows App Runtime 2.4+, which a PC installs
#           once. The launcher talks to the rest only through the
#           player's command line; the player links QEMU's mingw DLL, two
#           C runtimes in one process (docs/11-m1-embed-api.md, "The C
#           runtime boundary").
#   exec    (Windows only) DXVK's d3d9.dll into build/win/dxvk
#           (configure-dxvk.sh --windows), then build-d3dpt-exec.sh
#           --windows: d3dpt_exec.dll, the Direct3D executor (doc 14). Both
#           MSVC with a static C runtime, in Visual Studio's environment
#           (scripts/msvc-env.sh): DXVK throws C++ exceptions the executor
#           must catch, so they share a compiler (ADR-026's amendment). The
#           package ships DXVK as dxvk_d3d9.dll, the executor's default.
#           D3DPT_D3D9=system (or auto, when DXVK opens no adapter) runs it
#           on Windows' own system32\d3d9.dll instead.
#   wddm    (Windows only) guest-tools/build-wddm.cmd: the WDDM display
#           driver for Windows 7 (track M18, ADR-022), d3dptkmd.sys and
#           d3dptumd.dll into build/wddm/x86. MSVC through the Enterprise
#           WDK for Windows 10 2004 (10.0.19041), the last kit that builds
#           32-bit kernel drivers for Windows 7: one ISO, mounted (or
#           EWDK_ISO=<iso> to mount it, EWDK=<drive:> to name it),
#           nothing installed. With none mounted it fetches the driver
#           the PC published for these sources (scripts/wddm-prebuilt.sh),
#           or is skipped with a note. --publish then uploads the build
#           for Linux and macOS ISOs. The guest stage puts the result on
#           the ISO, in WDDM\.
#   guest   guest-tools/build-wrappers.sh: the guest-tools ISO. It is
#           32-bit guest code, the same file the Linux package ships, so
#           a default run rebuilds it when its sources move, by
#           scripts/build.sh's stamp (build/.stamp-guest-tools). Naming
#           the stage rebuilds it regardless.
#
# docs/build-windows.md is the prose; docs/tracks/m11-windows-host.md is
# the track. Nothing here writes to build/qemu or target/release, so a
# checkout holds a Linux build and a Windows build side by side.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# MSYS2's MINGW64 shell only. The other MSYS2 shells are other C runtimes
# and C++ libraries than the package's msvcrt + libstdc++, so a build there
# would not be the one that ships.
case "${MSYSTEM:-}" in
  MINGW64) ;;
  "") echo "build-windows.sh: Windows builds are made on Windows, in MSYS2's MINGW64 shell (ADR-026; docs/build-windows.md)" >&2; exit 1 ;;
  *) echo "build-windows.sh: this is MSYS2's $MSYSTEM shell; open the MINGW64 one" >&2; exit 1 ;;
esac

# Everything the build needs from MSYS2, in one place: the toolchain and
# QEMU's libraries, gdb, and diffutils, which a bare MSYS2
# lacks: QEMU's meson requires `diff` (tests/qapi-schema) and prepare-qemu.sh
# keeps meson files' mtimes with `cmp`. The second half is the guest-tools
# ISO's: the i686 toolchain, gendef, and what qemu-3dfx's build calls
# (make, which, xxd from vim, shasum from perl, nasm), plus
# xorriso. The third is scripts/test.sh's (docs/testing.md "On Windows"):
# mtools for the guests' scratch disks, bsdtar, and ImageMagick for the
# icon check. Not here: Rust, which is rustup's own installer with the GNU
# host, and Open Watcom, which is a snapshot to unpack (both in
# docs/build-windows.md).
MSYS2_PACKAGES=(git rsync diffutils
  mingw-w64-x86_64-{gcc,clang,lld,gdb,ninja,meson,pkgconf,python,python-distlib}
  mingw-w64-x86_64-{glib2,pixman,zlib,libepoxy,libslirp}
  mingw-w64-x86_64-glslang
  mingw-w64-i686-gcc mingw-w64-x86_64-tools make which vim perl nasm xorriso zstd
  mingw-w64-x86_64-{mtools,imagemagick} libarchive)

JOBS=(); PACKAGE=""; PUBLISH=""; STAGES=(); EXPLICIT=""; FORCE=""
while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS=(-j "$2"); shift 2 ;;
    -j*) JOBS=(-j "${1#-j}"); shift ;;
    -p|--package) PACKAGE=1; shift ;;
    --publish) PUBLISH=1; shift ;;
    -f|--force) FORCE=1; shift ;;
    --msys2-deps)
      pacman -S --needed "${MSYS2_PACKAGES[@]}"
      exit ;;
    -h|--help) awk 'NR > 1 && !/^#/ { exit } NR > 1' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    qemu|rust|mitsuami|exec|wddm|guest) STAGES+=("$1"); shift ;;
    *) echo "build-windows.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done
if [ ${#STAGES[@]} -eq 0 ]; then STAGES=(qemu rust mitsuami exec wddm guest); else EXPLICIT=1; fi

BUILT=(); SKIPPED=(); T0=$SECONDS
want() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }
say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
skip() { # stage, reason
  if [ -n "$EXPLICIT" ]; then echo "build-windows.sh: cannot build '$1': $2" >&2; exit 1; fi
  echo "    SKIP $1 - $2"; SKIPPED+=("$1 ($2)"); return 1
}
# MSYS2's compilers carry no target prefix: the host is the target.
WCC=gcc WCXX=g++

# CRLF fails far from here: every patch of the queue "does not apply".
# Git for Windows sets core.autocrlf=true system-wide, so a clone or a
# worktree made outside MSYS2's own git converts. .gitattributes keeps
# this repository's files as committed; submodules have their own
# attributes, so every git this build runs (submodule init, prepare's
# restores, DXVK's nested submodules) is told not to convert.
if grep -q $'\r' scripts/prepare-qemu.sh; then
  echo "build-windows.sh: the checkout has CRLF line endings; check it out again with core.autocrlf=false (docs/build-windows.md)" >&2
  exit 1
fi
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.autocrlf GIT_CONFIG_VALUE_0=false
missing=()
for t in git rsync diff cmp cygpath gcc g++ clang ld.lld ninja meson pkg-config windres glslangValidator cargo rustc; do
  command -v "$t" >/dev/null || missing+=("$t")
done
if [ ${#missing[@]} -gt 0 ]; then
  echo "build-windows.sh: not found: ${missing[*]} -- run scripts/build-windows.sh --msys2-deps" >&2
  exit 1
fi
# rustup's default on Windows is the MSVC toolchain, whose build scripts
# need Microsoft's link.exe: the host has to be the GNU one.
host="$(rustc -vV | sed -n 's/^host: //p')"
[ "$host" = x86_64-pc-windows-gnu ] || {
  echo "build-windows.sh: rustc's host is $host; run: rustup default stable-x86_64-pc-windows-gnu" >&2; exit 1; }
export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-gcc}"

if [ ! -f qemu/VERSION ] || [ ! -f third_party/qemu-3dfx/qemu-1/hw/mesa/meson.build ]; then
  say "git submodule update --init (qemu, qemu-3dfx)"
  git submodule update --init --depth 1 qemu third_party/qemu-3dfx
fi
# A submodule already checked out with CRLF is checked out again as
# committed. Only pinned upstream trees live there, and prepare restores
# and re-patches qemu/ and DXVK anyway.
git submodule --quiet foreach --recursive 'echo "$displaypath"' | while read -r s; do
  git -C "$s" ls-files --eol | grep -q '^i/lf[[:space:]]*w/crlf' || continue
  echo "    $s: checked out with CRLF line endings; checking it out again"
  (cd "$s" && git ls-files -z | xargs -0 rm -f && git checkout -- .)
done

# The patch queue is applied to the one qemu/ tree both builds compile
# from, so it is prepared here as scripts/build.sh does it, skipped by the
# same stamp (build/.stamp-qemu-prepare: same inputs, same file). A prepare
# rewrites every patched file, and ninja then rebuilt all of QEMU (~1400
# steps, 4 minutes) on every run. A tree left half-prepared by an
# interrupted build is the failure that costs an hour, so the stamp is
# removed before prepare starts and written only once it has finished:
# the skip is taken only over a tree a prepare completed. -f prepares
# regardless.
if want qemu; then
  qemu_stamp=$( { for g in qemu third_party/qemu-3dfx; do git -C "$g" rev-parse HEAD 2>/dev/null || echo none; done
                  find patches/qemu embed d3dpt/hw d3dpt/d3dpt_proto.h \
                       d3dpt/d3dpt_fb.h d3dpt/exec/d3dpt_exec.h libdisc/qemu libdisc/libdisc.h \
                       libsynth/qemu libsynth/libsynth.h gamepad/qemu tpm/qemu voodoo firmware \
                       scripts/prepare-qemu.sh patches/qemu-3dfx \
                       -type f 2>/dev/null | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat 2>/dev/null || true
                } | sha256sum | cut -d' ' -f1)
  if [ -z "$FORCE" ] && [ "$(cat build/.stamp-qemu-prepare 2>/dev/null || true)" = "$qemu_stamp" ]; then
    say "qemu: prepare"
    echo "    patch queue, overlays and submodules unchanged - skipping prepare"
  else
    say "qemu: prepare (overlay + patch queue)"
    rm -f build/.stamp-qemu-prepare
    scripts/prepare-qemu.sh
    mkdir -p build && printf '%s\n' "$qemu_stamp" > build/.stamp-qemu-prepare
  fi

  # meson will not switch a build directory's compiler (patch 68 moved the
  # Windows QEMU from GCC to clang), so a different one configures afresh
  want_cc="${WIN_QEMU_CC:-clang}"
  if [ -f build/win/qemu/build.ninja ] && [ "$(cat build/win/qemu/.2ksbox-cc 2>/dev/null || echo gcc)" != "$want_cc" ]; then
    echo "    build/win/qemu was built with $(cat build/win/qemu/.2ksbox-cc 2>/dev/null || echo gcc), wanted $want_cc - configuring afresh"
    rm -rf build/win/qemu
  fi
  # Nor a directory built from another QEMU release (9.2 to 11.1, M21):
  # ninja's regeneration replays the old command line, which names options
  # the new release removed ("Unknown options: glusterfs"), and the old
  # release's thin archives are updated in place, which MSYS2's ar cannot
  # open ("ar: libqemuutil.a: No such file or directory"). A directory with
  # no record is from before the record, so 9.2's.
  want_qemu="$(cat qemu/VERSION)"
  if [ -f build/win/qemu/build.ninja ] && [ "$(cat build/win/qemu/.2ksbox-qemu 2>/dev/null || echo 9.2.4)" != "$want_qemu" ]; then
    echo "    build/win/qemu was built from QEMU $(cat build/win/qemu/.2ksbox-qemu 2>/dev/null || echo 9.2.4), the tree is $want_qemu - configuring afresh"
    rm -rf build/win/qemu
  fi
  # Configure again when the tree or the configure script changed since the
  # last configure, as scripts/build.sh does (a flag configure-qemu.sh
  # gained is otherwise not applied). configure starts from a clean
  # meson-private, so the objects stay.
  needs_configure=""
  [ -f build/win/qemu/build.ninja ] || needs_configure=1
  for f in qemu/meson.build qemu/hw/mesa/meson.build scripts/configure-qemu.sh; do
    if [ -f build/win/qemu/build.ninja ] && [ "$f" -nt build/win/qemu/build.ninja ]; then
      needs_configure=1
    fi
  done
  if [ -n "$needs_configure" ]; then
    say "qemu: configure (mingw-w64, $want_cc)"
    scripts/configure-qemu.sh --windows
    echo "$want_cc" > build/win/qemu/.2ksbox-cc
    echo "$want_qemu" > build/win/qemu/.2ksbox-qemu
  else
    echo "    build/win/qemu is configured - skipping configure"
    # prepare re-applied the queue, so meson may need to regenerate; ninja
    # works that out itself from the mtimes it just saw change.
    :
  fi
  say "qemu: ninja"
  ninja -C build/win/qemu ${JOBS[@]+"${JOBS[@]}"} \
    qemu-system-i386.exe qemu-img.exe qemu-io.exe libqemu-embed-i386.dll
  BUILT+=(qemu)
fi

if want rust; then
  # The winit player stays on QEMU's mingw ABI: it is test.sh's player,
  # not the package's (ADR-026's second amendment). libdisc and libsynth
  # are linked into QEMU by its own stage.
  say "rust: cargo build --release --target x86_64-pc-windows-gnu -p player"
  cargo build --release --target x86_64-pc-windows-gnu ${JOBS[@]+"${JOBS[@]}"} -p player
  # The tools are MSVC, as everything that does not link into QEMU
  # (scripts/cargo-msvc.sh, target/x86_64-pc-windows-msvc).
  if ! rustup run stable-x86_64-pc-windows-msvc rustc -V >/dev/null 2>&1; then
    skip rust "no stable-x86_64-pc-windows-msvc toolchain for launcherx, discx and synthx (rustup toolchain install stable-x86_64-pc-windows-msvc; needs Visual Studio's C++ tools)" || true
  else
    say "rust: scripts/cargo-msvc.sh build --release (launcherx, discx, synthx)"
    scripts/cargo-msvc.sh build --release ${JOBS[@]+"${JOBS[@]}"} \
      -p launcher-core -p libdisc -p libsynth --bin launcherx --bin discx --bin synthx
  fi
  BUILT+=(rust)
fi

# --- mitsuami ---------------------------------------------------------
# The launcher (ADR-023), WinUI 3 through mitsuami: MSVC, into launcher-mitsuami/target/release, where package-windows.sh and
# win-run.sh look. `+crt-static` keeps vcruntime140.dll out of its import
# table: that DLL is not part of Windows, and a PC without Visual C++'s
# redistributable would get a loader dialog before any code of ours ran.
if want mitsuami; then
  MSVC=stable-x86_64-pc-windows-msvc
  if ! rustup run "$MSVC" rustc -V >/dev/null 2>&1; then
    skip mitsuami "no $MSVC toolchain (rustup toolchain install $MSVC; needs Visual Studio's C++ tools)" || true
  else
    say "mitsuami: cargo +$MSVC build --release (launcher-mitsuami)"
    # Without MSYS2's /usr/bin (and /bin, the same directory) on PATH:
    # its `link` is coreutils' and is found before Microsoft's link.exe
    # ("link: extra operand"). /mingw64/bin stays, for windres.
    nolink=""; IFS=: read -ra dirs <<< "$PATH"
    for d in "${dirs[@]}"; do case "$d" in /usr/bin|/bin) ;; *) nolink="${nolink:+$nolink:}$d" ;; esac; done
    cargo=$(command -v cargo)
    ( cd launcher-mitsuami && PATH="$nolink" CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS="-C target-feature=+crt-static" \
        "$cargo" "+$MSVC" build --release )
    # ... and the player on mitsuami (track M22), the same way. It links
    # QEMU's mingw DLL across two C runtimes (doc 11, "The C runtime
    # boundary"). A launcher in a checkout starts it whenever it is built
    # (launcher_core::player); packages still ship the winit player.
    say "mitsuami: cargo +$MSVC build --release (player-mitsuami)"
    ( cd player-mitsuami && PATH="$nolink" CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS="-C target-feature=+crt-static" \
        "$cargo" "+$MSVC" build --release )
    BUILT+=(mitsuami)
  fi
fi

if want exec; then
  # The queue is applied on the host, like qemu's above. The DXVK tree is
  # shared with the native build, so this keeps scripts/build.sh's own
  # stamp (same file, same hash). A prepare hands both builds fresh
  # mtimes, so one that changed nothing would cost the native DXVK a full
  # rebuild.
  say "exec: DXVK d3d9.dll (prepare + MSVC)"
  dxvk_stamp=$( { git -C third_party/dxvk rev-parse HEAD 2>/dev/null || echo none
                  find patches/dxvk scripts/prepare-dxvk.sh -type f | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat
                } | sha256sum | cut -d' ' -f1)
  if [ "$(cat build/.stamp-dxvk-prepare 2>/dev/null || true)" != "$dxvk_stamp" ]; then
    scripts/prepare-dxvk.sh
    mkdir -p build && printf '%s\n' "$dxvk_stamp" > build/.stamp-dxvk-prepare
  else
    echo "    patch queue and submodule unchanged - skipping prepare"
  fi
  # DXVK and the executor are MSVC, together (ADR-026's amendment: DXVK
  # throws C++ exceptions the executor must catch). ninja runs in Visual
  # Studio's environment too (scripts/msvc-env.sh), since cl finds its
  # headers and libraries through it. A directory configured before the
  # move (mingw's gcc) is configured afresh by configure-dxvk.sh.
  if ! ( . scripts/msvc-env.sh ) >/dev/null 2>&1; then
    skip exec "no Visual Studio with the x64 C++ tools (DXVK and the executor are MSVC; docs/build-windows.md)" || true
  else
    if [ ! -f build/win/dxvk/build.ninja ] || [ "$(cat build/win/dxvk/.2ksbox-cc 2>/dev/null)" != msvc ]; then
      scripts/configure-dxvk.sh --windows
    fi
    ( . scripts/msvc-env.sh && ninja -C build/win/dxvk ${JOBS[@]+"${JOBS[@]}"} src/d3d9/d3d9.dll )
    say "exec: d3dpt_exec.dll (the Direct3D decoder + executor, MSVC)"
    scripts/build-d3dpt-exec.sh --windows
    # The WGL probe (tools/wgl-probe.c) rides along as one more compile,
    # MSVC like every shipped tool (static C runtime). It is the first
    # thing to run on a Windows machine whose Win98 guest gets no OpenGL.
    # Options with '-', not '/', which MSYS2 would take for paths.
    say "exec: wgl-probe.exe (the embed backend's WGL sequence, without QEMU)"
    ( . scripts/msvc-env.sh && cd build/win && cl -nologo -O1 -MT -W3 -D_CRT_SECURE_NO_WARNINGS \
        -Fewgl-probe.exe "$(cygpath -w "$ROOT/tools/wgl-probe.c")" opengl32.lib gdi32.lib user32.lib )
    BUILT+=(exec)
  fi
  # ... and the display driver's host test, which package-windows.sh runs
  # against the staged pair: a frame through the Windows DLLs. It stays
  # mingw, as QEMU is: it loads the MSVC executor as QEMU does, so it
  # proves the boundary between the two (doc 11, "The C runtime boundary").
  "$WCXX" -std=c++17 -O2 -static -o build/win/d3dpt-dp2-test.exe tools/d3dpt-dp2-test.cpp
fi

# The WDDM driver (track M18): MSVC through the EWDK, whose own
# environment build-wddm.cmd sets up (SetupBuildEnv), so no Visual Studio
# install is involved. msbuild builds what changed, so the stage always
# runs; it is skipped, with a note, when no EWDK 10.0.19041 is mounted
# and none is named. The guest stage stages build/wddm/x86 on the ISO.
if want wddm; then
  ewdk="${EWDK:-}${EWDK_ISO:-}"
  if [ -z "$ewdk" ]; then
    for d in /{d..z}; do
      if [ -f "$d/BuildEnv/SetupBuildEnv.cmd" ] &&
         [ -f "$d/Program Files/Windows Kits/10/Include/10.0.19041.0/km/dispmprt.h" ]; then ewdk="$d"; fi
    done
  fi
  if [ -z "$ewdk" ] && [ -z "$PUBLISH" ] && scripts/wddm-prebuilt.sh fetch; then
    echo "    no EWDK mounted; the published driver for these sources is in build/wddm/x86"
  elif [ -z "$ewdk" ]; then
    skip wddm "no EWDK 10.0.19041 mounted (mount it, or EWDK_ISO=<iso>; docs/build-windows.md \"The WDDM driver\")" || true
  else
    say "wddm: d3dptkmd.sys + d3dptumd.dll (MSVC, the EWDK)"
    cmd //c "$(cygpath -w guest-tools/build-wddm.cmd)"
    # the sources it was built from, which a publish checks
    scripts/wddm-prebuilt.sh key > build/wddm/x86/.key
    BUILT+=(wddm)
    if [ -n "$PUBLISH" ]; then
      say "wddm: publish for Linux and macOS (scripts/wddm-prebuilt.sh)"
      scripts/wddm-prebuilt.sh publish
    fi
  fi
fi

# The ISO is guest code and identical whatever host built it, so it is
# rebuilt when its sources move, by scripts/build.sh's stamp (same file,
# same hash): a protocol bump or a driver change makes it stale, and a
# stale one reads as a guest that will not attach. Only its presence was
# checked before, and an ISO from before M16 failed the XP checks.
if want guest; then
  guest_stamp=$( { git -C third_party/qemu-3dfx rev-parse HEAD 2>/dev/null || echo none
                   find guest-tools/src d3dpt/d3dpt_proto.h d3dpt/d3dpt_fb.h \
                        cdshelf/cdshelf_proto.h guest-tools/build-wrappers.sh \
                        guest-tools/build-driver.sh guest-tools/build-driver9x.sh \
                        build/wddm/x86/d3dptkmd.sys build/wddm/x86/d3dptumd.dll build/wddm/x86/d3dptkmd.inf \
                        -type f 2>/dev/null | LC_ALL=C sort | tr '\n' '\0' | xargs -0 cat 2>/dev/null
                 } | sha256sum | cut -d' ' -f1)
  guest_current=""
  [ "$(cat build/.stamp-guest-tools 2>/dev/null || true)" = "$guest_stamp" ] \
    && [ -e guest-tools/out/d3dpt-driver.iso ] \
    && ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1 && guest_current=1
  if [ -z "$EXPLICIT" ] && [ -n "$guest_current" ]; then
    say "guest"
    echo "    guest sources, protocol headers and qemu-3dfx unchanged - skipping"
  else
    # msys2-i686.sh, sourced by the script, switches it to MSYS2's i686
    # toolchain and says what is missing
    say "guest: guest-tools ISO (MSYS2 i686)"
    guest-tools/build-wrappers.sh
    mkdir -p build && printf '%s\n' "$guest_stamp" > build/.stamp-guest-tools
    BUILT+=(guest)
  fi
fi

say "summary after $((SECONDS - T0)) s"
[ ${#BUILT[@]} -gt 0 ] && printf '    built: %s\n' "${BUILT[*]}"
if [ ${#SKIPPED[@]} -gt 0 ]; then printf '    skipped:\n'; printf '      %s\n' "${SKIPPED[@]}"; fi

if [ -n "$PACKAGE" ]; then
  say "scripts/package-windows.sh"
  exec scripts/package-windows.sh
fi
echo
echo "    next: scripts/win-run.sh launcher   (or player / qemu; GDB=1 runs it under gdb)"
echo "          scripts/package-windows.sh    (the zip, checked on this PC)"
