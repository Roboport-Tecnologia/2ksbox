#!/usr/bin/env bash
# The regression suite: integration and end-to-end only (CLAUDE.md policy).
#
#   scripts/test.sh            host stage: every check that needs no guest
#   scripts/test.sh guest      XP on the paravirtual Direct3D device, headless
#   scripts/test.sh all        both
#
# Builds nothing big: it expects build/qemu (qemu-system-i386 +
# libqemu-embed), build/dxvk and build/d3dpt/libd3dpt_exec.* to exist (the
# cheat sheet in docs/00-status.md), and only compiles the small tools in
# tools/. Outputs land in build/test/. A check that cannot run here (no
# x86 host, no display, no image) is reported as SKIP, not as a failure.
#
# Every check group, what it proves and how to run it: docs/testing.md.
# Each check function below says why it exists.
#
# Environment: WINXP_IMG (~/vms/winxp.qcow2), GUEST_ISO (newest
# guest-tools/out/guest-tools-3dfx-*.iso), D3D_GOLDEN_BUDGET (1200).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build/test"; mkdir -p "$OUT"
STAGE="${1:-host}"
OS="$(uname -s)"; ARCH="$(uname -m)"
# The launcher every package installs (ADR-023), its own cargo workspace.
LAUNCHER_BIN=launcher-mitsuami/target/release/launcher-mitsuami
DX="$ROOT/third_party/dxvk/include/native"
GOLDEN="$ROOT/reference/d3d/rig-2026-09-03/d3dgame9-w300-ff.bmp"
HUD_MASK="0,368,270,112"
BUDGET="${D3D_GOLDEN_BUDGET:-1200}"
# Where this platform's build puts things. Windows runs natively in MSYS2's
# MINGW64 shell on what scripts/build-windows.sh built (build/win,
# target/x86_64-pc-windows-gnu, never the Linux layout); MSYS2 finds
# `foo.exe` for `foo`, so the names carry no suffix. docs/testing.md
# "On Windows".
case "$OS" in MINGW64_NT*) OS=Windows;; MINGW*|MSYS*|CYGWIN*)
  echo "test.sh: run it from MSYS2's MINGW64 shell" >&2; exit 2;; esac
QDIR=build/qemu; RREL=target/release; CARGO_TGT=()
case "$OS" in
  Darwin) SO=dylib;;
  Windows)
    SO=dll; QDIR=build/win/qemu; RREL=target/x86_64-pc-windows-gnu/release
    CARGO_TGT=(--target x86_64-pc-windows-gnu)
    export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER="${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-gcc}"
    # the programs below find libqemu-embed and the executor's DLLs here
    export PATH="$ROOT/$QDIR:$ROOT/$RREL:$PATH";;
  *) SO=so;;
esac
# the launcher's data dir (launcher-core/src/paths.rs), for the checks that
# use a machine of the user's: read only, booted through an overlay
DATA_DIR="$HOME/.local/share/2ksbox"
[ "$OS" = Windows ] && DATA_DIR="$(cygpath -F 26)/2ksbox/data"   # CSIDL_APPDATA: MSYS2 started from another shell may have no APPDATA
QSYS=$QDIR/qemu-system-i386; QIMG=$QDIR/qemu-img; QIO=$QDIR/qemu-io
# QMP piped into `-qmp stdio`: QEMU on Windows answers only the first line
# from a pipe, so there it goes through a loopback port (tools/qmp-pipe.py)
QSYS_PIPE=$QSYS
# the executor's host test: built here on Linux and macOS, by
# scripts/build-windows.sh's exec stage on Windows
DP2=build/d3dpt-dp2-test; [ "$OS" = Windows ] && DP2=build/win/d3dpt-dp2-test
# Windows: build/test as C:/..., which bash and the native programs read
# alike. MSYS2 rewrites a /c/... argument for a native program but not an
# environment variable's value, and the checks hand most paths over in
# LAUNCHER_* variables.
[ "$OS" = Windows ] && OUT="$(cygpath -m "$OUT")"
[ "$OS" = Windows ] && QSYS_PIPE="python3 tools/qmp-pipe.py $QSYS"
LAUNCHERX=$RREL/launcherx; DISCX=$RREL/discx; SYNTHX=$RREL/synthx; PLAYER=$RREL/player
# Paths in one spelling. On Windows the launcher writes them its way
# (C:/given/dir\joined\part, JSON's doubled backslashes) and bash its own
# (/c/...): `normp` turns every backslash of a text into '/', `np` writes
# a bash path as the launcher would (C:/...). Elsewhere both are identity.
normp() { if [ "$OS" = Windows ]; then sed -e 's#\\\\#/#g' -e 's#\\#/#g'; else cat; fi; }
np() { if [ "$OS" = Windows ]; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
# and for MSYS2's own programs (xorriso), whose arguments take no C:/...
up() { if [ "$OS" = Windows ]; then cygpath -u "$1"; else printf '%s\n' "$1"; fi; }
if [ "$OS" = Windows ] && [ -x "$LAUNCHERX" ]; then
  # launcherx's stdout through normp, so every check compares paths as on
  # Linux; its status and stderr pass through
  mkdir -p build/test-bin
  printf '%s\n' '#!/usr/bin/env bash' 'set -o pipefail' \
    "\"$ROOT/$LAUNCHERX\" \"\$@\" | sed -e 's#\\\\\\\\#/#g' -e 's#\\\\#/#g'" > build/test-bin/launcherx
  chmod +x build/test-bin/launcherx
  LAUNCHERX=build/test-bin/launcherx
fi
if [ "$OS" = Windows ]; then
  # the package's names (scripts/win-run.sh): DXVK only as dxvk_d3d9.dll,
  # never plain d3d9.dll, which is Windows' own
  if [ -f build/win/dxvk/src/d3d9/d3d9.dll ]; then
    cmp -s build/win/dxvk/src/d3d9/d3d9.dll build/win/d3dpt/dxvk_d3d9.dll \
      || cp build/win/dxvk/src/d3d9/d3d9.dll build/win/d3dpt/dxvk_d3d9.dll
  fi
  export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/win/d3dpt/d3dpt_exec.dll}"
  export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/win/d3dpt/dxvk_d3d9.dll}"
else
  export D3DPT_EXEC_LIB="${D3DPT_EXEC_LIB:-$ROOT/build/d3dpt/libd3dpt_exec.$SO}"
  export D3DPT_DXVK_LIB="${D3DPT_DXVK_LIB:-$ROOT/build/dxvk/src/d3d9/libdxvk_d3d9.$SO$([ "$SO" = so ] && echo .0)}"
fi
# A player window the compositor focuses would otherwise take the desktop's
# own shortcuts for the length of the run (player/src/kbcapture.rs).
export PLAYER_KEYBOARD_CAPTURE="${PLAYER_KEYBOARD_CAPTURE:-0}"
if [ "$OS" = Darwin ]; then
  # The cargo builds below link for the same macOS as everything else
  # (the floor, scripts/macos-floor.sh), not for rustc's default.
  MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(scripts/macos-floor.sh)}"
  export MACOSX_DEPLOYMENT_TARGET
  # DXVK dlopens the Vulkan loader by leaf name; a DYLD_* variable handed
  # to this script is stripped by SIP at the `#!/usr/bin/env` exec, so set
  # the documented macOS run environment here (docs/build-macos.md,
  # patches/dxvk/README.md): Homebrew's loader, and the LunarG SDK's
  # KosmicKrisp ICD unless the caller chose one.
  # The loader's own keg, never all of `/opt/homebrew/lib`: DYLD_LIBRARY_PATH
  # is searched by leaf name ahead of the path an image asks for, and ImageIO
  # `dlopen`s its codecs as `libGIF.dylib` / `libPng.dylib` / `libTIFF.dylib`
  # / `libJPEG.dylib`, every one of which that directory answers on a
  # case-insensitive filesystem (docs/00-status.md, 2026-09-08).
  VKLIB=/opt/homebrew/opt/vulkan-loader/lib
  [ -d "$VKLIB" ] || VKLIB=/opt/homebrew/lib
  export DYLD_LIBRARY_PATH="$VKLIB${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
  if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for f in "$HOME"/VulkanSDK/*/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json; do
      [ -f "$f" ] && export VK_ICD_FILENAMES="$f"
    done
  fi
fi

PASS=(); FAIL=(); SKIP=()
# The Wine executor (M15) that a check may start on a host without Vulkan
# gets a prefix in the build tree, never the one under the user's data dir.
export D3DPT_WINEPREFIX="${D3DPT_WINEPREFIX:-$PWD/build/wine-prefix}"
log() { printf '\n==> %s\n' "$*"; }
skip() { SKIP+=("$1: $2"); printf '  SKIP %s (%s)\n' "$1" "$2"; }
# GNU coreutils' timeout(1) is not on a Mac (nor is gtimeout unless
# someone installed coreutils), and a check that hangs is worse than one
# that fails, so stand one in.
if ! command -v timeout >/dev/null; then
  if command -v gtimeout >/dev/null; then
    timeout() { gtimeout "$@"; }
  else
    timeout() { # seconds, command...
      local s="$1" p w rc; shift
      "$@" & p=$!
      # The watchdog gets none of the command's descriptors. A caller
      # reading the output through a pipe (`o="$(timeout … | sed …)"`)
      # waits for every writer to close it, so a watchdog that inherited
      # stdout held the pipe for the whole limit and every window check took
      # its full 120 s on a Mac without coreutils. Afterwards the sleep is
      # killed with the subshell, or it lives on orphaned.
      ( sleep "$s"; kill -9 "$p" 2>/dev/null ) >/dev/null 2>&1 </dev/null & w=$!
      wait "$p"; rc=$?
      pkill -P "$w" 2>/dev/null; kill "$w" 2>/dev/null
      return $rc
    }
  fi
fi

run_check() { # name, log file, command...
  local name="$1" lf="$OUT/$2"; shift 2
  "$@" >"$lf" 2>&1; local rc=$?
  if [ $rc = 0 ]; then PASS+=("$name"); printf '  PASS %s\n' "$name"; return 0; fi
  if [ $rc = 77 ]; then skip "$name" "$(tail -1 "$lf")"; return 0; fi
  FAIL+=("$name"); printf '  FAIL %s (exit %d) — %s\n' "$name" $rc "$lf"; tail -5 "$lf" | sed 's/^/       /'; return 1
}
exec_no_device_check() { # the executor with a loader and no Vulkan device: refused, never a crash
  local o rc
  # (Windows: DXVK by name; `auto` there falls back to the system's own
  # d3d9.dll, which d3dpt-dp2-system checks)
  local pick=(); [ "$OS" = Windows ] && pick=(D3DPT_D3D9=dxvk)
  o="$(env "${pick[@]}" VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json \
       $DP2 "$OUT/dp2-no-device.bmp" 2>&1)"; rc=$?
  echo "$o" | grep -v "^info:" | tail -8
  if [ $rc -ge 128 ]; then echo "the dp2 test with no Vulkan device died of signal $((rc - 128))"; return 1; fi
  if [ $rc != 77 ]; then echo "the dp2 test with no Vulkan device exited $rc, not 77 (no executor)"; return 1; fi
  case "$o" in *"found no usable device"*) ;; *) echo "the executor never said the device was refused"; return 1;; esac
  return 0
}
exec_wine_bin() { # a Wine for the executor's other process: D3DPT_WINE, the PATH, a Mac app, the spike's tarball
  if [ -n "${D3DPT_WINE:-}" ]; then [ -x "$D3DPT_WINE" ] && echo "$D3DPT_WINE"; return; fi
  local c
  for c in "$(command -v wine64 2>/dev/null)" "$(command -v wine 2>/dev/null)" \
           "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine" \
           "/Applications/Wine Staging.app/Contents/Resources/wine/bin/wine" \
           build/wine/Wine*.app/Contents/Resources/wine/bin/wine; do
    [ -n "$c" ] && [ -x "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}
# A subshell, not a group: run_check calls a check in this shell, and these
# exports once stayed set for everything after it. Under `all` the XP guest
# then ran the Wine executor (`d3dpt: executor ...libd3dpt_exec_remote.so`
# in its QEMU log) and guest-G9/G8/F9=native failed by 1.93 %, while
# `guest` alone passed.
exec_wine_check() ( # the host test through the remote executor, its frame against the in-process one
  local wine="$1" rc=0
  # macOS: no window server over ssh, and Wine's Mac driver needs one
  if [ "$(uname -s)" = Darwin ] && [ -z "${TERM_PROGRAM:-}" ] && [ -n "${SSH_CONNECTION:-}" ]; then
    echo "no GUI session (ssh): Wine's Mac driver has no window server here"; return 77
  fi
  export D3DPT_WINE="$wine" D3DPT_EXEC_LIB="build/d3dpt/libd3dpt_exec_remote.$SO"
  export D3DPT_WINEPREFIX="${D3DPT_WINEPREFIX:-$PWD/build/wine-prefix}"   # kept across runs: a fresh one costs ~20 s
  export D3DPT_REMOTE_DIR="$OUT"
  echo "wine: $wine"; echo "prefix: $D3DPT_WINEPREFIX"
  $DP2 "$OUT/dp2-wine.bmp" > "$OUT/exec-wine-dp2.log" 2>&1 || { echo "dp2 test through the remote executor failed:"; tail -5 "$OUT/exec-wine-dp2.log"; return 1; }
  grep -h "d3dpt-remote:\|d3dpt-exec-host: exec: d3d9\|frames," "$OUT/exec-wine-dp2.log" | sort -u | head -8
  python3 tools/bmpdiff.py "$OUT/dp2-test.bmp" "$OUT/dp2-wine.bmp" --tolerance 8 || rc=1
  return $rc
)
dirdisc_check() { # a host directory served as a disc, read back by someone else's ISO 9660 reader
  # discx's own dirdisc case (the libdisc check) proves the model reads
  # the tree back; this one proves the *volume* is one, by handing it to
  # a reader that is not ours and diffing the result against the folder.
  local src="$OUT/dirsrc" ext="$OUT/dirsrc-out" iso="$OUT/dirsrc.iso" rc=0
  # QEMU is given absolute paths: it does not run from here, and $OUT may
  # or may not be absolute already.
  local abs="$src" absiso="$iso"
  case "$abs" in /*|[A-Za-z]:/*) ;; *) abs="$PWD/$abs"; absiso="$PWD/$absiso";; esac
  $DISCX mktree "$src" || { echo "mktree failed"; return 1; }
  $DISCX export "isodir:$src" "$iso" >/dev/null || { echo "export failed"; return 1; }
  [ -d "$ext" ] && chmod -R u+w "$ext"; rm -rf "$ext"; mkdir -p "$ext"
  # Both readers are independent of us; xorriso is preferred only because
  # libarchive rewrites names to NFD on macOS, which no guest does.
  local extra=()
  if command -v xorriso >/dev/null; then
    xorriso -osirrox on -indev "$(up "$iso")" -extract / "$(up "$ext")" >"$OUT/dirdisc-extract.log" 2>&1 || { echo "xorriso could not read the volume"; return 1; }
  elif command -v bsdtar >/dev/null; then
    bsdtar -xf "$iso" -C "$ext" >"$OUT/dirdisc-extract.log" 2>&1 || { echo "bsdtar could not read the volume"; return 1; }
    [ "$OS" = Darwin ] && extra=(-x 'caf*')
  else
    echo "needs xorriso or bsdtar"; return 77
  fi
  chmod -R u+w "$ext"
  # The two names Joliet cannot hold are excluded here and checked below:
  # everything else must come back exactly as it went in.
  diff -r -x 'semi*' -x 'star*' "${extra[@]}" "$src" "$ext" || { echo "the folder did not come back identical"; rc=1; }
  cmp -s "$src/semi;colon.txt" "$ext/semi_colon.txt" || { echo "semi;colon.txt is not there as semi_colon.txt"; rc=1; }
  # (a Windows folder cannot hold a *, so mktree makes no such name there)
  [ "$OS" = Windows ] || cmp -s "$src/star*name.txt" "$ext/star_name.txt" || { echo "star*name.txt is not there as star_name.txt"; rc=1; }

  # The *other* tree: ISO 9660 level 1, which is what a real-mode DOS
  # driver reads and what Windows falls back to. bsdtar with Joliet
  # turned off is the only reader here that will look at it. The two
  # colliding names are the point. A mangling that crossed their
  # contents would pass every check that only counts files.
  if command -v bsdtar >/dev/null; then
    local dos="$OUT/dirsrc-83"
    [ -d "$dos" ] && chmod -R u+w "$dos"; rm -rf "$dos"; mkdir -p "$dos"
    if bsdtar -xf "$iso" -C "$dos" --options 'iso9660:!joliet,iso9660:!rockridge' 2>>"$OUT/dirdisc-extract.log"; then
      chmod -R u+w "$dos"
      local star=STAR_NAM.TXT; [ "$OS" = Windows ] && star=   # no * in a Windows folder
      for n in EMPTY.BIN README.TXT PROGRAM_.TXT CAF_.TXT SEMI_COL.TXT $star COLLISIO.TXT COLLIS~1.TXT LLLLLLLL.TXT EXACT204.BIN ODD2049.BIN; do
        [ -f "$dos/$n" ] || { echo "the 8.3 tree has no $n"; rc=1; }
      done
      [ -d "$dos/EMPTY_DI" ] || { echo "the 8.3 tree has no EMPTY_DI directory"; rc=1; }
      grep -q '^one$' "$dos/COLLISIO.TXT" 2>/dev/null || { echo "COLLISIO.TXT is not collision-one.txt"; rc=1; }
      grep -q '^two$' "$dos/COLLIS~1.TXT" 2>/dev/null || { echo "COLLIS~1.TXT is not collision-two.txt"; rc=1; }
      cmp -s "$src/big.bin" "$dos/BIG.BIN" || { echo "BIG.BIN differs in the 8.3 tree"; rc=1; }
    else
      echo "bsdtar could not read the primary tree"; rc=1
    fi
  fi

  # The block layer: the same bytes through QEMU's own read path.
  if [ -x $QIMG ]; then
    local info; info="$($QIMG info --output=json "isodir:$abs" 2>/dev/null)"
    echo "$info" | grep -q '"format": "isodir"' || { echo "not opened by the isodir driver"; echo "$info"; rc=1; }
    $QIMG convert -O raw "isodir:$abs" "$OUT/dirsrc-qemu.iso" 2>/dev/null \
      && cmp -s "$OUT/dirsrc-qemu.iso" "$iso" || { echo "qemu-img read the folder differently from discx"; rc=1; }
  fi

  # The drive. The ATAPI path has to find the disc model through whatever
  # node graph the block layer built. A protocol driver reached by its
  # filename prefix ends up under a probed `raw` format node, and a
  # cdimage_disc() that misses it fails silently, leaving the guest with
  # QEMU's stock answers. CDIMAGE_TRACE prints a line per packet only
  # when the model is there, so SeaBIOS probing the drive is the proof;
  # the same run on a plain .iso (the raw driver, no model) is the control.
  if [ -x $QSYS ]; then
    local n c
    n="$(atapi_disc_packets "isodir:$abs" "$OUT/dirdisc-probe.log")"
    c="$(atapi_disc_packets "$absiso" "$OUT/dirdisc-control.log")"
    [ "${n:-0}" -gt 0 ] || { echo "the drive saw no disc model for the folder (cdimage_disc found nothing)"; rc=1; }
    [ "${c:-0}" = 0 ] || { echo "the trace fired for a plain .iso on the raw driver: it proves nothing"; rc=1; }
  fi
  return $rc
}

atapi_disc_packets() { # disc, log -> packets the cdimage disc path saw while SeaBIOS probed the drive
  local disc="$1" out="$2" pid i
  # Both logs are markers this function waits on: a stale one from an
  # earlier run reads as "already finished" and the machine gets killed
  # before it has probed anything.
  rm -f "$out" "$out.bios"
  CDIMAGE_TRACE=1 $QSYS -L qemu/pc-bios -machine pc -m 64 \
    -display none -net none -boot d -no-reboot \
    -debugcon "file:$out.bios" -global isa-debugcon.iobase=0x402 \
    -drive "if=none,id=cd0,media=cdrom,file=$disc" \
    -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0 >"$out" 2>&1 &
  pid=$!
  # SeaBIOS says this once it has probed every drive; it takes well under
  # a second, and the machine would otherwise sit there with no disk.
  for i in $(seq 1 40); do
    grep -q "No bootable device" "$out.bios" 2>/dev/null && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.25
  done
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  grep -c "atapi-disc:" "$out" 2>/dev/null || true
}

cdimage_check() { # the block driver through qemu-img / qemu-io on the selftest images
  local d="$OUT/disc" want=$((6800 * 2048)) rc=0
  for f in mixed.cue mixed.ccd cooked.cue; do
    local info; info="$($QIMG info --output=json "$d/$f")" || { echo "$f: qemu-img info failed"; return 1; }
    echo "$info" | grep -q '"format": "cdimage"' || { echo "$f: not probed as cdimage"; echo "$info"; rc=1; }
    echo "$info" | grep -q "\"virtual-size\": $want" || { echo "$f: virtual size is not $want"; echo "$info"; rc=1; }
  done
  $QIMG info --output=json "$d/plain.iso" | grep -q '"format": "raw"' || { echo "plain.iso: no longer probes to raw"; rc=1; }
  $QIMG dd -f cdimage -O raw bs=2048 count=2000 "if=$d/mixed.cue" "of=$OUT/cdimage-dd.bin" || rc=1
  cmp "$OUT/cdimage-dd.bin" "$d/plain.iso" || { echo "the data track through the block layer differs from plain.iso"; rc=1; }
  local o
  o="$($QIO -r -c "read $((1000 * 2048)) 2048" "$d/lec.cue" 2>&1)"
  case "$o" in *"read failed"*) ;; *) echo "the flipped sector read cleanly"; rc=1;; esac
  o="$($QIO -r -c "read $((2000 * 2048)) 2048" "$d/mixed.cue" 2>&1)"
  case "$o" in *"read failed"*) ;; *) echo "an audio sector read as data"; rc=1;; esac
  [ "$(nm -D build/qemu/libqemu-embed-i386.$SO 2>/dev/null | grep -c ' T _ZN3std')" = 0 ] || { echo "Rust std symbols exported from the embed library"; rc=1; }
  return $rc
}
host_check_probe() { # `launcherx --host-check` (ADR-013), on any host
  local rc=0 o
  # This host's own answer: either verdict is legal, the report is not.
  o="$($LAUNCHERX --host-check 2>&1)" || true
  case "$o" in *"Vulkan loader:"*) ;; *) echo "the report names no loader"; echo "$o"; rc=1;; esac
  case "$o" in *"Required: a 1.3 device"*) ;; *) echo "the report names no bar"; echo "$o"; rc=1;; esac
  # A host with no Vulkan driver at all, which every host can be made
  # into. Both loader variables are set, since which one is read depends
  # on how old the loader is. No Wine either (D3DPT_WINE naming a path
  # that does not exist means none, by the probe's rule). That is the host
  # with no executor at all, so the answer is unavailable and the user is
  # told to install Wine.
  if [ "$OS" = Windows ]; then
    # Windows below the floor runs the executor on its own d3d9.dll
    # (ADR-007's second amendment), never through Wine
    o="$(VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json \
         $LAUNCHERX --host-check 2>&1)" \
      || { echo "exit non-zero with no Vulkan driver on Windows"; echo "$o"; rc=1; }
    case "$o" in *"available, on this PC's own Direct3D 9"*) ;; *) echo "no Vulkan driver on Windows, yet not on its own Direct3D 9"; echo "$o"; rc=1;; esac
  else
  o="$(VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json D3DPT_WINE=/nonexistent \
       $LAUNCHERX --host-check 2>&1)" \
    && { echo "exit 0 with no Vulkan driver and no Wine"; rc=1; }
  case "$o" in *unavailable*) ;; *) echo "no Vulkan driver, no Wine, yet not reported unavailable"; echo "$o"; rc=1;; esac
  case "$o" in *"Install Wine"*) ;; *) echo "no Vulkan driver, no Wine, yet not told to install Wine"; echo "$o"; rc=1;; esac
  case "$o" in *"Wine: none found"*) ;; *) echo "the report does not say no Wine was found"; echo "$o"; rc=1;; esac
  fi
  # The same host with a Wine and the executor's Windows build (ADR-018,
  # M15): the device is available, through the executor in another
  # process, and a script asking "can this host do 3D" hears yes. Needs
  # both halves on this box; without them the case is not testable here.
  local wine_bin
  if wine_bin=$(exec_wine_bin) && [ -f build/d3dpt/wine/d3dpt-exec-host.exe ]; then
    o="$(VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json D3DPT_WINE="$wine_bin" \
         $LAUNCHERX --host-check 2>&1)" \
      || { echo "exit non-zero with no Vulkan driver but a Wine at hand"; echo "$o"; rc=1; }
    case "$o" in *"through Wine on this host"*) ;; *) echo "no Vulkan driver, a Wine at hand, yet not reported as through Wine"; echo "$o"; rc=1;; esac
    case "$o" in *"Wine: $wine_bin"*) ;; *) echo "the report does not name the Wine it was given"; echo "$o"; rc=1;; esac
    case "$o" in *"Executor for Wine: "*d3dpt-exec-host.exe*) ;; *) echo "the report does not name the executor's Windows build"; echo "$o"; rc=1;; esac
  else
    echo "  (no Wine or no build/d3dpt/wine/ here: the through-Wine answer not exercised)"
  fi
  # Where lavapipe is installed, the other half is testable for real: a
  # software driver is *usable* (DXVK ranks a CPU device last but never
  # excludes it), so the verdict is available with a warning that it
  # will be slow, never a refusal (ADR-013).
  local lvp; lvp="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)"
  if [ -n "$lvp" ]; then
    o="$(VK_DRIVER_FILES="$lvp" $LAUNCHERX --host-check 2>&1)" \
      || { echo "a software driver was refused instead of warned about"; echo "$o"; rc=1; }
    case "$o" in *slow*) ;; *) echo "a software driver was not called slow"; echo "$o"; rc=1;; esac
    case "$o" in *"software, usable but slow"*) ;; *) echo "the software device was not listed as usable"; echo "$o"; rc=1;; esac
  fi
  return $rc
}
shelforder_check() { # the disc shelf is in order by label, all the way to the guest
  local rc=0 dir="$OUT/shelforder" bundle o shelf_file got want
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp shelved "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  # Added in an order nobody would want to read them in: mixed case, and a
  # numbered set whose tenth disc a string sort files between 1 and 2.
  for f in "zork.iso" "blood disc 10.cue" "Blood disc 2.cue" "aladdin.iso" "Blood disc 1.cue"; do
    : >"$dir/$f"
  done
  o="$($LAUNCHERX --discs add "$dir/zork.iso" "$dir/blood disc 10.cue" \
        "$dir/Blood disc 2.cue" "$dir/aladdin.iso" "$dir/Blood disc 1.cue")" \
    || { echo "--discs add failed"; rc=1; }
  want="aladdin Blood disc 1 Blood disc 2 blood disc 10 zork"
  got="$(printf '%s\n' "$o" | cut -f1 | tr '\n' ' ')"
  case "$got" in "$want "*) ;; *) echo "the shelf is not in order by label"; echo "  got:  $got"; echo "  want: $want"; rc=1;; esac
  # And the same order in the flat file the guest's own CDSHELF program
  # lists (patch 52). It is served by slot number, so the order the host
  # writes is the order the guest shows and the numbers a guest loads by.
  shelf_file="$($LAUNCHERX --discs publish "$(dirname "$bundle")" \
                | sed -n 's/^shelf published to //p')"
  if [ -n "$shelf_file" ] && [ -f "$shelf_file" ]; then
    got="$(cut -f1 "$shelf_file" | tr '\n' ' ')"
    case "$got" in "$want "*) ;; *) echo "the guest's shelf file is not in order by label"; echo "  got: $got"; rc=1;; esac
  else
    echo "no shelf file was published"; rc=1
  fi
  # A disc added later lands where its name belongs, not at the end.
  : >"$dir/Age of Empires.iso"
  o="$($LAUNCHERX --discs add "$dir/Age of Empires.iso" | cut -f1 | head -1)"
  [ "$o" = "Age of Empires" ] || { echo "a disc added later did not land in order (first row: $o)"; rc=1; }
  return $rc
}
drive_check() { # the shelf's one drive: the boot disc when stopped, the tray when running
  local rc=0 dir="$OUT/drive" bundle o sock qpid i
  rm -rf "$dir"; mkdir -p "$dir/library" "$dir/Patch 1.3"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  truncate -s 2M "$dir/zork.iso" "$dir/guest-tools-3dfx-0000000.iso"
  bundle="$($LAUNCHERX --new xp Drive "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  $LAUNCHERX --discs add "$dir/zork.iso" "$dir/Patch 1.3" "$dir/guest-tools-3dfx-0000000.iso" >/dev/null \
    || { echo "--discs add failed"; return 1; }
  # Stopped, empty: the card says so, and every row is a kind of its own.
  o="$($LAUNCHERX --drive "$bundle" 2>&1)" || { echo "--drive failed: $o"; return 1; }
  grep -qx $'running\tfalse' <<<"$o" || { echo "a stopped machine reads as running: $o"; rc=1; }
  grep -q $'^drive\tempty\tTray empty\tThe machine boots without a disc' <<<"$o" || { echo "empty card: $o"; rc=1; }
  grep -q $'^-\tdisc\tzork\tDisc image · ' <<<"$o" || { echo "zork's row: $o"; rc=1; }
  grep -q $'^-\tfolder\tPatch 1.3\tFolder · ' <<<"$o" || { echo "the folder's row: $o"; rc=1; }
  grep -q $'^-\ttools\tguest-tools-3dfx-0000000\tGuest tools · ' <<<"$o" || { echo "guest tools' row: $o"; rc=1; }
  # Insert on a stopped machine is the boot disc, written to the bundle.
  o="$($LAUNCHERX --drive "$bundle" insert "$dir/zork.iso" 2>&1)" || { echo "insert failed: $o"; return 1; }
  grep -q $'^drive\tdisc\tzork\t' <<<"$o" || { echo "the card after insert: $o"; rc=1; }
  grep -q $'^in-drive\tdisc\tzork\t' <<<"$o" || { echo "zork is not marked in the drive: $o"; rc=1; }
  grep -qx $'Storage\tCD in drive\tzork.iso' <<<"$($LAUNCHERX --machine-details "$bundle")" \
    || { echo "the bundle does not boot with zork"; rc=1; }
  # Running: the card is the tray, read from the monitor. A paused QEMU
  # with the machine's CD drive stands in for the player.
  if [ -x $QSYS ]; then
    sock="$($LAUNCHERX --qmp-socket "$bundle")"
    mkdir -p "$(dirname "$sock")"; rm -f "$sock"
    $QSYS -machine pc -S -display none -nodefaults \
      -drive "if=none,id=cd0,media=cdrom,file=$dir/zork.iso" -device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0 \
      -qmp "unix:$sock,server=on,wait=off" >"$dir/qemu.log" 2>&1 & qpid=$!
    for i in $(seq 100); do [ -S "$sock" ] && break; sleep 0.05; done
    o="$($LAUNCHERX --drive "$bundle" 2>&1)"
    grep -qx $'running\ttrue' <<<"$o" || { echo "a running machine reads as stopped: $o"; rc=1; }
    grep -q $'^drive\tdisc\tzork\t' <<<"$o" || { echo "the running card: $o"; rc=1; }
    # A folder goes in as isodir: and comes back out of query-block as the folder.
    o="$($LAUNCHERX --drive "$bundle" insert "$dir/Patch 1.3" 2>&1)"
    grep -q $'^drive\tfolder\tPatch 1.3\t' <<<"$o" || { echo "the card after a live folder insert: $o"; rc=1; }
    grep -q $'^in-drive\tfolder\tPatch 1.3\t' <<<"$o" || { echo "the folder is not marked in the drive: $o"; rc=1; }
    grep -qx $'Storage\tCD in drive\tPatch 1.3' <<<"$($LAUNCHERX --machine-details "$bundle")" \
      || { echo "a live insert did not set the boot disc too"; rc=1; }
    o="$($LAUNCHERX --drive "$bundle" eject 2>&1)"
    grep -q $'^drive\tempty\tTray empty\tInsert a disc from the library' <<<"$o" || { echo "the card after a live eject: $o"; rc=1; }
    grep -qx $'Storage\tCD in drive\tEmpty' <<<"$($LAUNCHERX --machine-details "$bundle")" \
      || { echo "a live eject did not empty the boot drive too"; rc=1; }
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
  else
    echo "  (no $QSYS: the running card is not checked)"
  fi
  return $rc
}
machinedetails_check() { # what the machine window shows of a chosen machine (doc 07)
  local rc=0 dir="$OUT/machinedetails" xp dos o groups
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/xp.qcow2"; : >"$dir/dos.qcow2"
  xp="$($LAUNCHERX --new xp "XP box" "$dir/xp.qcow2")" || { echo "--new xp failed"; return 1; }
  dos="$($LAUNCHERX --new dos "DOS box" "$dir/dos.qcow2")" || { echo "--new dos failed"; return 1; }
  o="$($LAUNCHERX --machine-details "$xp" 2>&1)" || { echo "--machine-details failed: $o"; return 1; }
  # The line under the name, then a group per page of the form with the
  # form's own labels: System, Storage, then the rest in the form's order.
  [ "$(head -1 <<<"$o")" = "XP · Stopped" ] || { echo "xp subtitle: $(head -1 <<<"$o")"; rc=1; }
  groups="$(tail -n +2 <<<"$o" | cut -f1 | uniq | tr '\n' ' ')"
  [ "$groups" = "System Storage Display Audio Input Network " ] || { echo "xp groups: $groups"; rc=1; }
  grep -qx $'Display\tDirect3D\tAutomatic' <<<"$o" || { echo "xp has no Direct3D row"; rc=1; }
  # The disk is its file and two folders: .../<OUT's name>/machinedetails/xp.qcow2.
  grep -qx "Storage"$'\t'"Hard disk"$'\t'".../$(basename "$OUT")/machinedetails/xp.qcow2" <<<"$o" \
    || { echo "xp's disk is not its file and two folders: $(grep 'Hard disk' <<<"$o")"; rc=1; }
  grep -qx $'Storage\tCD in drive\tEmpty' <<<"$o" || { echo "xp's drive is not empty"; rc=1; }
  # No Direct3D on a machine without our adapter, as the form hides it.
  o="$($LAUNCHERX --machine-details "$dos" 2>&1)" || { echo "--machine-details dos failed: $o"; return 1; }
  [ "$(head -1 <<<"$o")" = "DOS · Stopped" ] || { echo "dos subtitle: $(head -1 <<<"$o")"; rc=1; }
  grep -q $'\tDirect3D\t' <<<"$o" && { echo "dos shows a Direct3D row"; rc=1; }
  grep -qx $'System\tMemory\t64 MB' <<<"$o" || { echo "dos memory: $(grep Memory <<<"$o")"; rc=1; }
  return $rc
}

accelchoices_check() { # the acceleration picker offers hardware acceleration only where the host has it
  local rc=0 dir="$OUT/accelchoices" bundle o nokvm=(bwrap --bind / / --dev /dev)
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  # This host, as it is.
  o="$($LAUNCHERX --kvm 2>/dev/null)"
  if [ "$(head -1 <<<"$o")" = available ]; then
    grep -qx 'choices: Automatic, Hardware virtualization, Emulation' <<<"$o" || { echo "with KVM: $o"; rc=1; }
  else
    grep -qx 'choices: Automatic, Emulation' <<<"$o" || { echo "without KVM: $o"; rc=1; }
  fi
  # A host with no KVM at all: a fresh /dev, with no /dev/kvm in it.
  o="$("${nokvm[@]}" $LAUNCHERX --kvm 2>/dev/null)"
  [ "$(head -1 <<<"$o")" = "not available" ] || { echo "bwrap left /dev/kvm: $o"; return 1; }
  grep -qx 'choices: Automatic, Emulation' <<<"$o" || { echo "a new machine without KVM: $o"; rc=1; }
  # A machine already set to it keeps the entry, so the picker shows it.
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp "On KVM" "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  $LAUNCHERX --wizard-edit "$bundle" - - kvm >/dev/null 2>&1 || { echo "--wizard-edit kvm failed"; return 1; }
  o="$("${nokvm[@]}" $LAUNCHERX --kvm "$bundle" 2>/dev/null)"
  grep -qx 'choices: Automatic, Hardware virtualization, Emulation' <<<"$o" || { echo "a KVM machine without KVM: $o"; rc=1; }
  return $rc
}

clone_check() { # "Clone…", from the model to a disk our QEMU reads (doc 07)
  local rc=0 dir="$OUT/clone" img=$QIMG io=$QIO
  local bundle disk copy copy_disk o args outside twin sock qpid i
  rm -rf "$dir"; mkdir -p "$dir/library" "$dir/vms"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" LAUNCHER_QEMU_IMG_BIN="$img"
  # A machine the wizard made, disk and all, with something on the disk
  # and an internal snapshot inside it. (`--wizard-new` passes qemu-img's
  # "Formatting" line through, so the bundle is the last line.)
  bundle="$($LAUNCHERX --wizard-new xp Original 1 2>/dev/null | tail -1)"
  [ -f "$bundle" ] || { echo "--wizard-new made no bundle"; return 1; }
  disk="$(dirname "$bundle")/disk.qcow2"
  $io -c "write -P 0x5a 0 1M" "$disk" >/dev/null || { echo "qemu-io could not write the original"; return 1; }
  $LAUNCHERX --snapshots "$bundle" take before-clone >/dev/null 2>&1 \
    || { echo "--snapshots take failed"; return 1; }
  # No name: the one the window offers.
  copy="$($LAUNCHERX --clone "$bundle" 2>&1)" || { echo "--clone failed: $copy"; return 1; }
  copy_disk="$(dirname "$copy")/disk.qcow2"
  grep -qx 'name = "Original (copy)"' "$copy" || { echo "the clone is not called Original (copy):"; grep '^name' "$copy"; rc=1; }
  [ "$(dirname "$copy")" != "$(dirname "$bundle")" ] || { echo "the clone shares the original's directory"; rc=1; }
  args="$($LAUNCHERX --print-args "$copy")"
  case "$args" in *"file=$copy_disk,"*) ;; *) echo "the clone does not boot its own disk: $args"; rc=1;; esac
  cmp -s "$disk" "$copy_disk" || { echo "the clone's disk is not a copy of the original's"; rc=1; }
  o="$($LAUNCHERX --snapshots "$copy" 2>&1)"
  printf '%s\n' "$o" | grep -q before-clone || { echo "the snapshot did not come along: $o"; rc=1; }
  # Two machines from then on: what the clone writes, the original never sees.
  $io -c "write -P 0xa5 0 1M" "$copy_disk" >/dev/null || { echo "qemu-io could not write the clone"; rc=1; }
  o="$($io -r -c "read -P 0x5a 0 1M" "$disk" 2>&1)"
  case "$o" in *failed*|*rror*) echo "writing the clone changed the original: $o"; rc=1;; *"read 1048576/"*) ;; *) echo "$o"; rc=1;; esac
  o="$($io -r -c "read -P 0xa5 0 1M" "$copy_disk" 2>&1)"
  case "$o" in *failed*|*rror*) echo "the clone did not keep its own write: $o"; rc=1;; *"read 1048576/"*) ;; *) echo "$o"; rc=1;; esac
  # The name offered moves on when it is taken; a taken name or no name is refused.
  o="$($LAUNCHERX --clone "$bundle" 2>&1)" && grep -qx 'name = "Original (copy 2)"' "$o" \
    || { echo "a second clone was not offered Original (copy 2): $o"; rc=1; }
  o="$($LAUNCHERX --clone "$bundle" "Original (copy)" 2>&1)" \
    && { echo "a clone under a name already in the library was made: $o"; rc=1; }
  case "$o" in *"already a machine called"*) ;; *) echo "...and not refused for that: $o"; rc=1;; esac
  o="$($LAUNCHERX --clone "$bundle" " " 2>&1)" && { echo "a clone with no name was made"; rc=1; }
  case "$o" in *"name is required"*) ;; *) echo "...and not refused for that: $o"; rc=1;; esac
  # "Use the same hard disk": the clone boots the original's disk, copies
  # nothing of it, and carries the rest of the bundle (the snapshot tree).
  twin="$($LAUNCHERX --clone "$bundle" --same-disk "Same disk" 2>&1)" \
    || { echo "--clone --same-disk failed: $twin"; rc=1; }
  args="$($LAUNCHERX --print-args "$twin")"
  case "$args" in *"file=$(np "$(realpath "$disk")"),"*) ;; *) echo "the same-disk clone does not boot the original's disk: $args"; rc=1;; esac
  [ ! -e "$(dirname "$twin")/disk.qcow2" ] || { echo "the same-disk clone copied the disk"; rc=1; }
  [ -f "$(dirname "$twin")/snapshots.toml" ] || { echo "the same-disk clone left the snapshot record behind"; rc=1; }
  # A disk outside the library, as "Use an existing disk" leaves one, and
  # an overlay whose backing file is named relative to it: the copy lands
  # in the clone's own folder and still finds the backing file.
  $img create -q -f qcow2 "$dir/vms/base.qcow2" 16M && $io -c "write -P 0x33 0 64k" "$dir/vms/base.qcow2" >/dev/null \
    && $img create -q -f qcow2 -b base.qcow2 -F qcow2 "$dir/vms/overlay.qcow2" \
    || { echo "could not make the overlay"; return 1; }
  outside="$($LAUNCHERX --new xp Outside "$dir/vms/overlay.qcow2")"
  twin="$($LAUNCHERX --clone "$outside" "Outside twin" 2>&1)" || { echo "--clone (outside) failed: $twin"; return 1; }
  args="$($LAUNCHERX --print-args "$twin")"
  case "$args" in *"file=$(dirname "$twin")/overlay.qcow2,"*) ;; *) echo "the outside disk was not copied into the clone: $args"; rc=1;; esac
  o="$($img info --output=json "$(dirname "$twin")/overlay.qcow2" 2>&1)"
  printf '%s' "$o" | normp | grep -q "\"backing-filename\": \"$(np "$(realpath "$dir/vms/base.qcow2")")\"" \
    || { echo "the copy's backing file is not the original's, by absolute path:"; printf '%s\n' "$o" | grep backing; rc=1; }
  o="$($io -r -c "read -P 0x33 0 64k" "$(dirname "$twin")/overlay.qcow2" 2>&1)"
  case "$o" in *failed*|*rror*) echo "the copy does not read through to its backing file: $o"; rc=1;; *"read 65536/"*) ;; *) echo "$o"; rc=1;; esac
  $img info --output=json "$dir/vms/overlay.qcow2" | grep -q '"backing-filename": "base.qcow2"' \
    || { echo "the original overlay's header was changed"; rc=1; }
  # A running machine is refused: a QEMU listening on its monitor socket is
  # what a player that is up looks like, whoever started it.
  if [ -x $QSYS ]; then
    sock="$($LAUNCHERX --qmp-socket "$bundle")"
    mkdir -p "$(dirname "$sock")"; rm -f "$sock"
    $QSYS -machine none -S -display none -nodefaults \
      -qmp "unix:$sock,server=on,wait=off" >"$dir/qemu.log" 2>&1 & qpid=$!
    # The socket's own appearance, bounded: QEMU makes it before its main loop.
    for i in $(seq 100); do [ -S "$sock" ] && break; sleep 0.05; done
    o="$($LAUNCHERX --clone "$bundle" "While running" 2>&1)" && { echo "a running machine was cloned"; rc=1; }
    case "$o" in *"is running"*) ;; *) echo "...and not refused for that: $o"; rc=1;; esac
    [ ! -e "$dir/library/while-running" ] || { echo "a refused clone left a directory behind"; rc=1; }
    # Nothing of the disk is read for a same-disk clone, so that one goes ahead.
    o="$($LAUNCHERX --clone "$bundle" --same-disk "Same while running" 2>&1)" \
      || { echo "a same-disk clone of a running machine was refused: $o"; rc=1; }
    kill "$qpid" 2>/dev/null; wait "$qpid" 2>/dev/null
  else
    echo "  (no $QSYS: the running-machine refusal is not checked)"
  fi
  return $rc
}
win11snap_check() { # a Windows 11 machine's offline snapshot holds its firmware variables and TPM (M20)
  local rc=0 dir="$OUT/win11snap" img=$QIMG bundle bdir vars tpm copy
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" LAUNCHER_QEMU_IMG_BIN="$img"
  # The wizard's own machine, no guest: what the snapshot window does to
  # the files is the whole question. A fresh TPM is a file libtpms
  # writes on first start, so a stand-in is enough here.
  bundle="$($LAUNCHERX --wizard-new win11 W11 64 2>/dev/null | tail -1)"
  [ -f "$bundle" ] || { echo "--wizard-new made no bundle"; return 1; }
  bdir="$(dirname "$bundle")"; vars="$bdir/efivars.qcow2"; tpm="$bdir/tpm.permall"
  $LAUNCHERX --prepare "$bundle" || { echo "--prepare failed"; return 1; }
  [ -f "$vars" ] || { echo "--prepare made no $vars"; return 1; }
  printf 'first' > "$tpm"
  op() { $LAUNCHERX --snapshots "$bundle" "$@" >/dev/null 2>&1 || { echo "--snapshots $* failed"; return 1; }; }
  has() { "$img" snapshot -l "$vars" | grep -qF "  $1  "; }   # the name may have spaces
  op take "a b" || return 1
  has "a b" || { echo "the take put no snapshot on the variable store"; rc=1; }
  copy="$(ls "$bdir"/tpm-snapshots/*.permall 2>/dev/null)"
  [ -n "$copy" ] && [ "$(cat "$copy")" = first ] || { echo "the take kept no copy of the TPM's state"; rc=1; }
  printf 'second' > "$tpm"
  op restore "a b" || return 1
  [ "$(cat "$tpm")" = first ] || { echo "the restore did not put the TPM's state back (holds: $(cat "$tpm"))"; rc=1; }
  # Clone (user decision): the TPM is copied with the machine unless the
  # window's "new TPM" box is ticked, which leaves the state file and the
  # snapshots' copies of it behind.
  local clone
  clone="$($LAUNCHERX --clone "$bundle" "Same TPM" 2>/dev/null)" || { echo "--clone failed"; return 1; }
  cmp -s "$tpm" "$(dirname "$clone")/tpm.permall" || { echo "a plain clone did not copy the TPM's state"; rc=1; }
  [ -n "$(ls "$(dirname "$clone")"/tpm-snapshots 2>/dev/null)" ] || { echo "a plain clone did not copy the snapshot's TPM"; rc=1; }
  normp <"$clone" | grep -q "tpm_state = [\"']$(dirname "$clone")/tpm.permall[\"']" || { echo "the clone's tpm_state does not name its own file"; rc=1; }
  clone="$($LAUNCHERX --clone "$bundle" --new-tpm "New TPM" 2>/dev/null)" || { echo "--clone --new-tpm failed"; return 1; }
  [ ! -e "$(dirname "$clone")/tpm.permall" ] || { echo "a new-TPM clone copied the TPM's state"; rc=1; }
  [ -z "$(ls "$(dirname "$clone")"/tpm-snapshots 2>/dev/null)" ] || { echo "a new-TPM clone copied the snapshots' TPM"; rc=1; }
  [ -f "$(dirname "$clone")/efivars.qcow2" ] && [ -f "$(dirname "$clone")/disk.qcow2" ] || { echo "a new-TPM clone lost its disk or variables"; rc=1; }
  op delete "a b" || return 1
  has "a b" && { echo "the delete left the snapshot on the variable store"; rc=1; }
  [ -z "$(ls "$bdir"/tpm-snapshots 2>/dev/null)" ] || { echo "the delete left the TPM's copy"; rc=1; }
  [ $rc = 0 ] && echo "take, restore, clone and delete cover the variable store and the TPM"
  return $rc
}

snaptree_check() { # the snapshot window's tree (doc 07): the launcher's own record over a qcow2, which keeps none
  local rc=0 dir="$OUT/snaptree" img=$QIMG bundle disk copy o want
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" LAUNCHER_QEMU_IMG_BIN="$img"
  bundle="$($LAUNCHERX --wizard-new xp Tree 1 2>/dev/null | tail -1)"
  [ -f "$bundle" ] || { echo "--wizard-new made no bundle"; return 1; }
  disk="$(dirname "$bundle")/disk.qcow2"
  # The window's rows as the CLI prints them: the name set in by its
  # depth, and the last column "current" / "no record" / nothing.
  tree() { $LAUNCHERX --snapshots "$1" 2>/dev/null | awk -F'\t' '{ printf "%s%s%s|", $2, ($5 == "" ? "" : " "), $5 }'; }
  op() { $LAUNCHERX --snapshots "$bundle" "$@" >/dev/null 2>&1 || { echo "--snapshots $* failed"; return 1; }; }
  # a; b from a; back to a and c from a: b and c are siblings under a, and
  # the disk now descends from c. The flat list would read a, b, c.
  op take a && op take b && op restore a && op take c || return 1
  want="a|  b|  c current|"
  o="$(tree "$bundle")"; [ "$o" = "$want" ] || { echo "after a, b, restore a, c: got $o, wanted $want"; rc=1; }
  [ -f "$(dirname "$bundle")/snapshots.toml" ] || { echo "no snapshots.toml beside the bundle"; rc=1; }
  # d from c goes a level deeper; deleting c moves d up under a and the
  # present state stays d's.
  op take d || return 1
  want="a|  b|  c|    d current|"
  o="$(tree "$bundle")"; [ "$o" = "$want" ] || { echo "after d: got $o, wanted $want"; rc=1; }
  op delete c || return 1
  want="a|  b|  d current|"
  o="$(tree "$bundle")"; [ "$o" = "$want" ] || { echo "after deleting c: got $o, wanted $want"; rc=1; }
  # The clone carries the tree (every file beside the bundle is copied).
  copy="$($LAUNCHERX --clone "$bundle" 2>&1)" || { echo "--clone failed: $copy"; return 1; }
  o="$(tree "$copy")"; [ "$o" = "$want" ] || { echo "the clone's tree: got $o, wanted $want"; rc=1; }
  # Outside the launcher: a snapshot deleted by hand takes its record with
  # it on the next read, and one taken by hand has none, so it sits at the
  # top level marked so.
  $img snapshot -d b "$disk" && $img snapshot -c e "$disk" || { echo "qemu-img could not edit the disk"; return 1; }
  want="a|  d current|e no record|"
  o="$(tree "$bundle")"; [ "$o" = "$want" ] || { echo "after qemu-img -d b, -c e: got $o, wanted $want"; rc=1; }
  # Restoring one with no record makes it a root that the next take
  # hangs under, so the tree grows from there rather than staying flat.
  op restore e && op take f || return 1
  want="a|  d|e|  f current|"
  o="$(tree "$bundle")"; [ "$o" = "$want" ] || { echo "after restore e, f: got $o, wanted $want"; rc=1; }
  # Deleting the snapshot the disk descends from moves that up too.
  op delete f || return 1
  want="a|  d|e current|"
  o="$(tree "$bundle")"; [ "$o" = "$want" ] || { echo "after deleting f: got $o, wanted $want"; rc=1; }
  return $rc
}
shaderdefaults_check() { # the first-run shader offer and its starter profiles (doc 07)
  local rc=0 dir="$OUT/shaderdefaults" o preset n
  rm -rf "$dir"; mkdir -p "$dir/profiles" "$dir/empty"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  # Everything here except the 50 MB download, the one part that needs
  # the network. `shader_source::fetch` is the same code the profile
  # manager's button runs. What can go wrong quietly is the question's
  # *once-only* rule and the profiles written after a "yes".

  # A launcher with no collection asks, and says what it will do.
  export LAUNCHER_SHADERS_DIR="$dir/empty"
  o="$($LAUNCHERX --first-run status)" || { echo "--first-run status failed"; return 1; }
  case "$o" in asking:*) ;; *) echo "a launcher with no presets did not ask: $o"; rc=1;; esac
  case "$o" in *"$dir/empty"*) ;; *) echo "the question does not name where the collection would land"; echo "$o"; rc=1;; esac
  # It names the profiles it is offering, so the sentence and
  # `DEFAULT_PROFILES` cannot drift apart.
  for n in "CRT Aperture" "CRT Royale" "Apple II"; do
    case "$o" in *"$n"*) ;; *) echo "the question does not mention the $n profile"; rc=1;; esac
  done

  # A launcher that *has* one never asks, which is why nobody working in
  # a checkout sees this dialog (the submodule is a collection).
  o="$(LAUNCHER_SHADERS_DIR=third_party/slang-shaders $LAUNCHERX --first-run status)"
  [ "$o" = idle ] || { echo "a launcher with a collection asked anyway: $o"; rc=1; }

  # "Not now" is remembered, or the offer comes back on every start.
  $LAUNCHERX --first-run decline >/dev/null || { echo "--first-run decline failed"; rc=1; }
  [ -f "$dir/profiles/first-run.txt" ] || { echo "declining wrote no marker"; rc=1; }
  o="$($LAUNCHERX --first-run status)"
  [ "$o" = idle ] || { echo "the offer came back after being declined: $o"; rc=1; }

  # The other half of a "yes", against the collection this checkout has.
  # Three profiles, each naming a preset librashader really parses (a
  # profile pointing at a missing or unreadable `.slangp` is a parse error
  # deferred to whoever opens it), each at the preset's own defaults,
  # which is an *empty* override table.
  o="$($LAUNCHERX --default-profiles third_party/slang-shaders)" \
    || { echo "--default-profiles failed"; return 1; }
  # `-eq`, not `=`: BSD `wc` pads its count with spaces and the string
  # compare then fails on macOS for a library that is exactly right.
  [ "$(printf '%s\n' "$o" | wc -l)" -eq 3 ] || { echo "not three profiles: $o"; rc=1; }
  for n in crt-aperture crt-royale apple-ii; do
    if [ ! -f "$dir/profiles/$n.toml" ]; then echo "no $n.toml"; rc=1; continue; fi
    o="$(sed -n '/^\[params\]/,$p' "$dir/profiles/$n.toml" | grep -c '=' || true)"
    [ "$o" = 0 ] || { echo "$n came out with $o parameter overrides, not the preset's defaults"; rc=1; }
    preset="$(sed -n "s/^preset = [\"']\(.*\)[\"']\$/\1/p" "$dir/profiles/$n.toml")"
    case "$preset" in /*|[A-Za-z]:[\\/]*) ;; *) echo "$n's preset path is relative ($preset)"; rc=1;; esac
    $LAUNCHERX --list-shader-params "$preset" >/dev/null 2>&1 \
      || { echo "$n names something librashader will not parse: $preset"; rc=1; }
  done

  # And running it again adds nothing. `shader_library::create` would
  # otherwise deduplicate the *slug* and hand back a second "CRT Royale"
  # as `crt-royale-2`, and every second download or launcher start would
  # add more copies to the library.
  o="$($LAUNCHERX --default-profiles third_party/slang-shaders)"
  case "$o" in "(nothing to add"*) ;; *) echo "a second run added profiles again: $o"; rc=1;; esac
  [ "$(ls "$dir/profiles"/*.toml | wc -l)" -eq 3 ] || { echo "the profile library is not still three"; rc=1; }

  # The first download marks CRT Aperture as the library's default
  # (`shader_library::create_defaults`), and a machine on "(default)"
  # (a new one names no profile) plays through it: `--print-shader-args`
  # is the line the player gets. The default is one file,
  # `default-profile.txt`, so a front end and a hand edit agree.
  [ "$(cat "$dir/profiles/default-profile.txt" 2>/dev/null)" = crt-aperture ] \
    || { echo "the first download did not mark CRT Aperture as the default"; rc=1; }
  mkdir -p "$dir/library"
  bundle="$(LAUNCHER_LIBRARY_DIR="$dir/library" $LAUNCHERX --wizard-new win98 Tube 1 2>/dev/null | tail -1)"
  [ -f "$bundle" ] || { echo "--wizard-new made no bundle"; return 1; }
  if grep -q '^shader_profile = ' "$bundle"; then echo "a new machine names a profile instead of the default:"; grep shader "$bundle"; rc=1; fi
  o="$($LAUNCHERX --print-shader-args "$bundle")"
  case "$o" in "--shader "*crt-aperture.slangp) ;; *) echo "a machine on (default) does not play the default profile: $o"; rc=1;; esac
  # A machine that names a profile keeps it whatever the default is.
  $LAUNCHERX --assign-shader "$bundle" apple-ii
  o="$($LAUNCHERX --print-shader-args "$bundle")"
  case "$o" in *apple-monitor-II.slangp) ;; *) echo "a machine naming Apple II plays something else: $o"; rc=1;; esac
  $LAUNCHERX --assign-shader "$bundle" "(none)"
  # Moving the default moves every machine on it; clearing it is the
  # unshaded app default again, and the picker's first row says which.
  o="$($LAUNCHERX --default-shader-profile crt-royale)"
  [ "$o" = crt-royale ] || { echo "--default-shader-profile did not move the default: $o"; rc=1; }
  o="$($LAUNCHERX --print-shader-args "$bundle")"
  case "$o" in "--shader "*crt-royale.slangp) ;; *) echo "the machine did not follow the default to CRT Royale: $o"; rc=1;; esac
  # A second download (or `--default-profiles` by hand) never takes a
  # default the user chose.
  $LAUNCHERX --default-profiles third_party/slang-shaders >/dev/null
  [ "$($LAUNCHERX --default-shader-profile)" = crt-royale ] || { echo "a second run of the starters moved the default"; rc=1; }
  o="$($LAUNCHERX --default-shader-profile "(none)")"
  [ "$o" = "(none)" ] || { echo "clearing the default left: $o"; rc=1; }
  o="$($LAUNCHERX --print-shader-args "$bundle")"
  [ -z "$o" ] || { echo "with no default the machine still gets a shader: $o"; rc=1; }
  # A default whose profile is gone (deleted by hand here; the
  # window's Delete also clears the file) is no default.
  $LAUNCHERX --default-shader-profile crt-royale >/dev/null
  rm "$dir/profiles/crt-royale.toml"
  [ "$($LAUNCHERX --default-shader-profile)" = "(none)" ] || { echo "a deleted default profile is still the default"; rc=1; }
  return $rc
}
mitsuami_check() { # the launcher's window, driven through its probes (doc 07, track M19)
  local rc=0 dir="$OUT/mitsuami" bin="$LAUNCHER_BIN" o broadway=""
  rm -rf "$dir"; mkdir -p "$dir/library" "$dir/profiles" "$dir/empty"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles" LAUNCHER_SHADERS_DIR="$dir/empty"
  # GTK draws on a private Broadway display (`gtk4-broadwayd`), so nothing
  # opens on the desktop and the suite runs with no desktop at all. AppKit
  # has no such mode: on a Mac each probe's window shows for a moment.
  if [ "$OS" = Linux ]; then
    command -v gtk4-broadwayd >/dev/null || { echo "no gtk4-broadwayd (GTK 4's own tools)"; return 1; }
    mkdir -m700 "$dir/run"
    export XDG_RUNTIME_DIR="$dir/run" GDK_BACKEND=broadway BROADWAY_DISPLAY=":$((20 + RANDOM % 50))" GTK_USE_PORTAL=0
    gtk4-broadwayd "$BROADWAY_DISPLAY" >/dev/null 2>&1 &
    broadway=$!
    for _ in $(seq 50); do [ -n "$(ls -A "$dir/run")" ] && break; sleep 0.1; done
  fi
  # `LAUNCHER_SCREEN` picks the window and what it does, `LAUNCHER_SHOT`
  # draws it into a PNG and exits (launcher-mitsuami/src/shot.rs).
  probe() { timeout 120 env LAUNCHER_SCREEN="$1" LAUNCHER_SHOT="$dir/$2.png" "$bin" 2>&1; }

  # The machine window, and a picture of it.
  o="$(probe "" main)"
  [ -s "$dir/main.png" ] || { echo "the machine window drew nothing: $o"; rc=1; }
  # A fresh form filled on an existing disk and submitted, then the machine
  # window with the new row: the wizard's whole path through the window.
  o="$(probe "create:xp:Probe box" create)"
  printf '%s' "$o" | grep -q "create: saved Some(" || { echo "the form saved nothing: $o"; rc=1; }
  [ -f "$dir/library/probe-box/machine.toml" ] || { echo "no probe-box/machine.toml in the library"; rc=1; }
  # The first-run offer on a launcher with no presets, answered No
  # through the platform's own alert: asked with the shared model's words,
  # and never again (the marker).
  o="$(probe firstrun:no firstrun)"
  printf '%s\n' "$o" | grep '^\[launcher\] firstrun' | sed 's/^/  /'
  printf '%s' "$o" | grep -q "firstrun Asking: No CRT shader presets are installed yet" \
    || { echo "the offer did not ask with the model's headline"; rc=1; }
  printf '%s' "$o" | grep -q "firstrun settled: open=false" || { echo "No did not settle the offer"; rc=1; }
  [ -f "$dir/profiles/first-run.txt" ] || { echo "declining wrote no marker"; rc=1; }
  # About, with the credits.
  o="$(probe about about)"
  [ -s "$dir/about.png" ] || { echo "About drew nothing: $o"; rc=1; }

  if [ -n "$broadway" ]; then kill "$broadway" 2>/dev/null; wait "$broadway" 2>/dev/null; fi
  return $rc
}

dirshelf_check() { # a shared folder as a disc, from the shelf to a real QEMU (M5g)
  local rc=0 dir="$OUT/dirshelf" bundle args o spaced comma plain shelf_file
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  # Three folders, because the awkward parts of a folder name are what
  # this is about: a space, a comma (which is what separates options in a
  # QEMU option string), and one plain one to compare against. They go
  # through the launcher's own headless verbs, the code the shelf
  # window's buttons run.
  spaced="$dir/Shared Files"; mkdir -p "$spaced"; echo hello > "$spaced/README.TXT"
  comma="$dir/Doom,Quake"; mkdir -p "$comma"; echo hi > "$comma/GAME.TXT"
  plain="$dir/patch13"; mkdir -p "$plain"; echo p > "$plain/PATCH.TXT"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp folders "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }

  # On the shelf a folder is labelled by its own name, extension and all
  # (`patch13` would lose its .3 to a file stem).
  o="$($LAUNCHERX --discs add "$spaced" "$comma" "$plain")" || { echo "--discs add failed"; rc=1; }
  for want in "Shared Files	$spaced" "Doom,Quake	$comma" "patch13	$plain"; do
    case "$o" in *"$want"*) ;; *) echo "not on the shelf under its own name: $want"; echo "$o"; rc=1;; esac
  done

  # The flat file the guest's own CDSHELF program reads (patch 52) names a
  # folder the way QEMU has to be told to open one, and only that way.
  shelf_file="$($LAUNCHERX --discs publish "$(dirname "$bundle")" \
                | sed -n 's/^shelf published to //p')"
  if [ -n "$shelf_file" ] && [ -f "$shelf_file" ]; then
    grep -q "	isodir:$spaced\$" "$shelf_file" || { echo "the shelf file does not name the folder as isodir:"; cat "$shelf_file"; rc=1; }
    grep -q "	$spaced\$" "$shelf_file" && { echo "the shelf file names the folder as a plain path too"; rc=1; }
  else
    echo "no shelf file was published"; rc=1
  fi

  # The boot drive: the prefix, and a comma written twice so that the
  # option string survives being parsed.
  $LAUNCHERX --boot-disc "$bundle" "$spaced" >/dev/null || { echo "--boot-disc failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"file=isodir:$spaced "*) ;; *) echo "the boot drive does not name the folder as isodir:"; echo "$args"; rc=1;; esac
  $LAUNCHERX --boot-disc "$bundle" "$comma" >/dev/null || { echo "--boot-disc (comma) failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"file=isodir:$dir/Doom,,Quake "*) ;; *) echo "the comma in the path is not doubled"; echo "$args"; rc=1;; esac

  # And the point of all of it: our QEMU opens a folder as a disc, and the
  # doubled comma reaches it as one path rather than an unknown option.
  # Only the space-free folders are run: this check has to re-split a flat
  # command line in the shell, which the launcher itself never does (it
  # spawns an argv), so a space in a path is the harness's limit and not
  # the product's.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for d in "$plain" "$comma"; do
      $LAUNCHERX --boot-disc "$bundle" "$d" >/dev/null || rc=1
      args="$($LAUNCHERX --print-args "$bundle")"
      # shellcheck disable=SC2086
      o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
           | timeout 30 $QSYS_PIPE $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused the folder $d"; echo "$o" | tail -3; rc=1; }
    done
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

pad_check() { # the gamepad host end (M13 step 0) and the machine setting behind it
  local rc=0 dir="$OUT/pad" bundle dos args o held
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp pad "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  dos="$($LAUNCHERX --new dos pad-dos "$dir/disk.qcow2")" || { echo "--new dos failed"; return 1; }
  # A new machine ignores a controller, on every family. A stick that
  # rests a little off centre would otherwise hold an arrow key down on a
  # desktop nobody was playing a game on.
  for b in "$bundle" "$dos"; do
    grep -q '^pad = "none"' "$b" || { echo "a new machine did not come out with the pad off"; grep '^pad' "$b"; rc=1; }
  done
  # Neither `none` nor `keys` is a device, so neither may add anything to
  # the *guest's* command line (`usb` and `gameport` are, below).
  args="$($LAUNCHERX --print-args "$bundle")"
  local before="$args"
  # A machine with the pad off says nothing to the player either.
  o="$($LAUNCHERX --print-player-args "$bundle")"
  [ -z "$o" ] || { echo "a machine with the pad off still passed the player something: $o"; rc=1; }
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - keys >/dev/null \
    || { echo "--wizard-edit keys failed"; rc=1; }
  grep -q '^pad = "keys"' "$bundle" || { echo "the pad setting did not stick"; grep '^pad' "$bundle"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  [ "$args" = "$before" ] || { echo "turning the gamepad on changed the QEMU command line"; diff <(echo "$before") <(echo "$args"); rc=1; }
  # ...and the whole of what it does say is the setting (path C).
  o="$($LAUNCHERX --print-player-args "$bundle")"
  [ "$o" = "--pad keys" ] || { echo "expected '--pad keys' for the player, got: $o"; rc=1; }
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - none >/dev/null \
    || { echo "--wizard-edit none failed"; rc=1; }
  grep -q '^pad = "none"' "$bundle" || { echo "turning it back off did not stick"; rc=1; }
  # A bundle from a later launcher, naming a setting this build has never
  # heard of. It must load and fall back, not refuse the whole machine
  # over a field about a controller. The value has to be one no build
  # knows, or this stops testing anything (`gameport` was once the
  # placeholder, until path B made it real).
  sed -i 's/^pad = "none"/pad = "wheel"/' "$bundle"
  args="$($LAUNCHERX --print-args "$bundle" 2>&1)" \
    || { echo "a bundle naming a future pad setting would not load at all"; echo "$args"; rc=1; }
  case "$args" in *usb-gamepad*) echo "an unknown pad setting was treated as usb"; rc=1;; esac
  case "$args" in *"-device gameport"*) echo "an unknown pad setting was treated as a gameport"; rc=1;; esac

  # --- path A: the USB HID gamepad --------------------------------
  # DOS is not offered one: it has no USB stack, so the entry is absent
  # the way the display picker omits an adapter a family has no driver
  # for, rather than being offered and then warned about.
  $LAUNCHERX --wizard-edit "$dos" - - - - - - - - usb >/dev/null 2>&1
  grep -q '^pad = "usb"' "$dos" && { echo "a DOS machine accepted the USB gamepad"; rc=1; }
  args="$($LAUNCHERX --print-args "$dos")"
  case "$args" in *usb-gamepad*) echo "a DOS machine got a usb-gamepad"; echo "$args"; rc=1;; esac
  # A Windows machine gets the device, and it is passed to the player too.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - usb >/dev/null \
    || { echo "--wizard-edit usb failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-device usb-gamepad"*) ;; *) echo "an XP machine with the pad on has no usb-gamepad"; echo "$args"; rc=1;; esac
  o="$($LAUNCHERX --print-player-args "$bundle")"
  [ "$o" = "--pad usb" ] || { echo "expected '--pad usb' for the player, got: $o"; rc=1; }
  # The controller comes with it even when the pointer does not want one:
  # a `-device usb-gamepad` with no bus to attach to is a machine that
  # will not start, and turning the seamless mouse off used to take the
  # whole USB bus away with the tablet.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - noseamless - - >/dev/null \
    || { echo "--wizard-edit noseamless failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *usb-tablet*) echo "the tablet survived turning the seamless mouse off"; echo "$args"; rc=1;; esac
  case "$args" in *"-usb"*) ;; *) echo "the pad lost its USB controller when the pointer gave one up"; echo "$args"; rc=1;; esac
  case "$args" in *"-device usb-gamepad"*) ;; *) echo "the pad went away with the tablet"; echo "$args"; rc=1;; esac

  # --- path B: the gameport ----------------------------------------
  # The mirror image of path A's asymmetry, and the point of the whole
  # path: DOS is the family that cannot have the USB pad and *can* have
  # this, so a DOS machine takes it...
  $LAUNCHERX --wizard-edit "$dos" - - - - - - - - gameport >/dev/null \
    || { echo "--wizard-edit gameport failed on DOS"; rc=1; }
  grep -q '^pad = "gameport"' "$dos" || { echo "a DOS machine would not take the gameport"; grep '^pad' "$dos"; rc=1; }
  args="$($LAUNCHERX --print-args "$dos")"
  case "$args" in *"-device gameport"*) ;; *) echo "a DOS machine with the gameport on has no gameport"; echo "$args"; rc=1;; esac
  # ...and it brings no USB controller with it. The port is an ISA device
  # and a DOS guest has no USB stack to drive one with anyway; a stray
  # `-usb` here would be a device in the machine nothing can use.
  case "$args" in *"-usb"*) echo "the gameport dragged a USB controller in"; echo "$args"; rc=1;; esac
  o="$($LAUNCHERX --print-player-args "$dos")"
  [ "$o" = "--pad gameport" ] || { echo "expected '--pad gameport' for the player, got: $o"; rc=1; }
  # ...while XP is not offered it and must refuse it rather than write a
  # port its guest has no way to enumerate: XP's answer is path A.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - gameport >/dev/null 2>&1
  grep -q '^pad = "gameport"' "$bundle" && { echo "an XP machine accepted the gameport"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-device gameport"*) echo "an XP machine got a gameport"; echo "$args"; rc=1;; esac
  # Win98 is the family with both stacks and is offered both, one at a
  # time: choosing the gameport takes the HID pad *and* its controller
  # away, or the machine would carry two controllers for one host pad and
  # only one of them would ever move.
  local w98
  w98="$($LAUNCHERX --new win98 pad-98 "$dir/disk.qcow2")" || { echo "--new win98 failed"; return 1; }
  for p in usb gameport; do
    $LAUNCHERX --wizard-edit "$w98" - - - - - - - - "$p" >/dev/null \
      || { echo "--wizard-edit $p failed on win98"; rc=1; }
    grep -q "^pad = \"$p\"" "$w98" || { echo "a Win98 machine would not take $p"; grep '^pad' "$w98"; rc=1; }
  done
  args="$($LAUNCHERX --print-args "$w98")"
  case "$args" in *"-device gameport"*) ;; *) echo "a Win98 machine with the gameport on has no gameport"; echo "$args"; rc=1;; esac
  case "$args" in *usb-gamepad*) echo "the Win98 machine kept its usb-gamepad after switching to the gameport"; echo "$args"; rc=1;; esac
  # The host end itself, against the scripted pad, with no controller,
  # guest or window. What it proves is the shaping. The deadzone
  # swallows a resting stick, and the press/release pair has a gap in it
  # so an axis held between them does not chatter.
  if [ -x $PLAYER ]; then
    # raw 0.25 is inside the 0.30 deadzone and must produce nothing at all.
    o="$(PLAYER_PAD_SCRIPT='5:lx=0.25' $PLAYER --pad-sweep 10 2>&1)" || { echo "$o"; rc=1; }
    case "$o" in *"0 events"*) ;; *) echo "a stick inside the deadzone produced an event"; echo "$o"; rc=1;; esac
    # The hysteresis, as three readings: 0.450 shaped is under the press
    # threshold, 0.600 is over it, and 0.450 *again* must stay held. One
    # threshold instead of two would release on the third and the guest
    # would see a key repeating at the poll rate.
    o="$(PLAYER_PAD_SCRIPT='5:lx=0.615,10:lx=0.72,15:lx=0.615' $PLAYER --pad-sweep 20 2>&1)" || { echo "$o"; rc=1; }
    # Per line, not over the whole output. A glob across it matches the
    # word "press" from any *other* frame's line and the check passes for
    # the wrong reason.
    echo "$o" | grep -q '^\[pad\] frame 5 lx .* press$' \
      && { echo "an axis under the press threshold was called pressed"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^\[pad\] frame 10 lx .* press$' \
      || { echo "an axis over the press threshold was not pressed"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^\[pad\] frame 15 lx .* release$' \
      && { echo "an axis inside the hysteresis band chattered"; echo "$o"; rc=1; }
    # `lx+`, not `lx`: the state is per *half*, because the two ends of a
    # stick are two different keys (path C) and a magnitude test cannot
    # tell them apart.
    held="$(echo "$o" | sed -n 's/^pad-sweep: held at end: //p')"
    [ "$held" = "lx+" ] || { echo "expected lx+ still held at the end, got: $held"; echo "$o"; rc=1; }
    # A malformed script is refused loudly. A typo here otherwise reads
    # exactly like a pad that does not work.
    for bad in '5:nope=1' '5:lx=2.0' '5:south=0.5' 'bad'; do
      if o="$(PLAYER_PAD_SCRIPT="$bad" $PLAYER --pad-sweep 10 2>&1)"; then
        echo "PLAYER_PAD_SCRIPT=$bad was accepted"; echo "$o"; rc=1
      fi
    done
    # And that the binary can say what it can read, which is the only
    # place a sandbox with no input access reports itself.
    o="$($PLAYER --pads 2>&1)" || { echo "--pads failed"; echo "$o"; rc=1; }
    case "$o" in gamepads:*) ;; *) echo "--pads said something unexpected: $o"; rc=1;; esac

    # --- path C: the pad presses keys -------------------------------
    # Two controls on one key. The default map puts both the d-pad and
    # the left stick on the arrows, so `left` has two holders: pressing
    # the second must not press the key again, and releasing the *first*
    # must not release it. Counting instead of unioning gets this wrong,
    # and the guest is left with an arrow key stuck down.
    o="$(PLAYER_PAD=keys PLAYER_PAD_SCRIPT='5:dpad_left=1,10:lx=-1.0,15:dpad_left=0,20:lx=0.0' \
         $PLAYER --pad-sweep 25 2>&1)" || { echo "$o"; rc=1; }
    [ "$(echo "$o" | grep -c '^pad-key: ')" = 2 ] \
      || { echo "two controls on one key did not produce exactly one down and one up"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 5 left .* down$' \
      || { echo "the first holder did not press the key"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 20 left .* up$' \
      || { echo "the key was not released when the last holder let go"; echo "$o"; rc=1; }
    # A stick swung across centre inside one poll: the key being left has
    # to go up *before* the key being entered goes down, or a guest that
    # samples between them sees both arrows held.
    o="$(PLAYER_PAD=keys PLAYER_PAD_SCRIPT='5:lx=-1.0,10:lx=1.0,15:lx=0.0' \
         $PLAYER --pad-sweep 20 2>&1)" || { echo "$o"; rc=1; }
    [ "$(echo "$o" | grep '^pad-key: frame 10 ' | head -1 | grep -c ' left .* up$')" = 1 ] \
      || { echo "crossing centre did not release the old direction first"; echo "$o"; rc=1; }
    [ "$(echo "$o" | grep '^pad-key: frame 10 ' | tail -1 | grep -c ' right .* down$')" = 1 ] \
      || { echo "crossing centre did not press the new direction"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-sweep: keys down at end: none$' \
      || { echo "a script that let go left keys down in the guest"; echo "$o"; rc=1; }
    # The whole default map reaches real scancodes. `up` is the one to
    # check by number: the stick's negative Y is up on the screen and +1
    # on the wire, so a missing flip here sends the guest `down`.
    o="$(PLAYER_PAD=keys PLAYER_PAD_SCRIPT='2:ly=-1.0,4:south=1' \
         $PLAYER --pad-sweep 6 2>&1)" || { echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 2 up 0xe048 down$' \
      || { echo "stick up did not send the up arrow (0xe048)"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: frame 4 ctrl 0x001d down$' \
      || { echo "the bottom face button did not send ctrl (0x1d)"; echo "$o"; rc=1; }
    # With the pad off nothing is mapped, whatever the controller does.
    o="$(PLAYER_PAD_SCRIPT='5:south=1' $PLAYER --pad-sweep 10 2>&1)" || { echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-key: ' \
      && { echo "a machine with the pad off still pressed a key"; echo "$o"; rc=1; }

    # --- path A: the report the guest is handed -----------------------
    # The packing, without a guest: this is the same hid_state() the
    # player sends through qemu_embed_pad_state, so what is checked here
    # is the bytes a driver would parse.
    o="$(PLAYER_PAD=usb PLAYER_PAD_SCRIPT='3:lx=1.0,6:ly=-1.0,9:dpad_up=1,12:dpad_right=1,15:dpad_up=0,18:south=1,21:start=1' \
         $PLAYER --pad-sweep 24 2>&1)" || { echo "$o"; rc=1; }
    # A pad nobody has touched reads centred with the hat released. A
    # driver that never gets this shows the stick in a corner.
    echo "$o" | grep -q '^pad-hid: frame 1 axes 80 80 80 80 hat 8 buttons 000000000000$' \
      || { echo "the pad does not start centred with the hat released"; echo "$o"; rc=1; }
    # Stick up is a *low* Y: the screen convention, which is what a guest
    # expects. The sign is flipped once, in the gilrs source; getting it
    # wrong here inverts every game's steering.
    echo "$o" | grep -q '^pad-hid: frame 6 axes ff 01 80 80 hat 8 ' \
      || { echo "stick right/up did not give X=ff, Y=01"; echo "$o"; rc=1; }
    # The hat walks north -> north-east -> east as the d-pad is pressed.
    echo "$o" | grep -q '^pad-hid: frame 9 .* hat 0 ' || { echo "d-pad up is not hat 0"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 12 .* hat 1 ' || { echo "d-pad up+right is not hat 1"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 15 .* hat 2 ' || { echo "d-pad right is not hat 2"; echo "$o"; rc=1; }
    # Buttons land where gamepad::HID_BUTTONS says: south is button 1
    # (bit 0), start is button 10 (bit 9). This order is what a person
    # sees in joy.cpl and what every configured game is bound against.
    echo "$o" | grep -q '^pad-hid: frame 18 .* buttons 000000000001$' \
      || { echo "the bottom face button is not button 1"; echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 21 .* buttons 001000000001$' \
      || { echo "start is not button 10"; echo "$o"; rc=1; }
    # Opposite directions cancel. A real d-pad cannot press both, and a
    # guest handed "north and south" has to invent an answer.
    o="$(PLAYER_PAD=usb PLAYER_PAD_SCRIPT='3:dpad_up=1,6:dpad_down=1' \
         $PLAYER --pad-sweep 8 2>&1)" || { echo "$o"; rc=1; }
    echo "$o" | grep -q '^pad-hid: frame 6 .* hat 8 ' \
      || { echo "up and down together did not cancel to the null position"; echo "$o"; rc=1; }
  else
    echo "  (no $PLAYER: the machine setting was checked, the host end was not)"
  fi

  # The HID report descriptor: the bytes a guest's driver parses, which
  # nothing on this side reads, so a wrong one shows up only as a device
  # that enumerates and has no axes.
  if command -v python3 >/dev/null; then
    python3 tools/hid-descriptor-check.py >"$OUT/pad-hid-desc.log" 2>&1 \
      || { echo "the usb-gamepad report descriptor is wrong"; cat "$OUT/pad-hid-desc.log"; rc=1; }
    # ...and that the checker can still fail, which is the only thing
    # that makes the line above worth anything. Two mutations, each of
    # which produces a device that looks fine and is not.
    local mut="$dir/hid"; mkdir -p "$mut"
    sed 's/0x81, 0x42,/0x81, 0x02,/' gamepad/qemu/dev-gamepad.c >"$mut/nonull.c"
    sed 's/0x95, 0x04,/0x95, 0x03,/' gamepad/qemu/dev-gamepad.c >"$mut/short.c"
    for m in nonull short; do
      if python3 tools/hid-descriptor-check.py "$mut/$m.c" >/dev/null 2>&1; then
        echo "the descriptor checker passed a deliberately broken descriptor ($m)"; rc=1
      fi
    done
  fi

  # And the point of all of it: our own QEMU takes the machine, and the
  # device really attaches to the bus rather than merely being accepted
  # on the command line. `info usb` is the guest's own view of it.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    args="$($LAUNCHERX --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"human-monitor-command","arguments":{"command-line":"info usb"}}\n{"execute":"quit"}\n' \
         | timeout 30 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused a machine with a usb-gamepad"; echo "$o" | tail -3; rc=1; }
    case "$o" in *"2ksbox USB Gamepad"*) ;; *) echo "the usb-gamepad did not attach to the bus"; echo "$o" | tail -5; rc=1;; esac
    # A second one is refused outright rather than silently ignored: the
    # device drives a single host pad, and two would leave whichever the
    # lookup found first as the only live one.
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 $QSYS_PIPE $args -device usb-gamepad \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)"
    case "$o" in *"only one usb-gamepad"*) ;; *) echo "a second usb-gamepad was not refused"; echo "$o" | tail -3; rc=1;; esac

    # --- path B: the port, as a guest reads it ----------------------
    # The DOS machine's own line, and then the port itself through the
    # human monitor, which is the one way to read 0x201 with no guest.
    # Three readings, and the middle one is the whole timing model:
    #
    #   idle          f0   the four one-shots expired, no button held.
    #                      Not ff, which is what an *absent* port reads
    #                      off the open bus, and a game uses it to
    #                      decide there is no joystick.
    #   armed         ff   every axis still charging. Read with the VM
    #                      stopped, where the virtual clock does not
    #                      move at all, so this is exact rather than a
    #                      race against a 576 us pulse.
    #   a second on   f0   they end. A model that armed and never
    #                      expired would leave every game counting to
    #                      its own timeout, which reads as a stick
    #                      jammed at one extreme.
    args="$($LAUNCHERX --print-args "$dos")"
    # shellcheck disable=SC2086
    o="$({ echo 'i /b 0x201'; echo 'o /b 0x201 0'; echo 'i /b 0x201'; echo cont; sleep 1; \
           echo 'i /b 0x201'; echo 'info qtree'; echo quit; } \
         | timeout 40 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -monitor stdio -serial none 2>&1)" \
      || { echo "our QEMU refused a machine with a gameport"; echo "$o" | tail -3; rc=1; }
    case "$o" in *"dev: gameport"*) ;; *) echo "the gameport did not attach to the bus"; echo "$o" | tail -5; rc=1;; esac
    local reads
    reads="$(echo "$o" | grep -o 'portb\[0x0201\] = 0x[0-9a-f]*' | sed 's/.*= //' | tr '\n' ' ')"
    [ "$reads" = "0xf0 0xff 0xf0 " ] \
      || { echo "the gameport's one-shots read wrong (idle/armed/expired): $reads"; rc=1; }
    # And one at a time, for the reason the USB pad is: the host drives a
    # single controller, and two ports would answer the same addresses.
    # shellcheck disable=SC2086
    o="$(printf 'quit\n' \
         | timeout 30 $QSYS_PIPE $args -device gameport \
             -audiodev none,id=embed0 -display none -S -monitor stdio -serial none 2>&1)"
    case "$o" in *"only one gameport"*) ;; *) echo "a second gameport was not refused"; echo "$o" | tail -3; rc=1;; esac
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

voodoo2_check() { # the wizard's Voodoo 2 switch (doc 21), from a checkbox to a real QEMU
  local rc=0 dir="$OUT/voodoo2" bundle dos args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new win98 voodoo2 "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  dos="$($LAUNCHERX --new dos voodoo2-dos "$dir/disk.qcow2")" || { echo "--new dos failed"; return 1; }
  # Off unless picked, on every family: a card the guest has no driver
  # for is a New Hardware wizard on every boot.
  for b in "$bundle" "$dos"; do
    args="$($LAUNCHERX --print-args "$b")"
    # (`-device voodoo2`, not the bare name: the scratch disk's path has it)
    case "$args" in *"-device voodoo2"*) echo "a new machine has a Voodoo 2 nobody picked"; echo "$args"; rc=1;; esac
  done
  # The switch through the real form, on the Win98 machine (our adapter
  # + the chip is the pairing) and on DOS (3dfx's own overlay).
  for b in "$bundle" "$dos"; do
    $LAUNCHERX --wizard-edit "$b" - - - - - - - - - voodoo >/dev/null \
      || { echo "--wizard-edit voodoo failed on $b"; rc=1; }
    args="$($LAUNCHERX --print-args "$b")"
    case "$args" in *"-device voodoo2,addr=0x05"*) ;; *) echo "picking the Voodoo 2 added no device"; echo "$args"; rc=1;; esac
    # ...and the bundle says so in the field a newer launcher reads back
    grep -q '^voodoo2 = true' "$b" || { echo "the bundle does not record the card"; rc=1; }
    # The card's dither undone (doc 21 §12) is a setting of the card's,
    # so it is off with the card just picked, it reaches the device as a
    # property of that same -device, and it goes away with the card
    # rather than staying behind as a line nothing reads.
    case "$args" in *"undither=on"*) echo "picking the card turned its undither on too"; echo "$args"; rc=1;; esac
    $LAUNCHERX --wizard-edit "$b" - - - - - - - - - voodoo-undither >/dev/null \
      || { echo "--wizard-edit voodoo-undither failed on $b"; rc=1; }
    args="$($LAUNCHERX --print-args "$b")"
    case "$args" in *"-device voodoo2,addr=0x05,undither=on"*) ;;
      *) echo "the undither did not reach the card's device"; echo "$args"; rc=1;; esac
    grep -q '^voodoo2_undither = true' "$b" || { echo "the bundle does not record the undither"; rc=1; }
    $LAUNCHERX --wizard-edit "$b" - - - - - - - - - novoodoo >/dev/null \
      || { echo "--wizard-edit novoodoo failed on $b"; rc=1; }
    args="$($LAUNCHERX --print-args "$b")"
    case "$args" in *undither*) echo "the undither outlived the card"; echo "$args"; rc=1;; esac
    grep -q '^voodoo2_undither = true' "$b" && { echo "the bundle kept an undither with no card"; rc=1; }
    # and back on, so the rest of the check has the card it expects
    $LAUNCHERX --wizard-edit "$b" - - - - - - - - - voodoo >/dev/null \
      || { echo "--wizard-edit voodoo failed on $b"; rc=1; }
  done
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - novoodoo >/dev/null \
    || { echo "--wizard-edit novoodoo failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-device voodoo2"*) echo "turning the Voodoo 2 off left it on the bus"; echo "$args"; rc=1;; esac
  # Our QEMU accepts the DOS machine with the card, and the card is on
  # its bus: started paused, asked over QMP, told to quit.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    args="$($LAUNCHERX --print-args "$dos")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"query-pci"}\n{"execute":"quit"}\n' \
         | timeout 30 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the machine with a Voodoo 2"; echo "$o" | tail -3; rc=1; }
    case "$o" in *'"vendor": 4634'*) ;; *) echo "no 3dfx (121a) function on the bus"; echo "$o" | tail -3; rc=1;; esac
  else
    echo "  (no build/qemu: the QEMU half skipped)"
  fi
  return $rc
}

extra_args_check() { # the form's "Extra QEMU arguments" field, from the line to a real QEMU
  local rc=0 dir="$OUT/extra-args" bundle args o list
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp extra "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-global"*) echo "a new machine has extra arguments nobody typed"; echo "$args"; rc=1;; esac
  grep -q '^extra_qemu_args' "$bundle" && { echo "a new machine's bundle has an extra_qemu_args entry"; rc=1; }
  # Through the real form (`--wizard-edit`'s last field), with a quoted
  # argument: one list entry per argument, the quotes gone.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - \
      '-global d3dpt-vga.ddflags=32768 -name "extra args"' >/dev/null \
    || { echo "--wizard-edit with extra arguments failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *" -global d3dpt-vga.ddflags=32768 -name extra args") ;;
    *) echo "the extra arguments are not at the end of the command line"; echo "$args"; rc=1;; esac
  # (whitespace squeezed out: the TOML writer puts a list on many lines)
  list='extra_qemu_args=["-global","d3dpt-vga.ddflags=32768","-name","extraargs",]'
  [ "$(tr -d ' \n' <"$bundle" | grep -o 'extra_qemu_args=\[[^]]*\]')" = "$list" ] \
    || { echo "the bundle does not hold the arguments as a list"; grep -A6 extra_qemu_args "$bundle"; rc=1; }
  # An edit of another field reads the line back out of the bundle and
  # writes it again, so the quoting has to round-trip.
  $LAUNCHERX --wizard-edit "$bundle" - 1024 >/dev/null || { echo "--wizard-edit of the memory failed"; rc=1; }
  [ "$(tr -d ' \n' <"$bundle" | grep -o 'extra_qemu_args=\[[^]]*\]')" = "$list" ] \
    || { echo "an edit of another field changed the arguments"; grep -A6 extra_qemu_args "$bundle"; rc=1; }
  # A quote left open is refused at save, and nothing is written.
  cp "$bundle" "$dir/before.toml"
  o="$($LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - '-name "open' 2>&1)" \
    && { echo "a quote left open was saved"; rc=1; }
  case "$o" in *"never closed"*) ;; *) echo "the refusal does not say why"; echo "$o" | tail -3; rc=1;; esac
  cmp -s "$bundle" "$dir/before.toml" || { echo "a refused save changed the bundle"; rc=1; }
  # Our QEMU takes the line and the -global reaches our adapter.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - \
        '-global d3dpt-vga.ddflags=32768' >/dev/null || rc=1
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    args="$($LAUNCHERX --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
           '{"execute":"human-monitor-command","arguments":{"command-line":"info qtree"}}' \
           '{"execute":"quit"}' \
         | timeout 30 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the machine with extra arguments"; echo "$o" | tail -3; rc=1; }
    case "$o" in *"ddflags = 32768"*) ;; *) echo "the -global did not reach d3dpt-vga"; echo "$o" | tail -3; rc=1;; esac
  else
    echo "  (no build/qemu: the QEMU half skipped)"
  fi
  # An empty line clears them.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - '' >/dev/null || rc=1
  grep -q '^extra_qemu_args' "$bundle" && { echo "an empty line left extra_qemu_args in the bundle"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-global"*) echo "an empty line left the arguments on the command line"; echo "$args"; rc=1;; esac
  return $rc
}

pointer_check() { # the wizard's pointer switch, from a checkbox to a real QEMU
  local rc=0 dir="$OUT/pointer" bundle dos args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp pointer "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  dos="$($LAUNCHERX --new dos pointer-dos "$dir/disk.qcow2")" || { echo "--new dos failed"; return 1; }
  # A Windows machine gets the tablet, which is what "no grab" is made
  # of; a DOS machine does not, because its mouse drivers read the PS/2
  # controller and would find no pointer at all.
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-device usb-tablet"*) ;; *) echo "a new XP machine has no tablet"; echo "$args"; rc=1;; esac
  args="$($LAUNCHERX --print-args "$dos")"
  case "$args" in *usb*) echo "a new DOS machine has a tablet it cannot read"; echo "$args"; rc=1;; esac
  # The switch itself, through the real form: the tablet goes, and the
  # controller goes with it rather than staying behind with nothing on it.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - noseamless >/dev/null \
    || { echo "--wizard-edit noseamless failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *usb*) echo "turning the seamless mouse off left USB behind"; echo "$args"; rc=1;; esac
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - seamless >/dev/null \
    || { echo "--wizard-edit seamless failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-device usb-tablet"*) ;; *) echo "turning it back on did not restore the tablet"; echo "$args"; rc=1;; esac
  # And the point of it: our QEMU accepts both machines. Started paused
  # on the real binary and told to quit, so a refused device is an exit
  # code rather than a hung guest.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for b in "$bundle" "$dos"; do
      args="$($LAUNCHERX --print-args "$b")"
      # shellcheck disable=SC2086
      o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
           | timeout 30 $QSYS_PIPE $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused $b"; echo "$o" | tail -3; rc=1; }
    done
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

libsynth_check() { # the music engines through their C API (doc 20 §7)
  local dir="$OUT/libsynth"
  rm -rf "$dir"; mkdir -p "$dir"
  # The bank the packages ship is what the General MIDI cases play
  # through, deliberately: a truncated or unreadable bank in a package is
  # exactly the failure a fixture written for the occasion never sees.
  $SYNTHX selftest "$dir" --sf2 soundfonts/TimGM6mb.sf2 ${MT32_ROMS:+--roms "$MT32_ROMS"}
}

# One MPU-401 or OPL3 port write, as the human monitor spells it.
port_write() { printf 'o /b %s %s\n' "$1" "$2"; }

# The register writes an AdLib driver makes to hold a 440 Hz note: OPL3
# mode on, one channel of two operators, additive, both outputs, key on.
# fnum 580 at block 4 is 440 Hz on a chip clocked at 49716 Hz.
opl_note_script() {
  port_write 0x38a 0x05; port_write 0x38b 0x01
  port_write 0x388 0x01; port_write 0x389 0x20
  local op
  for op in 0 3; do
    port_write 0x388 "$((0x20 + op))"; port_write 0x389 0x01
    port_write 0x388 "$((0x40 + op))"; port_write 0x389 0x00
    port_write 0x388 "$((0x60 + op))"; port_write 0x389 0xf0
    port_write 0x388 "$((0x80 + op))"; port_write 0x389 0x77
  done
  port_write 0x388 0xc0; port_write 0x389 0x31
  port_write 0x388 0xa0; port_write 0x389 0x44
  port_write 0x388 0xb0; port_write 0x389 0x32
}

sb_mixer_check() { # the SB16's mixer volumes reach the FM chip (patch 61)
  local dir="$OUT/sb-mixer" v rc=0
  rm -rf "$dir"; mkdir -p "$dir"
  # One card with its FM chip, as a machine has them; the mixer is written
  # the way a driver writes it (index at base+4, data at base+5) before
  # the note. 0xc8 is 5-bit level 25 = -12 dB; the SB Pro's 0xcc is 4-bit
  # 12 a side, which a CT1745 reads as the same 25.
  for v in unity fm master sbpro; do
    { case $v in
        fm)     port_write 0x224 0x34; port_write 0x225 0xc8
                port_write 0x224 0x35; port_write 0x225 0xc8 ;;
        master) port_write 0x224 0x30; port_write 0x225 0xc8
                port_write 0x224 0x31; port_write 0x225 0xc8 ;;
        sbpro)  port_write 0x224 0x26; port_write 0x225 0xcc ;;
      esac
      opl_note_script; sleep 2; echo quit; } \
      | timeout 60 $QSYS_PIPE -display none -monitor stdio \
          -audiodev "wav,id=w,path=$dir/$v.wav" \
          -device sb16,audiodev=w -device opl3,audiodev=w,sbbase=0x220 >/dev/null 2>&1
  done
  python3 - "$dir" <<'PY' || rc=1
import math, struct, sys, wave
d = sys.argv[1]
def peak(name):
    w = wave.open("%s/%s.wav" % (d, name))
    raw = w.readframes(w.getnframes())
    v = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return max(abs(x) for x in v) if v else 0
ref = peak("unity")
if ref < 1000:
    sys.exit("no FM note at unity (peak %d): nothing to measure against" % ref)
bad = 0
for name in ("fm", "master", "sbpro"):
    p = peak(name)
    db = 20 * math.log10(p / ref) if p else -99
    ok = abs(db + 12) <= 1
    print("  %-6s %+5.1f dB against unity (want -12)%s" % (name, db, "" if ok else "  <- wrong"))
    bad += not ok
sys.exit(1 if bad else 0)
PY
  return $rc
}

# What a driver writes to an MPU-401: reset, UART mode, then a program
# change and a note-on for A4, the note the checks measure.
mpu_note_script() {
  port_write 0x331 0xff
  port_write 0x331 0x3f
  port_write 0x330 0xc0; port_write 0x330 0x00
  port_write 0x330 0x90; port_write 0x330 0x45; port_write 0x330 0x64
}

# The Sound Blaster's interrupt, asked of the card and the PIC and
# nothing else (patch 25). Every count below is a *rising edge* of IRQ 5,
# since `info irq` only counts 0→1, and edges are the point. The card
# holds its line until the DSP status port is read, so an assertion nobody
# can acknowledge holds it for good. Every block after it is a level 1
# into an already-high line, an edge-triggered i8259 sees nothing, and
# the card is deaf until the next reset. Duke Nukem 3D's SETUP.EXE plays
# its "Test Sound FX Card" once and says "Playback failed, possibly due
# to an invalid or conflicting IRQ" every time after.
sb16_irq_check() {
  local rc=0 o n1 n2 n3 n4
  # A block size first: `0x1c` with none set leaves the device with
  # block_size -1, and a DMA that then ran would spin in sb16.c's
  # left_till_irq wrap. The channel is masked at power-up, so nothing
  # transfers here. `0x1c` is only how a guest says "auto-init", which
  # is the state the old reset fabricated an interrupt out of.
  o="$( { port_write 0x22c 0x48; port_write 0x22c 0xff; port_write 0x22c 0x01
          port_write 0x22c 0x1c;               echo "info irq"
          port_write 0x226 0x01; port_write 0x226 0x00
                                               echo "info irq"
          # A one-sample silence block (DSP 0x80): its end is an ordinary
          # 8-bit interrupt and must be acknowledgeable, so the same
          # block a second time has to reach the PIC a second time.
          port_write 0x22c 0x80; port_write 0x22c 0x00; port_write 0x22c 0x00
                                               echo "info irq"
          printf 'i /b 0x22e\n'
          port_write 0x22c 0x80; port_write 0x22c 0x00; port_write 0x22c 0x00
                                               echo "info irq"; echo quit
        } | timeout 60 $QSYS_PIPE -display none -monitor stdio \
              -audiodev none,id=w -device sb16,audiodev=w 2>&1 \
            | tr '\r' '\n' \
            | awk '/^IRQ statistics for/ { isa = ($0 ~ /isa-i8259/)
                                 if (isa) { b++; v[b] = 0 }
                                 next }
                   /^ 5:/ && isa { v[b] = $2 }
                   END { for (i = 1; i <= b; i++) print v[i] }')"
  # Four readings, one per `info irq`, the master PIC's: after the DMA
  # command, after the DSP reset, after one silence block, after the
  # second. An absent line is no interrupt at all, which is 0.
  set -- $(printf '%s\n' "$o")
  n1="${1:-0}"; n2="${2:-0}"; n3="${3:-0}"; n4="${4:-0}"
  if [ "$n1" != 0 ]; then
    echo "the sb16 raised IRQ 5 on an auto-init DMA command alone ($n1)"; rc=1
  fi
  if [ "$n2" != "$n1" ]; then
    echo "a DSP reset raised IRQ 5 (count $n1 -> $n2):"
    echo "  hardware clears the pending interrupt there, it does not make one,"
    echo "  and the guest resetting the DSP has its own IRQ masked — the edge"
    echo "  is latched in the PIC, unowned, and Windows never unmasks again"
    rc=1
  fi
  if [ "$n3" != "$((n2 + 1))" ]; then
    echo "a silence block (DSP 0x80) did not raise IRQ 5 (count $n2 -> $n3)"; rc=1
  fi
  if [ "$n4" != "$((n3 + 1))" ]; then
    echo "the second silence block never reached the PIC (count $n3 -> $n4):"
    echo "  the first one's interrupt sets no status bit, so the driver's read"
    echo "  of the DSP status port cannot lower the line and no edge follows"
    rc=1
  fi
  return $rc
}

music_check() { # the two pickers, and then the devices actually sounding
  local rc=0 dir="$OUT/music" bundle args f want o irr
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  # What each family starts on. Every family keeps the card it already
  # had (98 and DOS the Sound Blaster, XP the AC'97, Other the Ensoniq),
  # so opening an existing machine changes no hardware. What is added is
  # the MIDI port on the two families that have no synthesizer of their
  # own, and the OPL3 that comes with the cards that carried one.
  for f in win98:sb16 dos:sb16 xp:AC97 other:ES1370; do
    want="${f#*:}"; f="${f%%:*}"
    bundle="$($LAUNCHERX --new "$f" "music-$f" "$dir/disk.qcow2")" || { echo "--new $f failed"; return 1; }
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *"$want,audiodev=embed0"*) ;; *) echo "a new $f machine has no $want"; echo "$args"; rc=1;; esac
  done
  # The FM chip follows the card, the way buying one did: an SB16 carries
  # an OPL3 (and mirrors it at the card's own base, where an SB-aware
  # driver looks), an AC'97 and an Ensoniq carry none.
  for f in win98 dos; do
    args="$($LAUNCHERX --print-args "$dir/library/music-$f/machine.toml")"
    case "$args" in *"opl3,audiodev=embed0,sbbase=0x220"*) ;; *) echo "$f: the SB16 came without its OPL3"; echo "$args"; rc=1;; esac
    case "$args" in *"mpu401,audiodev=embed0,synth=gm"*) ;; *) echo "$f: no General MIDI port on a family that has no synthesizer of its own"; echo "$args"; rc=1;; esac
  done
  for f in xp other; do
    args="$($LAUNCHERX --print-args "$dir/library/music-$f/machine.toml")"
    case "$args" in *"-device opl3,"*) echo "$f: an FM chip arrived with a card that never had one"; echo "$args"; rc=1;; esac
    case "$args" in *mpu401*) echo "$f: a MIDI port arrived on a family whose default is none"; echo "$args"; rc=1;; esac
  done
  # The switch itself, on the 98 machine: to the AC'97 (which takes the
  # SB16 *and* its FM away, and lands in the pinned PCI slot), to the
  # Gravis, and back.
  bundle="$dir/library/music-win98/machine.toml"
  $LAUNCHERX --music "$bundle" ac97 >/dev/null || { echo "--music ac97 failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"AC97,audiodev=embed0,addr=0x04"*) ;; *) echo "98: the AC'97 did not arrive at its pinned slot"; echo "$args"; rc=1;; esac
  # The device arguments, not the bare names: `-L` names the checkout, and
  # a worktree called `sb16-dsound` put `sb16` in every line.
  case "$args" in *"-device sb16,"*|*"-device opl3,"*) echo "98: the SB16 or its FM is still there beside the AC'97"; echo "$args"; rc=1;; esac
  $LAUNCHERX --music "$bundle" gus none >/dev/null || { echo "--music gus failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"gus,audiodev=embed0"*) ;; *) echo "98: no Gravis"; echo "$args"; rc=1;; esac
  case "$args" in *mpu401*) echo "98: the MIDI port survived being turned off"; echo "$args"; rc=1;; esac
  # A card this family does not offer is refused rather than written: an
  # ES1370 on Windows is a card 98 has no driver for, and a stray field
  # should not be able to produce one.
  $LAUNCHERX --music "$bundle" es1370 >/dev/null || { echo "--music es1370 failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *ES1370*) echo "98: was given the Ensoniq, which is not on offer there"; echo "$args"; rc=1;; esac
  $LAUNCHERX --music "$bundle" sb16 gm >/dev/null || { echo "--music sb16 gm failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"sb16,audiodev=embed0"*) ;; *) echo "98: the SB16 did not come back"; echo "$args"; rc=1;; esac
  # The MT-32 has no default and no fallback: nothing of Roland's ships,
  # so a machine asked for one without ROMs must be refused at the form
  # rather than at the guest's first note.
  if $LAUNCHERX --music "$bundle" - mt32 >/dev/null 2>&1; then
    echo "98: an MT-32 machine with no ROM directory was saved"; rc=1
  fi
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *mt32*) echo "98: the refused MT-32 was written anyway"; echo "$args"; rc=1;; esac
  $LAUNCHERX --music "$bundle" - mt32 - "$dir/roms" >/dev/null || { echo "--music mt32 with a directory failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"synth=mt32,romdir=$dir/roms"*) ;; *) echo "98: the MT-32's ROM directory did not reach the device"; echo "$args"; rc=1;; esac
  $LAUNCHERX --music "$bundle" - gm >/dev/null || { echo "--music gm failed"; rc=1; }

  if [ ! -x $QSYS ] || [ ! -x $QIMG ]; then
    echo "  (no build/qemu: the command lines were checked but not run)"
    return $rc
  fi
  $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
  # Every card on every family, on the real binary: started paused and
  # told to quit, so a machine QEMU will not build is an exit code. The
  # bank is named the way the player names it (companions.rs), because a
  # machine that says synth=gm and nothing else is the normal case.
  export LIBSYNTH_SF2="$PWD/soundfonts/TimGM6mb.sf2"
  for f in win98:sb16 win98:ac97 win98:gus win98:none dos:sb16 dos:gus dos:adlib xp:ac97 xp:sb16 other:es1370 other:ac97; do
    want="${f#*:}"; f="${f%%:*}"
    bundle="$dir/library/music-$f/machine.toml"
    $LAUNCHERX --music "$bundle" "$want" >/dev/null || { echo "$f: --music $want failed"; rc=1; continue; }
    args="$($LAUNCHERX --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the $f machine with the $want card"; echo "$o" | tail -3; rc=1; }
  done
  # And the half no command line can show: the devices *sounding*. The
  # monitor writes the same ports a guest would, QEMU's own wav backend
  # records what its mixer produced, and the note has to be in the file.
  # A device that accepts every write and plays nothing passes everything
  # above and fails here.
  rm -f "$dir/opl.wav" "$dir/midi.wav"
  { opl_note_script; sleep 2; echo quit; } \
    | timeout 60 $QSYS_PIPE -display none -monitor stdio \
        -audiodev "wav,id=w,path=$dir/opl.wav" -device opl3,audiodev=w >/dev/null 2>&1
  $SYNTHX wavtone "$dir/opl.wav" 440 || rc=1
  { mpu_note_script; sleep 2; echo quit; } \
    | timeout 60 $QSYS_PIPE -display none -monitor stdio \
        -audiodev "wav,id=w,path=$dir/midi.wav" \
        -device "mpu401,audiodev=w,synth=gm,soundfont=$PWD/soundfonts/TimGM6mb.sf2" >/dev/null 2>&1
  $SYNTHX wavtone "$dir/midi.wav" 440 || rc=1
  # And the interrupt the MIDI port must *not* raise (doc 20 §5.1). A
  # real MPU-401's line is IRQ 2/9. QEMU's PIIX4 puts the ACPI SCI on
  # IRQ 9, and an ACPI Windows 98 owns it, so the ACK a driver's reset
  # queues is an interrupt no handler can acknowledge. The line stays
  # high, the handler is re-entered on every IRET, and the guest
  # triple-faults. Duke Nukem 3D's SETUP rebooted a machine doing exactly
  # this. The reset is written the way a driver writes it and the PIC is
  # asked what is pending. In a guest the symptom is a spontaneous reboot,
  # which no headless run could tell from a hang, so the hardware is
  # asked instead.
  o="$(printf 'o /b 0x331 0xff\ninfo pic\nquit\n' \
       | timeout 30 $QSYS_PIPE -display none -monitor stdio \
           -audiodev none,id=w \
           -device "mpu401,audiodev=w,synth=gm,soundfont=$PWD/soundfonts/TimGM6mb.sf2" 2>&1)"
  irr="$(printf '%s\n' "$o" | sed -n 's/.*pic1: irr=\([0-9a-f]*\).*/\1/p' | tail -1)"
  if [ -z "$irr" ]; then
    echo "could not read the slave PIC back after an MPU-401 reset"; rc=1
  elif [ $(( 0x$irr & 2 )) -ne 0 ]; then
    echo "the MPU-401 left IRQ 9 asserted after a reset (pic1 irr=$irr):"
    echo "  an ACPI Win98 guest triple-faults on it — doc 20 §5.1"
    rc=1
  fi
  return $rc
}

family_other_check() { # the "Other" family's hardware, from the picker to a real QEMU
  local rc=0 dir="$OUT/family-other" bundle args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new other beos "$dir/disk.qcow2")" || { echo "--new other failed"; return 1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  # Standard hardware, and specifically *not* ours: `d3dpt-vga` needs the
  # display driver from the guest-tools ISO, which is a Windows driver, so
  # a BeOS or Linux guest on it would come up with no display at all.
  case "$args" in *"-vga std"*) ;; *) echo "an Other machine is not on the standard VGA"; echo "$args"; rc=1;; esac
  case "$args" in *d3dpt-vga*) echo "an Other machine got our own adapter, which has no driver for it"; echo "$args"; rc=1;; esac
  # No card at all until someone asks for one, the default for every
  # family (`bundle::default_network`). These guests stopped getting
  # security fixes twenty years ago, so a machine nobody has been asked
  # about is off the network.
  case "$args" in *rtl8139*|*-netdev*) echo "a new machine came with a network card"; echo "$args"; rc=1;; esac
  case "$args" in *"-nic none"*) ;; *) echo "networking off did not emit -nic none, so QEMU supplies a card of its own"; echo "$args"; rc=1;; esac
  case "$args" in *"ES1370,audiodev=embed0,addr=0x04"*) ;; *) echo "no ES1370 at 0x04"; echo "$args"; rc=1;; esac
  # And the card the checkbox turns on is doc 06's, in its own slot.
  $LAUNCHERX --wizard-edit "$bundle" - - - net >/dev/null || { echo "--wizard-edit net failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"rtl8139,netdev=n0,addr=0x03"*) ;; *) echo "no RTL8139 at 0x03"; echo "$args"; rc=1;; esac
  # The tablet is off here by default (`default_seamless_mouse`): an
  # absolute pointer needs the guest to agree it is absolute, and these
  # guests get no guest-tools install to make them.
  case "$args" in *usb*) echo "a new Other machine has a tablet it may not be able to read"; echo "$args"; rc=1;; esac
  # And the reason the addresses are written out: removing the NIC must
  # not slide the sound card up into its slot, which an installed guest
  # would see as its card having been swapped.
  $LAUNCHERX --wizard-edit "$bundle" - - - nonet >/dev/null || { echo "--wizard-edit nonet failed"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *rtl8139*) echo "turning networking off left the NIC behind"; echo "$args"; rc=1;; esac
  case "$args" in *"ES1370,audiodev=embed0,addr=0x04"*) ;; *) echo "the sound card moved when the NIC went"; echo "$args"; rc=1;; esac
  # A bundle with no `network` field at all has no card either (it once
  # meant on). Written by hand, as the wizard always writes the field.
  grep -v '^network' "$bundle" >"$dir/nofield.toml"
  args="$($LAUNCHERX --print-args "$dir/nofield.toml")"
  case "$args" in *rtl8139*|*-netdev*) echo "a bundle with no network field came with a card"; echo "$args"; rc=1;; esac
  case "$args" in *"-nic none"*) ;; *) echo "a bundle with no network field did not emit -nic none"; echo "$args"; rc=1;; esac
  $LAUNCHERX --wizard-edit "$bundle" - - - net >/dev/null || { echo "--wizard-edit net failed"; rc=1; }
  # Started paused on the real binary and told to quit, so a device our
  # QEMU does not have is an exit code rather than a hung guest.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    args="$($LAUNCHERX --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the Other machine"; echo "$o" | tail -3; rc=1; }
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

hpet_check() { # no HPET on Win98 and a versioned board, from the bundle to our QEMU's device tree
  local rc=0 dir="$OUT/hpet" w98 xp args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  w98="$($LAUNCHERX --new win98 hpet-98 "$dir/disk.qcow2")" || { echo "--new win98 failed"; return 1; }
  xp="$($LAUNCHERX --new xp hpet-xp "$dir/disk.qcow2")" || { echo "--new xp failed"; return 1; }
  # Windows 98 has no driver for an HPET and never uses one: with it the
  # guest's Device Manager shows an Unknown Device (ACPI\*PNP0103) with a
  # yellow mark on every machine. XP is left alone.
  # Both on a versioned board (`Machine::board`), so a newer QEMU's `pc`
  # cannot change a machine under its snapshots.
  args="$($LAUNCHERX --print-args "$w98")"
  case "$args" in *"-machine pc-i440fx-"*",hpet=off "*) ;; *) echo "a Win98 machine still has an HPET, or no versioned board"; echo "$args"; rc=1;; esac
  args="$($LAUNCHERX --print-args "$xp")"
  case "$args" in *"-machine pc-i440fx-"[0-9]*" "*) ;; *) echo "an XP machine's board changed"; echo "$args"; rc=1;; esac
  # A bundle from before the field boots the board it was made on, 9.2's.
  mkdir -p "$dir/library/legacy"
  grep -v '^board = ' "$w98" >"$dir/library/legacy/machine.toml"
  args="$($LAUNCHERX --print-args "$dir/library/legacy/machine.toml")"
  case "$args" in *"-machine pc-i440fx-9.2,hpet=off "*) ;; *) echo "a bundle with no board did not get 9.2's"; echo "$args"; rc=1;; esac
  # And the device itself, asked of the real binary: the property's name
  # is QEMU's to change, and a misspelt one would be an exit code, but a
  # property that stopped removing the device would be neither.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for b in "$w98" "$xp"; do
      args="$($LAUNCHERX --print-args "$b")"
      # shellcheck disable=SC2086
      o="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
             '{"execute":"human-monitor-command","arguments":{"command-line":"info qtree"}}' \
             '{"execute":"quit"}' \
           | timeout 30 $QSYS_PIPE $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused $b"; echo "$o" | tail -3; rc=1; continue; }
      case "$b:$o" in
        "$w98":*'dev: hpet'*) echo "our QEMU built a Win98 machine with an HPET"; rc=1;;
        "$xp":*'dev: hpet'*) ;;
        "$xp":*) echo "no HPET in the XP machine, so the Win98 answer proves nothing"; rc=1;;
      esac
    done
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  return $rc
}

# The command line an adapter name lands as (`bundle::Video::args`), so
# the checks below can name the pick rather than repeat its arguments.
vga_args() {
  case "$1" in
    d3dpt) echo "-device d3dpt-vga,addr=0x02";;
    *)     echo "-vga $1";;
  esac
}

display_adapter_check() { # the wizard's adapter picker, from a combo box to a real QEMU
  local rc=0 dir="$OUT/display-adapter" bundle args o f want other first
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  # What each family starts on. XP and Win98 on our own adapter, because
  # the whole display path is built on it (docs 15 and 19). Other on the
  # standard VGA, the one every guest can fall back on. DOS on the
  # standard VGA too, the fuller of the two VESA BIOSes (user decision).
  for f in win98:"-device d3dpt-vga,addr=0x02" xp:"-device d3dpt-vga,addr=0x02" other:"-vga std" dos:"-vga std"; do
    want="${f#*:}"; f="${f%%:*}"
    bundle="$($LAUNCHERX --new "$f" "adapter-$f" "$dir/disk.qcow2")" || { echo "--new $f failed"; return 1; }
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *"$want"*) ;; *) echo "a new $f machine is not on $want"; echo "$args"; rc=1;; esac
  done
  # The switch itself, on a Windows machine, away from the adapter the
  # family starts on (ours, on both) and back again. The one it left must
  # be *gone*, since a machine with both would show the guest two
  # displays. The cards pinned below it must not move, because a card
  # that moves is a hardware change an installed guest re-detects.
  for f in win98:d3dpt:cirrus xp:d3dpt:cirrus; do
    other="${f##*:}"; f="${f%:*}"; first="${f#*:}"; f="${f%%:*}"
    bundle="$dir/library/adapter-$f/machine.toml"
    # A new machine has no NIC (`bundle::default_network`), and the
    # question below is whether the cards *under* the adapter move when
    # it changes, so this one is given the card first.
    $LAUNCHERX --wizard-edit "$bundle" - - - net >/dev/null \
      || { echo "$f: --wizard-edit net failed"; rc=1; continue; }
    $LAUNCHERX --wizard-edit "$bundle" - - - - - - - "$other" >/dev/null \
      || { echo "$f: --wizard-edit $other failed"; rc=1; continue; }
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *"$(vga_args "$other")"*) ;; *) echo "$f: the $other adapter did not arrive"; echo "$args"; rc=1;; esac
    case "$args" in *"$(vga_args "$first")"*) echo "$f: the $first adapter is still there beside the $other"; echo "$args"; rc=1;; esac
    case "$args" in *"netdev=n0,addr=0x03"*) ;; *) echo "$f: the NIC moved when the adapter changed"; echo "$args"; rc=1;; esac
    # The standard VGA is not on offer to Windows (XP has no driver for
    # it at all), so asking for it must leave the machine as it was rather
    # than produce a guest with no display.
    $LAUNCHERX --wizard-edit "$bundle" - - - - - - - std >/dev/null \
      || { echo "$f: --wizard-edit std failed"; rc=1; continue; }
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *"-vga std"*) echo "$f: was given the standard VGA, which has no driver there"; echo "$args"; rc=1;; esac
    $LAUNCHERX --wizard-edit "$bundle" - - - - - - - "$first" >/dev/null \
      || { echo "$f: --wizard-edit $first failed"; rc=1; continue; }
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *"$(vga_args "$first")"*) ;; *) echo "$f: the $first adapter did not come back"; echo "$args"; rc=1;; esac
  done
  # DOS has the picker too, and it is the one family where the question
  # is not "which driver". Its titles program the adapter themselves, so
  # what changes is which VESA BIOS the game finds. The same three demands
  # as above apply (the new one arrives, the old one is *gone*, and it
  # comes back), plus one specific to DOS. Our own adapter is refused,
  # because there is no DOS driver for it anywhere and a DOS machine on it
  # would have the plain VGA and nothing else.
  bundle="$dir/library/adapter-dos/machine.toml"
  if $LAUNCHERX --wizard-edit "$bundle" - - - - - - - cirrus >/dev/null; then
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *"-vga cirrus"*) ;; *) echo "dos: the Cirrus did not arrive"; echo "$args"; rc=1;; esac
    case "$args" in *"-vga std"*) echo "dos: the standard VGA is still there beside the Cirrus"; echo "$args"; rc=1;; esac
  else
    echo "dos: --wizard-edit cirrus failed"; rc=1
  fi
  if $LAUNCHERX --wizard-edit "$bundle" - - - - - - - d3dpt >/dev/null; then
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *d3dpt-vga*) echo "dos: was given our own adapter, which has no DOS driver"; echo "$args"; rc=1;; esac
  else
    echo "dos: --wizard-edit d3dpt failed"; rc=1
  fi
  if $LAUNCHERX --wizard-edit "$bundle" - - - - - - - std >/dev/null; then
    args="$($LAUNCHERX --print-args "$bundle")"
    case "$args" in *"-vga std"*) ;; *) echo "dos: the standard VGA did not come back"; echo "$args"; rc=1;; esac
  else
    echo "dos: --wizard-edit std failed"; rc=1
  fi
  # Every adapter on every family, on the real binary: started paused and
  # told to quit, so a machine QEMU will not build is an exit code.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    for f in win98:d3dpt win98:cirrus xp:d3dpt xp:cirrus other:std other:cirrus dos:std dos:cirrus; do
      want="${f#*:}"; f="${f%%:*}"
      bundle="$dir/library/adapter-$f/machine.toml"
      $LAUNCHERX --wizard-edit "$bundle" - - - - - - - "$want" >/dev/null || { rc=1; continue; }
      args="$($LAUNCHERX --print-args "$bundle")"
      # shellcheck disable=SC2086
      o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
           | timeout 30 $QSYS_PIPE $args \
               -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
        || { echo "our QEMU refused $f on $want"; echo "$o" | tail -3; rc=1; }
    done
  else
    echo "  (no build/qemu: the command lines were checked but not run)"
  fi
  return $rc
}

d3d9_backend_check() { # the machine form's Direct3D picker, from a combo box to a real QEMU
  local rc=0 dir="$OUT/d3d9" bundle args host o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp d3d9 "$dir/disk.qcow2")" || { echo "--new xp failed"; return 1; }
  # A machine nobody has touched is on `auto`, which says nothing at all,
  # *unless* this host is a Windows one below DXVK's Vulkan 1.3 floor.
  # There auto is resolved here rather than in the executor, because only
  # this side has a Vulkan probe that can tell a software device from a
  # real one. So the absence is required, and the one presence allowed is
  # required to agree with `--host-check`.
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in
    *d3d9=dxvk*) echo "a new machine names dxvk, which is what saying nothing already means"; echo "$args"; rc=1;;
    *d3d9=system*)
      host="$($LAUNCHERX --host-check || true)"
      case "$host" in
        *"own Direct3D 9"*) ;;
        *) echo "auto resolved to the system Direct3D 9 on a host whose probe says otherwise"; echo "$host"; rc=1;;
      esac;;
  esac
  # The two explicit answers, which mean the same thing on every host: the
  # property rides on the adapter that carries the executor.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - - dxvk >/dev/null \
    || { echo "--wizard-edit dxvk failed"; return 1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-device d3dpt-vga,addr=0x02,d3d9=dxvk"*) ;; *) echo "dxvk did not reach the adapter"; echo "$args"; rc=1;; esac
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - - system >/dev/null \
    || { echo "--wizard-edit system failed"; return 1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *"-device d3dpt-vga,addr=0x02,d3d9=system"*) ;; *) echo "system did not reach the adapter"; echo "$args"; rc=1;; esac
  # ... and only on that adapter: the Cirrus has no executor behind it, so
  # a machine moved onto it must not carry the property to a device that
  # has never heard of it.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - cirrus >/dev/null \
    || { echo "--wizard-edit cirrus failed"; return 1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *d3d9=*) echo "the Cirrus machine carries a d3d9 property"; echo "$args"; rc=1;; esac
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - d3dpt >/dev/null || rc=1
  # Back to automatic, which writes nothing again.
  $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - - auto >/dev/null \
    || { echo "--wizard-edit auto failed"; return 1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *d3d9=dxvk*) echo "auto still names dxvk"; echo "$args"; rc=1;; esac
  # And the line a real QEMU is given: our own binary has to accept the
  # property and show it on the device, the same way `extra-args` proves
  # a typed `-global` reaches it.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    $LAUNCHERX --wizard-edit "$bundle" - - - - - - - - - - - system >/dev/null || rc=1
    args="$($LAUNCHERX --print-args "$bundle")"
    # shellcheck disable=SC2086
    o="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
           '{"execute":"human-monitor-command","arguments":{"command-line":"info qtree"}}' \
           '{"execute":"quit"}' \
         | timeout 30 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused d3d9=system"; echo "$o" | tail -3; rc=1; }
    # the qtree text comes back JSON-escaped: the quotes around the value are \"
    case "$o" in *'d3d9 = \"system\"'*|*'d3d9 = "system"'*) ;; *) echo "the property did not reach d3dpt-vga"; echo "$o" | tail -3; rc=1;; esac
  else
    echo "  (no build/qemu: the command lines were checked but not run)"
  fi
  return $rc
}

machine_map_check() { # the pass-through regions a PC machine carries
  # qemu-3dfx's overlay wires two pass-through devices into the machine;
  # patch 74 leaves the Glide one out of the build (ADR-020: Glide is the
  # Voodoo 2's). A tree that lost that patch grows a `glidept` region back
  # and nothing else notices, since no guest of ours asks for it. `mesapt`
  # must still be there, or the same tree lost the overlay. The SysBus
  # `d3dpt` left with the guest DLLs (M16 step 7); a region of it back
  # means patch 40 went stale.
  local out
  out="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
        '{"execute":"human-monitor-command","arguments":{"command-line":"info mtree"}}' \
        '{"execute":"quit"}' \
        | timeout 30 $QSYS_PIPE -L qemu/pc-bios -machine pc -m 64 \
            -display none -S -qmp stdio -net none 2>&1)" \
    || { echo "QEMU refused to start"; echo "$out" | tail -3; return 1; }
  local rc=0 r
  case "$out" in *"): mesapt"*) echo "mesapt region present" ;; *) echo "no mesapt region on the machine"; rc=1 ;; esac
  case "$out" in *"): d3dpt"*) echo "a SysBus d3dpt region is on the machine (patch 40 stale?)"; rc=1 ;; *) echo "no SysBus d3dpt region (M16 step 7)" ;; esac
  case "$out" in *glidept*|*glidelfb*|*glideshm*) echo "a Glide pass-through region is on the machine (patch 74 lost?)"; rc=1 ;; *) echo "no Glide pass-through region (patch 74)" ;; esac
  return $rc
}

bios_date_check() { # the legacy BIOS date, as a guest reads it out of a real QEMU
  # Windows 98 installs ACPI (and so enumerates the PCI bus at all) only
  # when the date at F000:FFF5 is at least the ACPICheckDate its own
  # machine.inf carries, 12/01/99 (doc 06). An older BIOS has to be one of
  # the four machines in BIOSINFO.INF's [GoodACPIBios], and we are not.
  # SeaBIOS ships 06/23/99, so prepare-qemu.sh stamps every firmware image
  # it finds. A tree that lost the stamp still boots every existing guest,
  # and only shows up weeks later as a *new* Win98 install in PnP-BIOS
  # mode: "Plug and Play BIOS" with a yellow ! and no USB tablet, AC'97 or
  # NIC ever detected. Hence a check on the firmware itself.
  local rc=0 out date key f cur
  out="$(printf '%s\n' '{"execute":"qmp_capabilities"}' \
        '{"execute":"human-monitor-command","arguments":{"command-line":"xp /8c 0xffff5"}}' \
        '{"execute":"quit"}' \
        | timeout 30 $QSYS_PIPE -L qemu/pc-bios -machine pc -m 64 \
            -display none -S -qmp stdio -net none 2>&1)" \
    || { echo "QEMU refused to start"; echo "$out" | tail -3; return 1; }
  # the monitor prints the row as quoted characters; the date is the first
  # eight of them, the rest of the row is the model byte and padding.
  date="$(printf '%s' "$out" | sed -n 's/.*ffff5: //p' | tr -d " '" | cut -c1-8)"
  case "$date" in
    [0-9][0-9]/[0-9][0-9]/[0-9][0-9]) ;;
    *) echo "no BIOS date at F000:FFF5 (read \"$date\")"; echo "$out" | tail -3; return 1 ;;
  esac
  echo "BIOS date $date (Win98 wants >= 12/01/99 to install ACPI)"
  # Setup compares a two-digit year, so the key is yymmdd and a 2000s date
  # would sort *below* the cutoff here exactly as it would there.
  key=$(( 10#${date##*/} * 10000 + 10#${date%%/*} * 100 + 10#$(x=${date#*/}; echo "${x%%/*}") ))
  if [ "$key" -lt 991201 ]; then
    echo "the guest reads $date, older than Win98's ACPICheckDate 12/01/99"
    echo "(run scripts/prepare-qemu.sh, or -f if the tree was edited by hand)"
    rc=1
  fi
  # Every image carries the same day: the pc machine maps bios-256k.bin,
  # but a package ships the lot and microvm/bios.bin are one -machine away.
  for f in qemu/pc-bios/bios.bin qemu/pc-bios/bios-256k.bin qemu/pc-bios/bios-microvm.bin; do
    [ -f "$f" ] || continue
    cur="$(dd if="$f" bs=1 skip=$(( $(wc -c < "$f") - 11 )) count=8 2>/dev/null)"
    [ "$cur" = "$date" ] || { echo "$(basename "$f") says $cur, the running firmware says $date"; rc=1; }
  done
  return $rc
}

optimizations_check() { # the wizard's fast-path switches, all the way to a real QEMU
  local rc=0 dir="$OUT/opt-switches" bundle args o
  rm -rf "$dir"; mkdir -p "$dir/library"
  export LAUNCHER_LIBRARY_DIR="$dir/library" LAUNCHER_DISC_LIBRARY="$dir/discs.toml"
  export LAUNCHER_SHADER_PROFILES_DIR="$dir/profiles"
  : >"$dir/disk.qcow2"
  bundle="$($LAUNCHERX --new xp opts "$dir/disk.qcow2")" || { echo "--new failed"; return 1; }
  # A machine nobody has touched must produce the command line it always
  # produced: no properties, and no `[optimizations]` table in the file.
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *-cpu\ pentium3\ *) ;; *) echo "a default machine names a CPU property"; echo "$args"; rc=1;; esac
  case "$args" in *=on*|*=off*) echo "a default machine names an optimization"; echo "$args"; rc=1;; esac
  grep -q '^\[optimizations\]' "$bundle" && { echo "a default machine wrote an [optimizations] table"; rc=1; }
  # Every switch, through the real form: off where it ships on, on where
  # it ships off, and each on the option QEMU looks it up on (a CPU
  # property on `-cpu`, an accelerator property on `-accel tcg`).
  $LAUNCHERX --optimizations "$bundle" \
    x87-fast off sse-fast off simd-fast off rep-fast off \
    smc-same-value off soft-imm off inline-lookup off \
    tb-invalidate-fast off tlb-floor off tls-hot-paths off jump-cache-keep off \
    eob-chain off tlb-retire off x87-pc64-as-53 on >"$OUT/optimizations-set.log" 2>&1 \
    || { echo "--optimizations failed"; cat "$OUT/optimizations-set.log"; rc=1; }
  # Patch 21's pinned-regs left the form (it crashed guests for too
  # little gain). A bundle written while it was offered and still saying
  # it on must not put it on the line, and "All defaults" below must take
  # the entry away with the rest.
  awk '{ print } /^\[optimizations\]/ { print "pinned-regs = true" }' "$bundle" >"$bundle.tmp" \
    && mv "$bundle.tmp" "$bundle"
  $LAUNCHERX --optimizations "$bundle" pinned-regs on >/dev/null 2>&1 \
    && { echo "--optimizations still takes pinned-regs"; rc=1; }
  args="$($LAUNCHERX --print-args "$bundle")"
  case "$args" in *pinned-regs*) echo "a retired pinned-regs entry reached the command line"; echo "$args"; rc=1;; esac
  for p in x87-fast=off sse-fast=off simd-fast=off rep-fast=off x87-pc64-as-53=on; do
    case "$args" in *"-cpu pentium3,"*"$p"*) ;; *) echo "$p is not on -cpu"; echo "$args"; rc=1;; esac
  done
  # Every fast path has a switch, so "turn everything off" is a real
  # control run. A guest that is still wrong with these off has cleared
  # our tree.
  for p in smc-same-value=off soft-imm=off inline-lookup=off \
           tb-invalidate-fast=off tlb-floor=off tls-hot-paths=off jump-cache-keep=off \
           eob-chain=off tlb-retire=off; do
    case "$args" in *"-accel tcg,"*"$p"*) ;; *) echo "$p is not on -accel tcg"; echo "$args"; rc=1;; esac
  done
  # The point of the whole thing: our QEMU accepts the line the launcher
  # writes. Started paused on the real binary and told to quit, so a
  # rejected property is an exit code and not a hung guest.
  if [ -x $QSYS ] && [ -x $QIMG ]; then
    # A real image, because a zero-byte file is not a disk; and
    # `audiodev=embed0` is the player's own backend, which lives inside
    # the embed library and not out here, so a null one takes the name
    # (the same stand-in `tools/dos-guest-test.py` uses) and the
    # machine's device line is run verbatim.
    $QIMG create -f qcow2 "$dir/disk.qcow2" 64M >/dev/null || rc=1
    # shellcheck disable=SC2086
    o="$(printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' \
         | timeout 30 $QSYS_PIPE $args \
             -audiodev none,id=embed0 -display none -S -qmp stdio -serial none 2>&1)" \
      || { echo "our QEMU refused the launcher's command line"; echo "$o" | tail -3; rc=1; }
  else
    echo "  (no build/qemu: the command line was checked but not run)"
  fi
  # "All defaults" empties the table again rather than writing every
  # switch out at its shipped value.
  $LAUNCHERX --optimizations "$bundle" defaults >/dev/null 2>&1 || rc=1
  grep -q '^\[optimizations\]' "$bundle" \
    && { echo "\"All defaults\" left an [optimizations] table behind"; rc=1; }
  return $rc
}
no_optionals_check() { # the artefacts link only what we chose
  # QEMU auto-detects many optional features, so what a build links is
  # otherwise decided by which libraries the machine happened to have.
  # That is how this box, the Mac and the Flatpak SDK end up with three
  # different libqemu-embed. scripts/configure-qemu.sh disables the lot and
  # this asks the built artefacts whether it stuck, because a dropped flag
  # re-links silently and every packager starts carrying the library again.
  # Four families, all dead for us:
  #
  #   display  the player is the front end. It embeds QEMU, the embed
  #            library appends `-display none` itself and registers its own
  #            3D provider (patch 30). SDL, GTK/VTE, Cocoa, curses, spice.
  #   audio    the player's sound is patch 20's `embed` audiodev; the
  #            headless tools use `none`, xp-cdimage-test.sh uses `wav`.
  #   network  a bundle with networking on says `-netdev user` and nothing
  #            else, so slirp stays and AF_XDP/vde go.
  #   block    every drive is a local file: qcow2, a raw floppy, or a
  #            disc image through our own `cdimage` driver (doc 17). curl,
  #            libssh, iscsi, nfs, rbd, gluster, blkio.
  #
  #   ...plus brlapi, a braille chardev nothing here has ever opened.
  #
  # It looks for a *loaded* name as well as a linked one. sdl2-compat once
  # reached for SDL3 through LoadLibrary on a user's PC, where no
  # import-table walk could have seen it.
  local rc=0 f
  local names="build/qemu/libqemu-embed-i386.$SO $QSYS $QIMG"
  names="$names build/dxvk/src/d3d9/libdxvk_d3d9.$SO$([ "$SO" = so ] && echo .0)"
  names="$names build/win/qemu/libqemu-embed-i386.dll build/win/qemu/qemu-system-i386.exe"
  # displays (Cocoa is a framework, matched by name in the same list)
  local linked='libSDL|libgtk-|libgdk-|libvte|libspice-server|libncurses|Cocoa\.framework'
  # host audio
  linked="$linked"'|libasound|libpulse|libjack|libpipewire|libsndio'
  # network backends and network-storage block drivers
  linked="$linked"'|libxdp|libbpf|libvdeplug|libcurl|libssh|libiscsi|libnfs|librbd|librados|libglusterfs|libblkio'
  # braille
  linked="$linked"'|libbrlapi'
  # PNG screendumps and VNC's JPEG: every tool takes the PPM (tools/qmpc.py)
  linked="$linked"'|libpng|libjpeg'
  # loaded by name at run time: the SDL DLLs, which is the case that bit
  local loaded='SDL[23][-.0-9]*\.(so|dll|dylib)'
  # compiled in: QAPI generates one AUDIODEV_DRIVER_<X> enumerator per
  # audio backend, each behind its own `if: CONFIG_AUDIO_<X>` (qapi/
  # audio.json), so the name is in the binary exactly when the backend is
  # built. That catches the three with no shared object of their own (OSS,
  # CoreAudio, DirectSound). It is how patch 23 was found: on Windows
  # `--disable-dsound` was a no-op and dsoundaudio.c went in anyway.
  # NONE, WAV and our EMBED are the three that must be there.
  local builtin_audio='AUDIODEV_DRIVER_(ALSA|PA|PIPEWIRE|JACK|OSS|SNDIO|COREAUDIO|DSOUND|SDL|SPICE)$'
  for f in $names; do
    [ -f "$f" ] || continue
    local frc=0
    case "$f" in
      *.dll|*.exe) ;;   # no ldd/otool for PE; the strings pass covers it
      *)
        if [ "$OS" = Darwin ]; then
          otool -L "$f" 2>/dev/null | grep -qE "$linked" \
            && { echo "$f links a library we disabled"; otool -L "$f" | grep -E "$linked" | sed 's/^/    /'; rc=1; frc=1; }
          # On a Mac QEMU's libraries are ours and static (scripts/
          # build-deps.sh, docs/build-macos.md "The libraries"), so a
          # Homebrew path in a load command is a library the app would
          # have to carry, and the configure that found it went wrong.
          otool -L "$f" 2>/dev/null | grep -qE '^\s*(/opt/homebrew|/usr/local)/' \
            && { echo "$f links a Homebrew library"; otool -L "$f" | grep -E '^\s*(/opt/homebrew|/usr/local)/' | sed 's/^/    /'; rc=1; frc=1; }
        else
          ldd "$f" 2>/dev/null | grep -qE "$linked" \
            && { echo "$f links a library we disabled"; ldd "$f" | grep -E "$linked" | sed 's/^/    /'; rc=1; frc=1; }
        fi;;
    esac
    if command -v strings >/dev/null; then
      strings -a "$f" | grep -qiE "$loaded" \
        && { echo "$f names an SDL library to load at run time"; rc=1; frc=1; }
      local built
      built=$(strings -a "$f" | grep -oE "$builtin_audio" | sort -u | tr '\n' ' ')
      [ -n "$built" ] && { echo "$f has host audio backends compiled in: $built"; rc=1; frc=1; }
    fi
    [ "$frc" = 0 ] && echo "  clean: $f"
  done
  return $rc
}
have_display() { [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] || [ "$OS" = Darwin ]; }
preview_anim_check() { # the shader preview keeps drawing (doc 07)
  # Plenty of presets do not stand still: an interlaced CRT draws
  # alternate fields, a phosphor afterglow decays, an NTSC signal
  # shimmers. The editor's preview renders on demand, so unless it knows
  # to keep asking it shows one frozen frame of all that. Every front end
  # takes the answer from `launcher_core::preview`, so it is asked here
  # through the shared verb.
  local moving=third_party/slang-shaders/crt/crt-beans-vga.slangp
  local still=third_party/slang-shaders/crt/crt-lottes.slangp
  local rc=0
  # A preset that stands still: said to stand still, and the same picture
  # at any frame number. This is also the probe. A box with no usable GPU
  # can answer none of this, and that is a skip, not a failure.
  if ! PREVIEW_FRAME=0 $LAUNCHERX --preview-shader \
       "$still" "$GOLDEN" "$OUT/preview-still-0.png" >"$OUT/preview-still.txt" 2>&1; then
    sed 's/^/  /' "$OUT/preview-still.txt"
    echo "no usable GPU for a headless preview"
    return 77
  fi
  grep -qx still "$OUT/preview-still.txt" || { echo "$still: reported as animated"; rc=1; }
  PREVIEW_FRAME=7 $LAUNCHERX --preview-shader \
    "$still" "$GOLDEN" "$OUT/preview-still-7.png" >>"$OUT/preview-still.txt" 2>&1 || rc=1
  cmp -s "$OUT/preview-still-0.png" "$OUT/preview-still-7.png" \
    || { echo "$still: frames 0 and 7 differ — the frame number reaches a preset that does not read it"; rc=1; }
  # A preset that does not: said to animate, and two frame numbers really
  # are two pictures (this one interlaces, so it is half the frame).
  PREVIEW_FRAME=0 $LAUNCHERX --preview-shader \
    "$moving" "$GOLDEN" "$OUT/preview-moving-0.png" >"$OUT/preview-moving.txt" 2>&1 || rc=1
  grep -qx animated "$OUT/preview-moving.txt" || { echo "$moving: reported as still"; rc=1; }
  PREVIEW_FRAME=1 $LAUNCHERX --preview-shader \
    "$moving" "$GOLDEN" "$OUT/preview-moving-1.png" >>"$OUT/preview-moving.txt" 2>&1 || rc=1
  if cmp -s "$OUT/preview-moving-0.png" "$OUT/preview-moving-1.png"; then
    echo "$moving: frames 0 and 1 are the same picture — the preview would be frozen"
    rc=1
  fi
  return $rc
}

# ---------------------------------------------------------------- host stage
host_stage() {
  log "host stage ($OS $ARCH)"

  # x87 oracle: only an x86 host has the real x87 to compare against
  if [ "$ARCH" = x86_64 ] || [ "$ARCH" = i686 ]; then
    cc -O2 -std=gnu11 -Iqemu/target/i386/tcg -o build/x87-fast-test tools/x87-fast-test.c -lm \
      && run_check x87-fast x87-fast.log build/x87-fast-test 2000000 \
      || { [ -x build/x87-fast-test ] || { FAIL+=(x87-fast); echo "  FAIL x87-fast (build)"; }; }
  else
    skip x87-fast "x87 oracle needs an x86 host"
  fi

  # the CD-ROM model (M5, doc 17): images written and read back by discx
  cargo build "${CARGO_TGT[@]}" --release -p libdisc -q 2>"$OUT/libdisc-build.log" \
    && run_check libdisc libdisc.log $DISCX selftest "$OUT/disc" \
    || { [ -x $DISCX ] || { FAIL+=(libdisc); echo "  FAIL libdisc (build)"; }; }
  if [ -x $QIMG ] && [ -f "$OUT/disc/mixed.cue" ]; then
    run_check cdimage cdimage.log cdimage_check || true
  else skip cdimage "needs $QIMG and the libdisc check's images"; fi
  if [ -x $DISCX ]; then
    run_check dirdisc dirdisc.log dirdisc_check || true
  else skip dirdisc "needs $DISCX"; fi
  if [ -x $LAUNCHERX ]; then
    run_check dirshelf dirshelf.log dirshelf_check || true
    run_check shelforder shelforder.log shelforder_check || true
    run_check drive drive.log drive_check || true
    run_check machine-details machine-details.log machinedetails_check || true
    if command -v bwrap >/dev/null; then
      run_check accel-choices accel-choices.log accelchoices_check || true
    else skip accel-choices "needs bwrap, for a /dev with no /dev/kvm"; fi
  else
    skip dirshelf "needs $LAUNCHERX"; skip shelforder "needs $LAUNCHERX"
    skip machine-details "needs $LAUNCHERX"; skip accel-choices "needs $LAUNCHERX"
  fi
  if [ -x $LAUNCHERX ] && [ -x $QIMG ] && [ -x $QIO ]; then
    run_check clone clone.log clone_check || true
  else skip clone "needs $LAUNCHERX, $QIMG and qemu-io"; fi
  if [ -x $LAUNCHERX ] && [ -x $QIMG ]; then
    run_check snapshot-tree snapshot-tree.log snaptree_check || true
    run_check win11-snapshots win11-snapshots.log win11snap_check || true
  else
    skip snapshot-tree "needs $LAUNCHERX and $QIMG"
    skip win11-snapshots "needs $LAUNCHERX and $QIMG"
  fi
  # The first-run shader offer and the starter profiles behind it. Needs
  # the preset collection to check what a "yes" writes, so it is skipped
  # on a checkout without the submodule rather than downloading 50 MB
  # inside the suite.
  if [ -x $LAUNCHERX ] && [ -f third_party/slang-shaders/crt/crt-aperture.slangp ]; then
    run_check shader-defaults shader-defaults.log shaderdefaults_check || true
  else
    skip shader-defaults "needs $LAUNCHERX and the slang-shaders submodule"
  fi
  # The launcher's own window (ADR-023: launcher-mitsuami is the one every
  # package installs). Conditional, because it is its own cargo workspace
  # and a Linux host with no GTK 4 builds everything else.
  if [ -x "$LAUNCHER_BIN" ]; then
    run_check mitsuami mitsuami.log mitsuami_check || true
  else
    skip mitsuami "needs $LAUNCHER_BIN (scripts/build.sh mitsuami)"
  fi

  # the host GPU probe (ADR-013): what the launcher tells someone about 3D
  # before a machine exists. The verdict itself is a property of the box,
  # so what is checked here is the part that has to hold on every box:
  # that a host with no Vulkan driver at all is reported unavailable,
  # exits non-zero and is told to install Wine, that a software
  # driver is warned about rather than refused, and that a report always
  # names the loader and the bar it was judged against.
  cargo build "${CARGO_TGT[@]}" --release -p launcher-core --bin launcherx -q 2>"$OUT/host-check-build.log" \
    && run_check host-check host-check.log host_check_probe \
    || { [ -x $LAUNCHERX ] || { FAIL+=(host-check); echo "  FAIL host-check (build)"; }; }

  # the wizard's emulation-optimization switches (patches/qemu/README.md):
  # that a machine nobody has touched still produces the command line it
  # always produced, that each switch lands on the option QEMU looks it up
  # on (a CPU property on `-cpu`, an accelerator property on `-accel tcg`),
  # and that our own QEMU actually accepts the line the launcher writes.
  # The switches' *effect* is the guest batteries' job (x87-guest,
  # sse-guest, rep-guest, smc-guest); this is the wiring between them and
  # a checkbox.
  cargo build "${CARGO_TGT[@]}" --release -p launcher-core --bin launcherx -q 2>"$OUT/optimizations-build.log" \
    && run_check optimizations optimizations.log optimizations_check \
    || { [ -x $LAUNCHERX ] || { FAIL+=(optimizations); echo "  FAIL optimizations (build)"; }; }

  # the wizard's pointer switch: a new Windows machine gets the USB tablet
  # (absolute: the host pointer is the guest cursor and the window never
  # grabs), a new DOS machine does not (its mouse drivers read the PS/2
  # controller), the checkbox adds and removes the device *and* its
  # controller, and our QEMU accepts both machines.
  if [ -x $LAUNCHERX ]; then
    run_check pointer pointer.log pointer_check || true
    run_check voodoo2 voodoo2.log voodoo2_check || true
    run_check extra-args extra-args.log extra_args_check || true
  fi

  # the gamepad (M13): a new machine ignores a controller on every family,
  # each setting puts on the QEMU command line only the device it names,
  # a bundle from a later launcher still loads, and the host end's
  # deadzone and two-threshold hysteresis behave, driven by the scripted
  # pad because no machine running this suite has a controller.
  if [ -x $LAUNCHERX ]; then
    run_check pad pad.log pad_check || true
  fi

  # the "Other" family (doc 06): the machine for an era OS that is neither
  # Windows nor DOS is defined by what it does *not* get, our display
  # adapter, whose driver is a Windows driver. So the check is that a new
  # one comes out on standard hardware, with the cards pinned where an
  # installed guest will not see them move.
  if [ -x $LAUNCHERX ]; then
    run_check family-other family-other.log family_other_check || true
  fi

  # no HPET on a Win98 machine: 98 has no driver for one and showed it as
  # an Unknown Device in Device Manager; asked of our QEMU's device tree.
  if [ -x $LAUNCHERX ]; then
    run_check hpet hpet.log hpet_check || true
  fi

  # the music engines (doc 20): the three of them through the same C API
  # the two QEMU devices drive them through, including the bank the
  # packages ship. No guest, no QEMU, ~3 s.
  if [ -x $SYNTHX ]; then
    run_check libsynth libsynth.log libsynth_check || true
  else
    skip libsynth "needs $SYNTHX (cargo build --release -p libsynth)"
  fi

  # the sound-card and MIDI-port pickers (doc 20 §6), and then the two
  # devices sounding into a wav QEMU recorded itself.
  if [ -x $LAUNCHERX ] && [ -x $SYNTHX ]; then
    run_check music music.log music_check || true
  fi
  if [ -x $QSYS ]; then
    run_check sb-mixer sb-mixer.log sb_mixer_check || true
  fi

  # The Sound Blaster's interrupt line (patch 25): no guest, ~1 s. The
  # card and the PIC are asked directly, because what breaks is invisible
  # from the command line and shows up two programs later.
  if [ -x $QSYS ]; then
    run_check sb16-irq sb16-irq.log sb16_irq_check || true
  fi

  # the display-adapter picker (doc 06): each family offers the adapters
  # it has a real driver question about (Windows ours against the one it
  # has an in-box driver for, Other the two standard ones, DOS the two
  # VESA BIOSes), and changing it must not move the cards pinned below it.
  if [ -x $LAUNCHERX ]; then
    run_check display-adapter display-adapter.log display_adapter_check || true
  fi

  # which Direct3D 9 the host runs the executor on (ADR-007's second
  # amendment): the form's picker, what `auto` resolves to here, and the
  # property on a real QEMU's device.
  if [ -x $LAUNCHERX ]; then
    run_check d3d9 d3d9.log d3d9_backend_check || true
  fi

  # the firmware's legacy BIOS date, which decides whether a *new* Win98
  # install comes out ACPI or PnP-BIOS (doc 06). Asked of a running QEMU,
  # not of the file, because the file is only half the path.
  if [ -x $QSYS ] && [ -d qemu/pc-bios ]; then
    run_check bios-date bios-date.log bios_date_check || true
    run_check machine-map machine-map.log machine_map_check || true
  else
    skip bios-date "needs $QSYS"
  fi

  # the libtpms TPM backend (patch 75, track M20) with no guest: qtest
  # drives tpm-crb's registers, and a fresh TPM, a restart on the same
  # state file, and a savevm / loadvm round trip each have to hold. On
  # the i386 QEMU where there is no x86_64 one (a Mac): its q35 has the
  # same tpm-crb and backend
  tpm_qemu=$QDIR/qemu-system-x86_64
  [ -x "$tpm_qemu" ] || tpm_qemu=$QSYS
  if [ -x "$tpm_qemu" ] && ! $tpm_qemu -tpmdev help 2>&1 | grep -q libtpms; then
    skip tpm-qtest "this QEMU has no libtpms backend (M20 step 5 brings it to Windows)"
  elif [ -x "$tpm_qemu" ]; then
    run_check tpm-qtest tpm-qtest.log env OUT="$OUT/tpm-qtest" \
      tools/tpm-qtest.py "$tpm_qemu" || true
  else
    skip tpm-qtest "needs build/qemu/qemu-system-x86_64 or -i386"
  fi

  # nothing shipped links or loads one of the optional host libraries we
  # disabled (see the function: it is asked of the artefacts, because the
  # configure summary is not what a packager ends up carrying)
  if [ -f "$QDIR/libqemu-embed-i386.$SO" ] || [ -f "$D3DPT_DXVK_LIB" ]; then
    run_check no-optionals no-optionals.log no_optionals_check || true
  else
    skip no-optionals "needs build/qemu or build/dxvk"
  fi

  # the application icon: every size in packaging/icon/ still derived from
  # the one master (doc 07). They are checked in because nothing that
  # needs an icon can draw one. The launcher embeds a PNG at compile
  # time, the Flatpak build is offline, and the Windows package is
  # cross-built without ImageMagick, so a master edited without a
  # regenerate would ship the old picture everywhere but the repository.
  if command -v magick >/dev/null; then
    run_check icons icons.log scripts/gen-icons.sh --check || true
  else
    skip icons "needs ImageMagick"
  fi

  # the Linux package (M6 step 6): staged from this build and asked, with a
  # scrubbed environment, whether it resolves its own player, qemu-img,
  # firmware and guest-tools. The launcher's paths are otherwise baked in
  # at compile time and a regression there only shows on someone else's
  # machine. Rolls no tarball (the check is the point, not the archive).
  if [ ! -x "$LAUNCHER_BIN" ]; then
    # The package installs the launcher (ADR-023), so a checkout that has
    # not built it cannot be packaged at all.
    skip package "needs $LAUNCHER_BIN (scripts/build.sh mitsuami)"
  elif [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ] && [ -x $QIMG ] && [ -d qemu/pc-bios ]; then
    run_check package package.log scripts/package-linux.sh --no-tar --out "$OUT/package" || true
  elif [ "$OS" = Darwin ] && [ -f build/qemu/libqemu-embed-i386.dylib ] && [ -x $QIMG ] && [ -d qemu/pc-bios ]; then
    # The same question in the macOS form (docs/build-macos.md): the .app
    # staged, and everything the loader touches when the packaged player
    # actually runs required to be inside it. No signing, since a
    # Developer ID is not something a test suite should assume, and the checks it would
    # protect all run before it.
    run_check package package.log scripts/package-macos.sh --no-build --no-sign --no-dmg --out "$OUT/package" || true
  else
    skip package "Linux or macOS with build/qemu (libqemu-embed, qemu-img) and qemu/pc-bios only"
  fi

  # The Intel app made on this Apple Silicon Mac (scripts/build.sh
  # --x86_64, docs/build-macos.md "The Intel build"): the same staging and
  # checks under Rosetta, where every Mach-O must be x86_64 and the loader
  # must find them all inside the app. Only when that build exists; a Mac
  # that never made it is not a failure.
  if [ "$OS" = Darwin ] && [ "$ARCH" = arm64 ]; then
    if [ -f build/x86_64/qemu/libqemu-embed-i386.dylib ] && [ -x launcher-mitsuami/target/x86_64-apple-darwin/release/launcher-mitsuami ]; then
      run_check package-x86_64 package-x86_64.log scripts/package-macos.sh --x86_64 --no-build --no-sign --no-dmg --out "$OUT/package-x86_64" || true
    else
      skip package-x86_64 "no Intel build (scripts/build.sh --x86_64)"
    fi
  fi

  # the C ABI (doc 07): `launcher-core` is a library, and this proves it is
  # usable as one. A C program creates a DOS machine through the shared
  # wizard, puts a disc on the shelf and reads both back. It is the only
  # check on the C front end, so a rename or a changed default in a model
  # shows up here as well as in the launcher.
  # A scratch library and shelf, never the user's own.
  if cargo build "${CARGO_TGT[@]}" -p launcher-capi >"$OUT/capi-build.log" 2>&1; then
    CAPI_LIB=""
    for cand in "${RREL%/release}/debug/liblauncher_capi.a" "$RREL/liblauncher_capi.a"; do
      [ -f "$cand" ] && CAPI_LIB="$cand" && break
    done
    # Which native libraries a Rust staticlib needs is the toolchain's
    # to know, not ours to guess: on macOS this one wants objc, iconv and
    # half a dozen frameworks, and the Linux list is not a subset of it.
    # rustc will say; the old list stays as the fallback.
    CAPI_LIBS="$(cargo rustc "${CARGO_TGT[@]}" -q -p launcher-capi -- --print native-static-libs 2>&1 \
                 | sed -n 's/^note: native-static-libs: //p' | tail -1)"
    [ -n "$CAPI_LIBS" ] || CAPI_LIBS="-lstdc++ -lm -ldl -lpthread"
    # (Windows: rustc lists the windows/winapi crates' import libraries,
    # -lwinapi_kernel32 and so on, which live in cargo's build tree; they
    # are mingw's own -lkernel32 under another name)
    [ "$OS" = Windows ] && CAPI_LIBS="${CAPI_LIBS//-lwinapi_/-l}"
    # shellcheck disable=SC2086
    if [ -n "$CAPI_LIB" ] && cc -O1 -std=gnu11 -Ilauncher-capi/include \
         -o build/capi-smoke launcher-capi/examples/smoke.c "$CAPI_LIB" \
         $CAPI_LIBS >>"$OUT/capi-build.log" 2>&1; then
      rm -rf "$OUT/capi"; mkdir -p "$OUT/capi/library"
      : >"$OUT/capi/disc.iso"
      # No Vulkan driver, as `host_check_probe` makes one: the 3D line
      # then has to tell the user to keep our adapter, on every host.
      run_check capi capi.log env \
        VK_DRIVER_FILES=/nonexistent.json VK_ICD_FILENAMES=/nonexistent.json D3DPT_WINE=/nonexistent \
        LAUNCHER_LIBRARY_DIR="$OUT/capi/library" \
        LAUNCHER_DISC_LIBRARY="$OUT/capi/discs.toml" \
        LAUNCHER_SHADER_PROFILES_DIR="$OUT/capi/profiles" \
        build/capi-smoke "$OUT/capi/library" "$OUT/capi/disc.iso" || true
    else
      FAIL+=(capi); echo "  FAIL capi (build)"
    fi
  else
    FAIL+=(capi); echo "  FAIL capi (cargo build -p launcher-capi)"
  fi

  # the embed library's Mesa backend, Linux (EGL) only, one VM per process
  if [ "$OS" = Linux ] && [ -f build/qemu/libqemu-embed-i386.so ]; then
    if cc -O1 -std=gnu11 -Iembed -Iqemu/hw/mesa -o build/embed-3d-test tools/embed-3d-test.c \
         -Lbuild/qemu -lqemu-embed-i386 -Wl,-rpath,"$ROOT/build/qemu" -lepoxy; then
      run_check embed-3d embed-3d.log build/embed-3d-test || true
    else FAIL+=(embed-3d); echo "  FAIL embed-3d (build)"; fi
  else
    skip embed-3d "Linux with build/qemu/libqemu-embed-i386.so only"
  fi


  # decoder + executor without a guest
  if [ -f "$D3DPT_EXEC_LIB" ] && [ -f "$D3DPT_DXVK_LIB" ]; then
    if { [ "$OS" = Windows ] && [ -x "$DP2" ]; } || { [ "$OS" != Windows ] && c++ -std=c++17 -O2 -o "$DP2" tools/d3dpt-dp2-test.cpp \
         -I"$DX" -I"$DX/windows" -I"$DX/directx" -ldl; }; then
      run_check d3dpt-dp2 d3dpt-dp2.log $DP2 "$OUT/dp2-test.bmp" || true
      # Windows below the floor runs the executor on the system's own
      # d3d9.dll (ADR-007's second amendment): the same test there
      [ "$OS" = Windows ] && { run_check d3dpt-dp2-system d3dpt-dp2-system.log \
        env D3DPT_D3D9=system $DP2 "$OUT/dp2-system.bmp" || true; }
      # The same executor on a host with a Vulkan loader and no working
      # device, which every host can be made into. Both loader variables
      # point at a file that does not exist, so the loader finds no ICD and
      # DXVK's instance constructor throws out of Direct3DCreate9. The
      # executor must say "no usable device" and the test must end by its
      # own "no executor" exit (77), not by a signal. The candidate list
      # names the same DXVK by its full path and by its leaf name, and a
      # second Direct3DCreate9 on a DXVK whose constructor threw once
      # dereferenced a null instance (the community app on macOS 15 died
      # at the adapter's realize and never reached the Wine executor).
      # DXVK patch 09 and the executor's once-per-library rule both guard
      # it; this asks the artefacts.
      run_check exec-no-device exec-no-device.log exec_no_device_check || true
    else FAIL+=(d3dpt-dp2); echo "  FAIL d3dpt-dp2 (build)"; fi
  else
    skip d3dpt-dp2 "needs $D3DPT_EXEC_LIB and $D3DPT_DXVK_LIB"
  fi

  # The executor in another process, on Wine (docs/tracks/m15-wine-executor.md,
  # ADR-018): a host below DXVK's Vulkan 1.3 floor runs Direct3D on this.
  # The host test above once more, through build/d3dpt/libd3dpt_exec_remote
  # (the same API, a child d3dpt-exec-host.exe under Wine running the
  # Windows build of the executor on Wine's own d3d9), and its frame must
  # be the frame the in-process executor drew. SKIPs without a Wine or
  # without mingw's pair; on macOS it needs a GUI session (Wine's Mac driver
  # wants the window server), so it SKIPs over plain ssh.
  if wine_bin=$(exec_wine_bin) && [ -f "build/d3dpt/wine/d3dpt-exec-host.exe" ] \
     && [ -f "build/d3dpt/libd3dpt_exec_remote.$SO" ] && [ -f "$OUT/dp2-test.bmp" ]; then
    run_check exec-wine exec-wine.log exec_wine_check "$wine_bin" || true
  else
    skip exec-wine "needs a Wine (D3DPT_WINE), build/d3dpt/wine/ (mingw) and the host frame"
  fi


  # the calibration patterns (doc 09): they render at every era mode, and the
  # circle in `grid` comes out round on the tube it is drawn for
  if cc -O2 -w -o build/crtcal-render tools/crtcal-render.c -lm; then
    mkdir -p "$OUT/crtcal"
    run_check crtcal crtcal.log build/crtcal-render "$OUT/crtcal" || true
  else FAIL+=(crtcal); echo "  FAIL crtcal (build)"; fi

  # the player's display path without a guest: mode analysis, the geometry
  # stage and the CRT preset over every mode in the table (doc 03, M2)
  local preset=third_party/slang-shaders/crt/crt-guest-advanced.slangp
  if have_display && [ -f "$preset" ]; then
    if cargo build "${CARGO_TGT[@]}" --release -p player -q 2>"$OUT/player-build.log"; then
      run_check mode-sweep mode-sweep.log \
        $PLAYER --shader "$preset" --mode-sweep "$OUT/mode-sweep" || true
      # The chain's border sampling, from the run that just happened: the
      # player names the *reason* it is off, and "although this adapter
      # has it" is the one that is our own descriptor's fault. A device
      # opened without `ADDRESS_MODE_CLAMP_TO_BORDER` makes librashader
      # sample clamp-to-edge, and every curved preset then smears its
      # outermost pixels over everything outside the tube.
      if grep -q "clamp-to-border sampling: off although" "$OUT/mode-sweep.log"; then
        FAIL+=(mode-sweep-border)
        echo "  FAIL mode-sweep-border (the device dropped clamp-to-border)"
      fi
    else FAIL+=(mode-sweep); echo "  FAIL mode-sweep (build)"; tail -5 "$OUT/player-build.log"; fi
  else
    skip mode-sweep "needs a display and the slang-shaders submodule"
  fi

  # the mitsuami player (M22) on a private headless sway: the mode sweep
  # through it, the test pattern on its GpuSurface, a key reaching it.
  # Its own workspace, built by hand until it has a build stage.
  if [ -x player-mitsuami/target/release/player-mitsuami ] \
      && command -v sway >/dev/null && command -v grim >/dev/null && command -v wtype >/dev/null; then
    run_check player-mitsuami player-mitsuami.log tools/player-mitsuami-test.sh "$OUT/player-mitsuami" || true
  else
    skip player-mitsuami "needs player-mitsuami built (cd player-mitsuami && cargo build --release), sway, grim and wtype"
  fi

  # the launcher's shader preview, which unlike the player renders only
  # when asked: that it knows which presets it must keep asking about,
  # and that a frame number really does change their picture (doc 07)
  if [ -f third_party/slang-shaders/crt/crt-beans-vga.slangp ] && [ -x $LAUNCHERX ]; then
    run_check preview-anim preview-anim.log preview_anim_check || true
  else
    skip preview-anim "needs the slang-shaders submodule and $LAUNCHERX"
  fi

  # the reference scene and the feature test natively over DXVK; window-less
  # (tools/d3dgame-native/win32_headless.h), so no display is needed
  if [ -f "$D3DPT_DXVK_LIB" ]; then
    # Linux and macOS: the scene through a Win32 shim on DXVK's native
    # build (tools/d3dgame-native). Windows: the scene's own source as an
    # x64 program, DXVK's d3d9.dll beside it (an application's folder is
    # searched before system32), so the oracle is DXVK here too. It opens
    # a window for the length of the run.
    local flags=(-I"$DX" -I"$DX/windows" -I"$DX/directx" -Lbuild/dxvk/src/d3d9 -ldxvk_d3d9 \
                 -Wl,-rpath,"$ROOT/build/dxvk/src/d3d9")
    local nat=../
    if [ "$OS" = Windows ]; then
      nat=../test-bin/; mkdir -p build/test-bin
      native_build() { cp build/win/dxvk/src/d3d9/d3d9.dll build/test-bin/d3d9.dll \
        && gcc -O2 -o build/test-bin/d3dgame9-native.exe guest-tools/src/d3dgame9.c -ld3d9 -lgdi32 -luser32 \
        && gcc -O2 -o build/test-bin/d3dfeat9-native.exe guest-tools/src/d3dfeat9.c -ld3d9 -lgdi32 -luser32; }
    else
      native_build() { c++ -std=c++17 -O2 -o build/d3dgame9-native tools/d3dgame9-native.cpp "${flags[@]}" \
        && c++ -std=c++17 -O2 -o build/d3dfeat9-native tools/d3dfeat9-native.cpp "${flags[@]}"; }
    fi
    if native_build; then
      rm -f "$OUT/D3DGAME9.LOG" "$OUT/D3DFEAT9.LOG"
      local wsi=(DXVK_WSI_DRIVER="${DXVK_WSI_DRIVER:-Headless}"); [ "$OS" = Windows ] && wsi=()
      ( cd "$OUT" && env BOXLOG="$OUT" "${wsi[@]}" ${nat}d3dgame9-native -frames 600 -dump 300 g9-native.bmp ) >"$OUT/d3dgame9-native.log" 2>&1
      if [ -f "$OUT/g9-native.bmp" ]; then
        run_check d3dgame9-nat d3dgame9-golden.log tools/bmpdiff.py "$GOLDEN" "$OUT/g9-native.bmp" \
          --mask "$HUD_MASK" --tolerance 8 --max-over "$BUDGET" -o "$OUT/g9-native-vs-rig.bmp" \
          && sed -n 1,2p "$OUT/d3dgame9-golden.log" | sed 's/^/       /'
      else FAIL+=(d3dgame9-nat); echo "  FAIL d3dgame9-nat (no frame) — $OUT/d3dgame9-native.log"; tail -3 "$OUT/d3dgame9-native.log"; fi
      ( cd "$OUT" && env BOXLOG="$OUT" "${wsi[@]}" ${nat}d3dfeat9-native -frames 600 -dump 300 f9-native.bmp ) >"$OUT/d3dfeat9-native.log" 2>&1
      # the occlusion query must have *resolved* (S_OK), not merely been
      # logged: a window-less client that nothing paces runs so far ahead of
      # the CS thread that GetData spins out and reports S_FALSE with 0
      # pixels, and then only the guest-vs-native diff notices
      if [ -f "$OUT/f9-native.bmp" ] && grep -q "occlusion query at frame .*: 0x00000000, [1-9]" "$OUT/D3DFEAT9.LOG"; then
        PASS+=(d3dfeat9-nat); echo "  PASS d3dfeat9-nat"
        grep "occlusion query\|getters" "$OUT/D3DFEAT9.LOG" | sed 's/^/       /'
      else FAIL+=(d3dfeat9-nat); echo "  FAIL d3dfeat9-nat — $OUT/d3dfeat9-native.log"; grep "occlusion query" "$OUT/D3DFEAT9.LOG" | sed 's/^/       /'; tail -3 "$OUT/d3dfeat9-native.log"; fi
    else FAIL+=(d3d-native); echo "  FAIL d3d native harness (build)"; fi
  else
    skip d3dgame9-nat "needs build/dxvk"
    skip d3dfeat9-nat "needs build/dxvk"
  fi
}

# --------------------------------------------------------------- guest stage
guest_stage() {
  log "guest stage"
  local img="${WINXP_IMG:-$HOME/vms/winxp.qcow2}"
  # Windows: a launcher machine of the user's (only read, through an overlay)
  [ "$OS" = Windows ] && img="${WINXP_IMG:-$DATA_DIR/machines/${WINXP_MACHINE:-basexp-br}/disk.qcow2}"
  local iso="${GUEST_ISO:-$(ls -t guest-tools/out/guest-tools-3dfx-*.iso 2>/dev/null | head -1)}"
  if command -v nasm >/dev/null && command -v mcopy >/dev/null && [ -x $QSYS ]; then
    if [ -f build/images/144m/x86BOOT.img ]; then
      run_check x87-guest x87-guest.log python3 tools/x87-guest-test.py || true
      run_check rep-guest rep-guest.log python3 tools/rep-guest-test.py || true
      run_check smc-guest smc-guest.log python3 tools/smc-guest-test.py || true
      run_check sse-guest sse-guest.log python3 tools/sse-guest-test.py || true
      run_check atapi-guest atapi-guest.log python3 tools/atapi-guest-test.py || true
      if [ "$OS" = Linux ]; then
        run_check atapi-read-error atapi-read-error.log env ATAPI_READ_ERROR=1 python3 tools/atapi-guest-test.py || true
      else skip atapi-read-error "Linux only (an LD_PRELOAD over glibc's pread64)"; fi
      run_check midi-guest midi-guest.log python3 tools/midi-guest-test.py || true
      run_check pit-guest pit-guest.log python3 tools/pit-guest-test.py || true
      run_check voodoo-guest voodoo-guest.log python3 tools/voodoo-guest-test.py || true
      run_check voodoo-guest-d3dpt voodoo-guest-d3dpt.log env VGA=d3dpt python3 tools/voodoo-guest-test.py || true
      run_check voodoo-guest-mmiofifo voodoo-guest-mmiofifo.log env RAMFIFO=off python3 tools/voodoo-guest-test.py || true
      run_check voodoo-guest-undither voodoo-guest-undither.log env UNDITHER=on python3 tools/voodoo-guest-test.py || true
      run_check vbe-palette vbe-palette.log env VBEPAL=1 python3 tools/vga-dirty-guest-test.py vesa || true
      # The gameport as a DOS guest reads it (M13 path B). Unlike its
      # neighbours this one runs the **player**, because the pad reaches a
      # guest through the embed library and a bare QEMU has a gameport
      # nothing ever moves. So it wants a display for the player's window
      # and skips rather than fails without one.
      if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ -x $PLAYER ]; then
        run_check pad-guest pad-guest.log python3 tools/pad-guest-test.py || true
      else
        skip pad-guest "needs $PLAYER and a display (it runs the player)"
      fi
    # **One skip per battery, not one skip standing for all of them.** Every
    # DOS battery is gated on the same floppy. Reported as a single
    # `SKIP x87-guest`, a fresh worktree (no `build/images/144m/x86BOOT.img`
    # until something fetches it) came back "37 passed, 1 skipped" beside
    # the main checkout's "42 passed, 0 skipped", which reads as two
    # different suites. The missing ones include `atapi-guest`, the only
    # check that reads a disc from inside a guest at all.
    else for c in x87-guest rep-guest smc-guest sse-guest atapi-guest atapi-read-error midi-guest pit-guest voodoo-guest voodoo-guest-d3dpt voodoo-guest-mmiofifo voodoo-guest-undither vbe-palette pad-guest; do
      skip "$c" "no FreeDOS floppy yet: run tools/x87-guest-test.py once to fetch it"
    done; fi
  else for c in x87-guest rep-guest smc-guest sse-guest atapi-guest atapi-read-error midi-guest pit-guest voodoo-guest voodoo-guest-d3dpt voodoo-guest-mmiofifo voodoo-guest-undither vbe-palette pad-guest; do
    skip "$c" "needs nasm, mtools and build/qemu"
  done; fi
  case "$OS" in Linux|Windows) ;; *) skip guest "Linux and Windows only for now (mtools)"; return;; esac
  for t in mcopy mmd mformat; do command -v $t >/dev/null || { skip guest "needs $t (mtools)"; return; }; done
  [ -f "$img" ] || { skip guest "no XP image at $img (WINXP_IMG)"; return; }
  [ -n "$iso" ] && [ -f "$iso" ] || { skip guest "no guest-tools ISO (guest-tools/build-wrappers.sh)"; return; }
  [ -x $QSYS ] || { skip guest "no $QSYS"; return; }
  # the CD-ROM backend: XP copies a converted guest-tools disc through cdrom.sys (doc 17 §6.3)
  if [ -x $DISCX ] && command -v bsdtar >/dev/null; then
    # bsdtar keeps the ISO's read-only modes: make the previous extraction deletable first
    [ -d "$OUT/gt-iso" ] && chmod -R u+w "$OUT/gt-iso"
    rm -rf "$OUT/gt-iso"; mkdir -p "$OUT/gt-iso" "$OUT/disc"
    if bsdtar -xf "$iso" -C "$OUT/gt-iso" 2>/dev/null && chmod -R u+w "$OUT/gt-iso" \
       && $DISCX convert "$iso" "$OUT/disc/gt.cue" --audio "$OUT/disc/tone.wav" >/dev/null 2>&1; then
      # with mingw the run also plays the tone track through MCI into a wav (CD-DA, doc 17 §5.4)
      cdtest=""
      if command -v i686-w64-mingw32-gcc >/dev/null && i686-w64-mingw32-gcc -O2 -D__MSVCRT_VERSION__=0x700 -mcrtdll=msvcrt-os \
           -march=pentium3 -mtune=generic -o "$OUT/CDTEST.EXE" guest-tools/src/cdtest.c -lwinmm 2>"$OUT/cdtest-build.log"; then
        cdtest="$OUT/CDTEST.EXE"
      else echo "  (no mingw: guest-cdimage runs without the CD audio part)"; fi
      CDTEST="$cdtest" run_check guest-cdimage guest-cdimage.log tools/xp-cdimage-test.sh "$img" "$OUT/disc/gt.cue" "$OUT/gt-iso" "$OUT/cdimage-xp" || true
      # the same tree again, this time served as a folder rather than an
      # image (isodir, M5g): same reference, same comparison, so the disc
      # being generated on the fly is the only difference
      run_check guest-dirdisc guest-dirdisc.log tools/xp-cdimage-test.sh "$img" "isodir:$OUT/gt-iso" "$OUT/gt-iso" "$OUT/dirdisc-xp" || true
    else skip guest-cdimage "could not extract or convert $iso"; fi
  else skip guest-cdimage "needs $DISCX and bsdtar"; fi
  [ -f "$D3DPT_EXEC_LIB" ] || { skip guest "no $D3DPT_EXEC_LIB"; return; }
  [ -f "$OUT/g9-native.bmp" ] && [ -f "$OUT/f9-native.bmp" ] || { skip guest "run the host stage first (native oracle frames)"; return; }
  # XP on the display driver through its own runtime (M16 step 7): a fresh
  # overlay, the driver installed from the ISO, then the scenes in one boot
  rm -f "$OUT"/G9.BMP "$OUT"/G8.BMP "$OUT"/F9.BMP "$OUT"/guest-*.log
  echo "  XP: $img (overlay), ISO: $iso"
  if OUT="$OUT/xpdx9" tools/xp-dx9-test.sh "$img" "$iso" > "$OUT/xpdx9.log" 2>&1; then
    echo "  $(tail -1 "$OUT/xpdx9.log")"
  else FAIL+=(guest-run); echo "  FAIL guest-run — $OUT/xpdx9.log"; tail -3 "$OUT/xpdx9.log" | sed 's/^/       /'; fi
  local g
  for g in G9.BMP G8.BMP F9.BMP guest-ddvmtest.log guest-d3dgame9.log guest-d3dgame8.log guest-d3dfeat9.log; do
    [ -f "$OUT/xpdx9/$g" ] && cp "$OUT/xpdx9/$g" "$OUT/"
  done
  # a Vice City-style launcher check through Windows' own ddraw.dll: the driver's video memory
  if grep -qiF "ddraw.dll is C:\\WINDOWS" "$OUT/guest-ddvmtest.log" 2>/dev/null && grep -q ": enough" "$OUT/guest-ddvmtest.log"; then
    PASS+=(guest-ddvm); echo "  PASS guest-ddvm"; grep "GetAvailableVidMem" "$OUT/guest-ddvmtest.log" | tr -d '\r' | sed 's/^/       /'
  else FAIL+=(guest-ddvm); echo "  FAIL guest-ddvm — $OUT/guest-ddvmtest.log"; cat "$OUT/guest-ddvmtest.log" 2>/dev/null | tr -d '\r' | sed 's/^/       /'; fi

  local f
  for f in G9 G8; do
    if [ ! -f "$OUT/$f.BMP" ]; then FAIL+=("guest-$f"); echo "  FAIL guest-$f (no frame on the scratch disk)"; continue; fi
    run_check "guest-$f=native" "guest-$f-native.log" tools/bmpdiff.py "$OUT/g9-native.bmp" "$OUT/$f.BMP" --mask "$HUD_MASK" || true
    run_check "guest-$f~rig" "guest-$f-rig.log" tools/bmpdiff.py "$GOLDEN" "$OUT/$f.BMP" --mask "$HUD_MASK" --tolerance 8 --max-over "$BUDGET" || true
  done
  if [ ! -f "$OUT/F9.BMP" ]; then FAIL+=(guest-F9); echo "  FAIL guest-F9 (no frame on the scratch disk)"
  else
    run_check "guest-F9=native" guest-F9-native.log cmp "$OUT/f9-native.bmp" "$OUT/F9.BMP" || true
    grep -h "occlusion query\|getters" "$OUT/D3DFEAT9.LOG" | sort > "$OUT/f9-native.lines"
    grep -h "occlusion query\|getters" "$OUT/guest-d3dfeat9.log" 2>/dev/null | tr -d '\r' | sort > "$OUT/f9-guest.lines"
    run_check "guest-F9-log=native" guest-F9-log.log diff "$OUT/f9-native.lines" "$OUT/f9-guest.lines" || true
  fi

  # The USB HID pad as a Windows game finds it (M13 path A), **last in the
  # stage**: the same scripted pad, through the guest's own HID stack and
  # DirectInput. Each is its own boot (~60 s on XP) rather than a passenger
  # on the one above, because that machine has no `usb-gamepad` and adding
  # one would change the hardware every other guest check runs against.
  # They go at the back because they are the newest checks here, and a new
  # check should not be able to perturb an established one by running
  # before it.
  local pad98="${WIN98_PAD_MACHINE:-claude98}"
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ -x $PLAYER ]; then
    run_check pad-guest-xp pad-guest-xp.log python3 tools/pad-guest-test.py xp "$img" || true
    # ...and on Windows 98, which is a *machine* rather than an image: the
    # pad's driver has to have been bound once (98 asks for its own source
    # files the first time and there is nobody here to answer), so this
    # names a launcher machine and reads the disk and display adapter out
    # of its bundle. Skipped where that machine does not exist rather than
    # assuming anyone's library looks like this one.
    if [ -f "$DATA_DIR/machines/$pad98/machine.toml" ]; then
      run_check pad-guest-98 pad-guest-98.log python3 tools/pad-guest-test.py win98 "$pad98" || true
    else
      skip pad-guest-98 "no launcher machine '$pad98' with the pad installed (WIN98_PAD_MACHINE)"
    fi
  else
    skip pad-guest-xp "needs $PLAYER and a display (it runs the player)"
    skip pad-guest-98 "needs $PLAYER and a display (it runs the player)"
  fi

}

win98_checks() { # the Win98 driver's Direct3D (M16 step 5): its own boots, whatever the XP stage had
  # Win98 on the display driver (M16 step 5), after the XP stage: Microsoft's
  # d3d9.dll / d3d8.dll through the 9x HAL. `win98-dx9` is one boot of the
  # reference scenes against the native frames above, the DX8 probes and
  # DDTEST's sysmem blits (tools/win98-dx9-test.sh); `win98-winetest` is
  # Wine's suites against the driver's Win98 baseline, which only shrinks
  # (reference/winetest/w98-driver.txt). Both on a raw copy of a launcher
  # machine (base98-br: DirectX 9.0c, 3dfx's driver installed), made once
  # and kept; TCG, about 5 minutes together. Skipped where the machine
  # does not exist.
  log "win98 checks"
  if { [ "$OS" != Linux ] && [ "$OS" != Windows ]; } || ! command -v mcopy >/dev/null || [ ! -x $QSYS ] || [ ! -f "$D3DPT_EXEC_LIB" ] \
     || [ ! -f "$OUT/g9-native.bmp" ]; then
    skip win98-dx9 "Linux, mtools, build/qemu, the executor and the host stage's native frames first"
    skip win98-winetest "the same"
    return
  fi
  local m98="${WIN98_DX9_MACHINE:-base98-br}"
  local d98="$DATA_DIR/machines/$m98/disk.qcow2"
  if [ -f "$d98" ] && [ -f build/winetest/out/wtrun.exe ]; then
    run_check win98-dx9 win98-dx9.log env RAW="$OUT/w98.raw" tools/win98-dx9-test.sh "$d98" || true
    grep "^  [PF]A[SI][SL]" "$OUT/win98-dx9.log" | sed 's/^/     /'
    run_check win98-winetest win98-winetest.log env RAW="$OUT/w98.raw" OUT="$OUT/w98wt" WT_BASELINE=w98-driver \
      tools/win98-winetest.sh "$d98" || true
    tail -9 "$OUT/win98-winetest.log" | sed 's/^/     /'
  elif [ -f "$d98" ]; then
    skip win98-dx9 "no build/winetest/out (guest-tools/build-winetests.sh)"
    skip win98-winetest "no build/winetest/out (guest-tools/build-winetests.sh)"
  else
    skip win98-dx9 "no launcher machine '$m98' (WIN98_DX9_MACHINE)"
    skip win98-winetest "no launcher machine '$m98' (WIN98_DX9_MACHINE)"
  fi
}

case "$STAGE" in
  host) host_stage;;
  guest) guest_stage; win98_checks;;
  all) host_stage; guest_stage; win98_checks;;
  *) echo "usage: $0 [host|guest|all]"; exit 2;;
esac

printf '\n%d passed, %d failed, %d skipped\n' ${#PASS[@]} ${#FAIL[@]} ${#SKIP[@]}
for s in "${SKIP[@]}"; do echo "  skip: $s"; done
for f in "${FAIL[@]}"; do echo "  FAIL: $f"; done
[ ${#FAIL[@]} = 0 ]
