#!/usr/bin/env bash
# A machine that cannot reset in place starts again cold (track M20 step
# 5): x64 Windows 11 on a Windows host, where a reset under WHPX stops the
# VM. The launcher runs it with -no-reboot (bundle::Machine::restarts_cold);
# a reset then ends QEMU's loop, the embed library says it was a reset
# (qemu_embed_stopped_by_reset, API v13), the player asks "The machine
# restarted" and on Restart exits with status 75, which the launcher's
# Machines::reap answers by starting the machine again.
#
#   LAUNCHERX=<launcherx> QIMG=<qemu-img> OUT=<dir> tools/restart-cold-test.sh
#
# 1. The launcher's Windows 11 machine for this host: on Windows q35
#    without SMM, EDK2's build without Secure Boot, no TPM and -no-reboot;
#    elsewhere SMM, the secure build, a TPM and no -no-reboot.
# 2. The x64 player on a bare q35 with -no-reboot and no disk (no guest
#    needed: a QMP system_reset ends the loop the way the guest's own
#    does), its question answered by PLAYER_RESET_ANSWER: "restart" exits
#    75, "close" exits 0. Without -no-reboot the same reset leaves the
#    player running.
# The player's window opens for each run (a few seconds).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${OUT:-build/restart-cold}"
LX="${LAUNCHERX:-}"
rc=0
rm -rf "$OUT" && mkdir -p "$OUT/library"
OUT="$(cd "$OUT" && pwd)"
case "$(uname -s)" in MINGW*|MSYS*) WIN=1; EXE=.exe ;; *) WIN=""; EXE="" ;; esac

# --- 1. the launcher's arguments ----------------------------------------
if [ -n "$LX" ] && [ -x "$LX" ]; then
  export LAUNCHER_LIBRARY_DIR="$OUT/library" LAUNCHER_DISC_LIBRARY="$OUT/discs.toml" \
    LAUNCHER_SHADER_PROFILES_DIR="$OUT/profiles" LAUNCHER_QEMU_IMG_BIN="${QIMG:-}"
  bundle="$($LX --wizard-new win11 W11 64 2>/dev/null | tail -1)"
  if [ -f "$bundle" ]; then
    a="$($LX --print-args "$bundle")"
    has() { case " $a " in *"$1"*) return 0 ;; esac; return 1; }
    if [ -n "$WIN" ]; then
      has " -no-reboot " || { echo "FAIL: no -no-reboot on a Windows host's Windows 11 machine"; rc=1; }
      has "edk2-x86_64-code.fd" || { echo "FAIL: not EDK2's build without Secure Boot"; rc=1; }
      has "-tpmdev" && { echo "FAIL: a TPM on a Windows host (QEMU builds none there)"; rc=1; }
      has "smm=on" && { echo "FAIL: SMM asked of WHPX"; rc=1; }
      has "property=secure" && { echo "FAIL: secure flash without SMM"; rc=1; }
    else
      has " -no-reboot " && { echo "FAIL: -no-reboot off a Windows host"; rc=1; }
      case "$a" in *"virt,gic"*) ;; *)
        has "q35,smm=on" || { echo "FAIL: no SMM on an x64 machine off Windows"; rc=1; }
        has "edk2-x86_64-secure-code.fd" || { echo "FAIL: not the secure EDK2 build"; rc=1; } ;;
      esac
      has "-tpmdev libtpms" || { echo "FAIL: no TPM off a Windows host"; rc=1; }
    fi
    [ $rc = 0 ] && echo "launcher: the Windows 11 machine's arguments fit this host"
  else
    echo "FAIL: --wizard-new win11 made no bundle"; rc=1
  fi
else
  echo "(no launcherx: the launcher's arguments are not checked)"
fi

# --- 2. the player ------------------------------------------------------
PLAYER=player-mitsuami/target/qemu-x86_64/release/player-mitsuami$EXE
[ -x "$PLAYER" ] || { echo "(no $PLAYER: the player's restart is not checked)"; exit $rc; }
if [ -n "$WIN" ]; then
  export PATH="$ROOT/build/win/qemu-msvc:$PATH"   # libqemu-embed-x86_64.dll
  QMP=tcp:127.0.0.1:44731; QMPARG=$QMP; PB="$(cygpath -m "$ROOT/qemu/pc-bios")"
else
  QMP="/tmp/rcold-$$.sock"; QMPARG="unix:$QMP"; PB="$ROOT/qemu/pc-bios"
fi
qmp() { python3 tools/qmpc.py "$QMP" json "$1" >/dev/null 2>&1; }

# play <name> <answer> <reboot-flag>: start, reset over QMP, wait for the
# exit (or not); sets STATUS (empty: still running after 15 s)
play() {
  local log="$OUT/$1.log" i
  PLAYER_RESET_ANSWER="$2" PLAYER_KEYBOARD_CAPTURE=0 "$PLAYER" -- -L "$PB" -machine q35 -accel tcg -m 128 \
    -nic none $3 -qmp "$QMPARG,server=on,wait=off" >"$log" 2>&1 &
  local pid=$!
  for i in $(seq 60); do qmp '{"execute":"query-status"}' && break; sleep 0.5; done
  sleep 2
  qmp '{"execute":"system_reset"}' || echo "  ($1: the reset was not sent)"
  STATUS=""
  for i in $(seq 30); do
    kill -0 $pid 2>/dev/null || { wait $pid; STATUS=$?; break; }
    sleep 0.5
  done
  if [ -z "$STATUS" ]; then
    qmp '{"execute":"quit"}'
    for i in $(seq 20); do kill -0 $pid 2>/dev/null || break; sleep 0.5; done
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
  fi
}

play restart restart -no-reboot
if [ "$STATUS" = 75 ] && grep -q "the machine reset; restarting" "$OUT/restart.log"; then
  echo "player: a reset under -no-reboot, answered Restart, exits 75"
else
  echo "FAIL: Restart exited '${STATUS:-still running}', not 75"; tail -5 "$OUT/restart.log"; rc=1
fi
play close close -no-reboot
if [ "$STATUS" = 0 ] && grep -q "the machine reset; closing" "$OUT/close.log"; then
  echo "player: answered Close, exits 0"
else
  echo "FAIL: Close exited '${STATUS:-still running}', not 0"; tail -5 "$OUT/close.log"; rc=1
fi
play inplace restart ""
if [ -z "$STATUS" ]; then
  echo "player: without -no-reboot the reset happens in place"
else
  echo "FAIL: without -no-reboot the player exited ($STATUS) on a reset"; tail -5 "$OUT/inplace.log"; rc=1
fi
exit $rc
