#!/usr/bin/env bash
# The M15 spike inside a macOS guest (docs/tracks/m15-wine-executor.md,
# ADR-019): a pre-26 macOS in a UTM virtual machine on this Mac stands in
# for the community build's user (no Vulkan 1.3, so the executor has to
# run on Wine). This drives the display driver's host test through the
# Windows build of the executor on Wine's own d3d9 *in that guest*, over
# ssh, and diffs the frame it drew against the DXVK frame this host made.
#
#   tools/macvm-wine-spike.sh <user>@<guest ip>        # utmctl ip-address <vm> gives the ip
#   RENDERER=vulkan …                                   # wined3d's Vulkan renderer (MoltenVK) instead of GL
#
# It expects, on this host, what the spike already made:
#   build/wine/wine-staging-*-osx64.tar.xz   WineHQ's macOS build (x86_64)
#   build/wine-spike/d3dpt_exec.dll          scripts/build-d3dpt-exec.sh --windows
#   build/wine-spike/d3dpt-dp2-test.exe      the display driver's host test, mingw build
#   build/wine-spike/libwinpthread-1.dll     Homebrew's mingw links it dynamically
#   build/test/dp2-test.bmp                  the DXVK frame (the oracle)
# and, in the guest, done once by hand: an account logged in at the
# console with Remote Login on, Rosetta installed (`softwareupdate
# --install-rosetta --agree-to-license`, it wants an administrator), and
# passwordless sudo for that account (`echo 'USER ALL=(ALL) NOPASSWD: ALL'
# | sudo tee /etc/sudoers.d/USER`). Wine has to run *in the guest's GUI
# session*. A process started over ssh has no window server, wined3d
# cannot make even its capability-probe window, and the only way from ssh
# into that session is `launchctl asuser`, which is root's.
#
# What this found (macOS 15.6.1 guest, WineHQ 11.17, the Air): Apple's
# paravirtual GPU has Metal and no accelerated OpenGL (CGL offers "Apple
# Software Renderer" only, to arm64 and x86_64 alike), and Wine's Mac
# driver demands kCGLPFAAccelerated for its bootstrap
# context (winemac.drv/opengl.c init_context; AllowSoftwareRendering only
# widens the list it enumerates afterwards), so **Wine has no OpenGL in
# such a guest at all** and wined3d's GL renderer, the community build's
# path, cannot be tested in a VM. The VM answers the Vulkan-renderer
# question instead (RENDERER=vulkan: MoltenVK sees the paravirtual GPU,
# the DDI frame differs from DXVK's in 8 % of pixels and the DLL path's
# device creation fails on a depth format), which is not the plan. The GL
# path's test is a real pre-26 macOS: a second APFS volume on this Mac.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
target="${1:?usage: $0 user@guest-ip}"
user="${target%@*}"
RENDERER="${RENDERER:-gl}"
OUT="${OUT:-build/macvm/spike}"; mkdir -p "$OUT"
tar=$(ls build/wine/wine-staging-*-osx64.tar.xz | head -1)
for f in build/wine-spike/d3dpt_exec.dll build/wine-spike/d3dpt-dp2-test.exe \
         build/wine-spike/libwinpthread-1.dll \
         build/test/dp2-test.bmp; do
  [ -f "$f" ] || { echo "missing $f (run the spike on the host first: the track doc)"; exit 1; }
done
ssh_() { ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$target" "$@"; }

echo "==> guest"
ssh_ 'sw_vers | head -2; uname -m
      arch -x86_64 /usr/bin/true 2>/dev/null && echo "rosetta: yes" || echo "rosetta: NO -- softwareupdate --install-rosetta --agree-to-license in the guest first"
      sudo -n true 2>/dev/null && echo "sudo: yes" || echo "sudo: NO -- passwordless sudo for this account first (the header)"
      [ "$(stat -f %Su /dev/console)" = "$(id -un)" ] && echo "console: yes" || echo "console: NO -- log this account in at the guest'"'"'s screen"' | tee "$OUT/guest.txt"
grep -q "rosetta: yes" "$OUT/guest.txt" && grep -q "sudo: yes" "$OUT/guest.txt" && grep -q "console: yes" "$OUT/guest.txt" || exit 1

echo "==> copying Wine ($(du -h "$tar" | cut -f1)) and the spike"
ssh_ 'mkdir -p ~/2ksbox-spike'
scp -q "$tar" build/wine-spike/d3dpt_exec.dll build/wine-spike/d3dpt-dp2-test.exe \
    build/wine-spike/libwinpthread-1.dll "$target:~/2ksbox-spike/"
ssh_ 'cd ~/2ksbox-spike && { [ -d "Wine Staging.app" ] || tar xJf wine-staging-*-osx64.tar.xz; }'

# The guest-side script: written whole, run in the GUI session.
ssh_ "cat > ~/2ksbox-spike/run.sh" <<EOF
#!/bin/bash
cd "\$HOME/2ksbox-spike"
W="\$HOME/2ksbox-spike/Wine Staging.app/Contents/Resources/wine/bin/wine"
export WINEPREFIX="\$HOME/2ksbox-spike/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="d3d9=b;mscoree,mshtml="
"\$W" wineboot -i >/dev/null 2>&1 || true
"\$W" reg add "HKCU\\\\Software\\\\Wine\\\\Direct3D" /v renderer /d $RENDERER /f >/dev/null 2>&1
"\$W" --version
D3DPT_EXEC_LIB=d3dpt_exec.dll D3DPT_D3D9=system WINEDEBUG=-all,+d3d,+wgl "\$W" d3dpt-dp2-test.exe dp2.bmp > dp2.log 2>&1; echo "dp2 exit=\$?"
grep -o 'GL_RENDERER "[^"]*"' dp2.log | head -1
grep "Using the .* renderer" dp2.log | head -1
grep -q "init_context CGLChoosePixelFormat() failed" dp2.log && echo "no OpenGL for Wine in this guest: the Mac driver found no accelerated pixel format (a VM's paravirtual GPU has none)"
grep "PASSED\|FAILED" dp2.log | tail -1
EOF

echo "==> running in the guest's GUI session (the first start makes the prefix: ~30 s)"
ssh_ "chmod +x ~/2ksbox-spike/run.sh; sudo -n launchctl asuser \$(id -u) sudo -u $user -i bash ~/2ksbox-spike/run.sh" | tee "$OUT/run.txt"

echo "==> frames"
for f in dp2.bmp dp2.log; do scp -q "$target:~/2ksbox-spike/$f" "$OUT/$f" 2>/dev/null || true; done
rc=0
[ -f "$OUT/dp2.bmp" ] && python3 tools/bmpdiff.py build/test/dp2-test.bmp "$OUT/dp2.bmp" --tolerance 8 -o "$OUT/dp2-diff.bmp" || rc=1
[ $rc = 0 ] && echo "PASS: the frame matches the DXVK frame" || echo "FAIL: the frame is missing or differs (logs and diff images in $OUT)"
exit $rc
