#!/usr/bin/env bash
# Build the macOS app (doc 07: signed .app, JIT entitlement, notarized).
# Stage everything a user's Mac needs into one bundle that depends on
# nothing but the system, sign it for Developer ID, notarize it and roll a
# disk image.
#
#   scripts/package-macos.sh                      # build, stage, check, sign, notarize, dmg
#   scripts/package-macos.sh --no-build           # use what is in target/release
#   scripts/package-macos.sh --no-sign            # stage and check only (ad-hoc signed)
#   scripts/package-macos.sh --no-notarize        # sign, but do not submit
#   scripts/package-macos.sh --no-dmg             # leave the .app, no image
#   scripts/package-macos.sh --identity NAME      # default: the one Developer ID Application
#   scripts/package-macos.sh --keychain-profile P # notarytool credentials (default: 2ksbox-notary)
#   scripts/package-macos.sh --out DIR            # default build/macos
#   scripts/package-macos.sh --community          # ADR-019's community build, which carries
#                                                 # the Direct3D executor for Wine (M15). The
#                                                 # App Store build never starts Wine
#   scripts/package-macos.sh --app-store --provision FILE
#                                                 # the App Store build for upload: sandboxed,
#                                                 # signed as Apple Distribution with FILE (a
#                                                 # Mac App Store provisioning profile for
#                                                 # com.2ksbox.2ksbox) embedded, macOS 26+, and
#                                                 # a .pkg signed as Mac Installer Distribution
#                                                 # (--installer-identity NAME) instead of a
#                                                 # notarized DMG. Into build/macos-app-store
#   scripts/package-macos.sh --x86_64             # on an Apple Silicon Mac: the Intel app, from
#                                                 # scripts/build.sh --x86_64 (build/x86_64,
#                                                 # build/deps/x86_64, target/x86_64-apple-darwin)
#                                                 # into build/macos-x86_64. Always the community
#                                                 # build, and with no Vulkan at all: no driver
#                                                 # exists for an Intel Mac (ADR-019), so its
#                                                 # Direct3D is the executor on Wine, and Wine
#                                                 # there is native x86_64. Untested until an
#                                                 # Intel Mac has run the reference scene
#                                                 # (docs/build-macos.md, "The Intel build")
#
# Notarization needs credentials, stored once by you:
#   xcrun notarytool store-credentials 2ksbox-notary \
#       --apple-id <you@example.com> --team-id <TEAMID> --password <app-specific-password>
#
# How this differs from the Linux package (scripts/package-linux.sh), and
# why it is a second script rather than a switch:
#
# * Nothing may come from outside the bundle. A Linux package relies on
#   the distribution for glib, pixman, zstd and the rest. A Mac has none
#   of them, and the Mac that runs the app has no Homebrew, no XQuartz and
#   no Vulkan. So the script copies in the whole non-system dylib closure
#   and rewrites every install name to @rpath. That is most of the script.
# * The launcher brings no toolkit. It is `launcher-mitsuami` (ADR-023),
#   on AppKit, which every Mac has, so there are no frameworks, plugins or
#   QML modules to deploy, and the closure below is the whole story.
# * The bundle is the prefix. `Contents` has the `lib/libexec/share`
#   shape of a Unix prefix, and `launcher_core::paths` finds it by the same
#   `share/2ksbox` marker. `MacOS/` takes the place of `bin/`, because it
#   is the one directory macOS launches an executable from.
# * Signing goes inside-out and the floor is ours. Every Mach-O is
#   signed before the thing that contains it. The oldest macOS the app
#   runs on is scripts/macos-floor.sh's number, and scripts/build.sh
#   builds everything for it: QEMU's libraries from source
#   (scripts/build-deps.sh), so nothing in the app comes from the Mac's
#   package manager. LSMinimumSystemVersion is measured from what the
#   bundle carries, and any file above the floor fails the package.
#
# It does not build QEMU or DXVK. Those come from `scripts/build.sh`. The
# script reports anything missing by name instead of leaving it out
# quietly. The optional companions (the Direct3D executor, Vulkan, the
# Wine pair) are only a warning each, and cost the guest one accelerated
# path.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ "$(uname -s)" = Darwin ] || { echo "package-macos.sh: macOS only" >&2; exit 1; }

BUILD=1 SIGN=1 NOTARIZE=1 DMG=1 OUT=""
COMMUNITY=0 X86_64="" APP_STORE=0 PROVISION="" INSTALLER_IDENTITY=""
IDENTITY="" PROFILE="2ksbox-notary"
ARGS=("$@")
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0; shift ;;
    --no-sign) SIGN=0; NOTARIZE=0; shift ;;
    --no-notarize) NOTARIZE=0; shift ;;
    --no-dmg) DMG=0; shift ;;
    --identity) IDENTITY=$2; shift 2 ;;
    --keychain-profile) PROFILE=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --community) COMMUNITY=1; shift ;;
    --x86_64) X86_64=1; shift ;;
    --app-store) APP_STORE=1; shift ;;
    --provision) PROVISION=$2; shift 2 ;;
    --installer-identity) INSTALLER_IDENTITY=$2; shift 2 ;;
    -h|--help) sed -n '2,77p' "$0"; exit 0 ;;
    *) echo "package-macos.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done
# The Intel app on an Apple Silicon Mac: the same re-run under Rosetta as
# scripts/build.sh --x86_64, and the same recognition of it. From there on
# `uname -m` says x86_64, so the DMG's name and the architecture check
# below come out Intel's without being told, and on an Intel Mac itself
# nothing is translated and the same lines make its native app.
ROSETTA=""
[ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ] && ROSETTA=1
if [ -n "$X86_64" ] && [ -z "$ROSETTA" ]; then
  exec arch -x86_64 "$0" "${ARGS[@]}"
fi
ARCH=$(uname -m)
# This build's inputs: the native build's directories, or the Intel
# build's beside them (scripts/build.sh --x86_64).
QB=build/qemu TD=player-mitsuami/target/release LTD=launcher-mitsuami/target/release D3DPT=build/d3dpt DXVK=build/dxvk CT=()
if [ -n "$ROSETTA" ]; then
  QB=build/x86_64/qemu TD=player-mitsuami/target/x86_64-apple-darwin/release
  LTD=launcher-mitsuami/target/x86_64-apple-darwin/release D3DPT=build/x86_64/d3dpt DXVK=build/x86_64/dxvk
  CT=(--target x86_64-apple-darwin)
  OUT="${OUT:-$ROOT/build/macos-x86_64}"
fi
if [ "$APP_STORE" = 1 ]; then OUT="${OUT:-$ROOT/build/macos-app-store}"; fi
OUT="${OUT:-$ROOT/build/macos}"
# The Intel app is the community build and nothing else (ADR-019): the
# App Store build is Apple Silicon only.
if [ "$ARCH" = x86_64 ]; then COMMUNITY=1; fi
# The App Store build goes up as a signed installer package, so there is
# nothing to notarize and no disk image (docs/build-macos.md, "The App
# Store package").
if [ "$APP_STORE" = 1 ]; then
  [ "$COMMUNITY" = 0 ] || { echo "package-macos.sh: --app-store is the App Store build: Apple Silicon, no --community" >&2; exit 2; }
  [ "$SIGN" = 1 ] || { echo "package-macos.sh: --app-store signs; stage unsigned without it" >&2; exit 2; }
  [ -n "$PROVISION" ] || { echo "package-macos.sh: --app-store needs --provision <Mac App Store profile for com.2ksbox.2ksbox>" >&2; exit 2; }
  [ -f "$PROVISION" ] || { echo "package-macos.sh: no provisioning profile at $PROVISION" >&2; exit 2; }
  # App Store Connect refuses an icon set without 512@2x (ITMS-90236), and
  # gen-icons.sh makes the 1024 only from a master that large.
  [ -f packaging/icon/macos/2ksbox-1024.png ] || { echo "package-macos.sh: --app-store needs packaging/icon/macos/2ksbox-1024.png: put a master of 1024 px or more at packaging/icon/2ksbox.png and run scripts/gen-icons.sh" >&2; exit 2; }
  case "$PROVISION" in /*) ;; *) PROVISION="$PWD/$PROVISION" ;; esac
  NOTARIZE=0 DMG=0
fi
# Absolute, whatever was typed: the checks below `cd /` before they run the
# staged binaries, and a relative --out broke there.
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
echo "arch           $ARCH${ROSETTA:+ (under Rosetta: the Intel app, from $QB and $TD)}$([ "$COMMUNITY" = 1 ] && echo ", the community build")"

VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)
APP="$OUT/2ksbox.app"
C="$APP/Contents"

need() { [ -e "$1" ] || { echo "package-macos.sh: missing $1${2:+ ($2)}" >&2; exit 1; }; }
warn() { echo "package-macos.sh: $*" >&2; }
need "$QB/libqemu-embed-i386.dylib" "scripts/configure-qemu.sh && ninja -C $QB libqemu-embed-i386.dylib"
need "$QB/qemu-img" "ninja -C $QB qemu-img"
need qemu/pc-bios "scripts/prepare-qemu.sh"
# Windows 11 on Arm (track M20), on an Apple Silicon Mac only: the Intel
# app has no Windows 11 at all (player::mac_x64_refusal). Its own player
# links its own QEMU (doc 11), boots our EDK2 build (bundle::Arch::
# efi_code_file; QEMU's own has no Secure Boot and no AHCI) and gets the
# ARM64 drivers disc (disc_library::drivers_iso), which setup installs the
# network and display drivers from.
W11=""
if [ "$ARCH" = arm64 ]; then
  W11=1
  need "$QB/libqemu-embed-aarch64.dylib" "scripts/build.sh qemu"
  need qemu/pc-bios/2ksbox-aarch64-code.fd "scripts/build.sh edk2"
  need qemu/pc-bios/2ksbox-aarch64-vars.fd "scripts/build.sh edk2"
  need build/virtio-win/2ksbox-drivers-arm64.iso "scripts/build.sh virtio"
fi

# The macOS the app is for (scripts/macos-floor.sh), which scripts/build.sh
# built everything for. Exported so that a cargo build below links for it
# too.
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(scripts/macos-floor.sh)}"
case "$MACOSX_DEPLOYMENT_TARGET" in *.*) ;; *) MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET.0" ;; esac
export MACOSX_DEPLOYMENT_TARGET
FLOOR=$MACOSX_DEPLOYMENT_TARGET

if [ "$BUILD" = 1 ]; then
  # The launcher and the player are each their own cargo workspace
  # (ADR-023, ADR-025), so their own build commands, as scripts/build.sh's
  # `mitsuami` stage. The player is player-mitsuami (track M22).
  ( cd launcher-mitsuami && cargo build --release ${CT[@]+"${CT[@]}"} )
  ( cd player-mitsuami && cargo build --release ${CT[@]+"${CT[@]}"} )
  [ -z "$W11" ] || ( cd player-mitsuami && cargo build --release --features qemu-aarch64 --target-dir target/qemu-aarch64 )
fi
need "$LTD/launcher-mitsuami" "scripts/build.sh mitsuami"
need "$TD/player-mitsuami" "scripts/build.sh mitsuami"
[ -z "$W11" ] || need player-mitsuami/target/qemu-aarch64/release/player-mitsuami "scripts/build.sh mitsuami"

# --- stage -----------------------------------------------------------
rm -rf "$APP"
mkdir -p "$C"/{MacOS,Resources,lib/2ksbox,libexec/2ksbox,share/2ksbox,share/doc/2ksbox}

install -m755 "$LTD/launcher-mitsuami" "$C/MacOS/2ksbox"
install -m755 "$TD/player-mitsuami" "$C/MacOS/2ksbox-player"
install -m755 "$QB/libqemu-embed-i386.dylib" "$C/lib/2ksbox/"
install -m755 "$QB/qemu-img" "$C/libexec/2ksbox/"
# pc-bios/ carries our EDK2 pair too: build-edk2.sh installs it there.
cp -a qemu/pc-bios "$C/share/2ksbox/pc-bios"
if [ -n "$W11" ]; then
  install -m755 player-mitsuami/target/qemu-aarch64/release/player-mitsuami "$C/MacOS/2ksbox-player-aarch64"
  install -m755 "$QB/libqemu-embed-aarch64.dylib" "$C/lib/2ksbox/"
  # 2.7 MB; virtio-win's license is on the disc beside its drivers
  # (BSD-3-Clause, binaries included) and in THIRD-PARTY-NOTICES.md.
  mkdir -p "$C/share/2ksbox/drivers"
  install -m644 build/virtio-win/2ksbox-drivers-arm64.iso "$C/share/2ksbox/drivers/"
fi
install -m644 COPYING THIRD-PARTY-NOTICES.md README.md "$C/share/doc/2ksbox/"

# The General MIDI bank (doc 20 §4): the machine form's default music
# port plays through it, and the packaged player names it to QEMU.
mkdir -p "$C/share/2ksbox/soundfonts"
install -m644 soundfonts/TimGM6mb.sf2 "$C/share/2ksbox/soundfonts/"

iso=$(ls -t guest-tools/out/guest-tools-*.iso 2>/dev/null | head -1 || true)
if [ -n "$iso" ]; then
  mkdir -p "$C/share/2ksbox/guest-tools" && install -m644 "$iso" "$C/share/2ksbox/guest-tools/"
else
  warn "no guest-tools ISO in guest-tools/out (guest-tools/build-wrappers.sh); packaging without it"
fi

# The three optional companions. QEMU dlopens each of them by a search
# that begins in a checkout's build/ directory, so the packaged player
# names them through the environment instead (player-core/src/companions.rs);
# all this has to do is put them where that expects.

D3D=1
if [ "$ARCH" = x86_64 ]; then
  # No Vulkan driver exists for an Intel Mac (ADR-019: KosmicKrisp is
  # arm64 only, MoltenVK refused), so the in-process executor, DXVK and
  # the loader stay out; the pair below is the Intel app's Direct3D.
  echo "direct3d       the executor on Wine only (no Vulkan driver exists for an Intel Mac)"
  D3D=0
elif [ -f "$D3DPT/libd3dpt_exec.dylib" ] && [ -f "$DXVK/src/d3d9/libdxvk_d3d9.0.dylib" ]; then
  install -m755 "$D3DPT/libd3dpt_exec.dylib" "$C/lib/2ksbox/"
  install -m755 "$DXVK/src/d3d9/libdxvk_d3d9.0.dylib" "$C/lib/2ksbox/"
else
  warn "no Direct3D executor (scripts/build-d3dpt-exec.sh); XP Direct3D will fall back"
  D3D=0
fi
# The community build only (ADR-019): the same executor in another
# process, on a Wine the user has (M15, ADR-018). That is the library QEMU
# opens when DXVK finds no Vulkan device, plus the Windows build of the
# executor with its host program, PE files the Mach-O closure below never
# touches. The App Store build is macOS 26+ with KosmicKrisp and never
# starts Wine, so it carries none of this.
if [ "$COMMUNITY" = 1 ]; then
  if [ -f "$D3DPT/libd3dpt_exec_remote.dylib" ] && [ -f "$D3DPT/wine/d3dpt_exec.dll" ] && [ -f "$D3DPT/wine/d3dpt-exec-host.exe" ]; then
    install -m755 "$D3DPT/libd3dpt_exec_remote.dylib" "$C/lib/2ksbox/"
    mkdir -p "$C/lib/2ksbox/wine"
    install -m644 "$D3DPT/wine/d3dpt_exec.dll" "$D3DPT/wine/d3dpt-exec-host.exe" "$C/lib/2ksbox/wine/"
  elif [ "$ARCH" = x86_64 ]; then
    echo "package-macos.sh: no executor for Wine in $D3DPT (scripts/build.sh --x86_64 exec, mingw-w64), and it is the Intel app's only Direct3D" >&2
    exit 1
  else
    warn "no executor for Wine (scripts/build-d3dpt-exec.sh --wine, mingw-w64); a Mac below Vulkan 1.3 gets no Direct3D"
  fi
fi

# Vulkan: stock macOS has none, so the executor's driver travels with us.
# The LunarG SDK's own loader and the KosmicKrisp ICD (docs/build-macos.md,
# patches/dxvk/README.md), the pair the D3D9 harnesses pass on.
if [ "$D3D" = 1 ]; then
  sdk=$(ls -d "$HOME"/VulkanSDK/*/macOS 2>/dev/null | sort -V | tail -1 || true)
  if [ -n "$sdk" ] && [ -f "$sdk/lib/libvulkan_kosmickrisp.dylib" ]; then
    # Follow the symlink: the bundle carries files, not links into $HOME.
    cp -L "$sdk/lib/libvulkan.1.dylib" "$C/lib/2ksbox/libvulkan.1.dylib"
    install -m755 "$sdk/lib/libvulkan_kosmickrisp.dylib" "$C/lib/2ksbox/"
    chmod 755 "$C/lib/2ksbox/libvulkan.1.dylib"
    # Our own ICD manifest: the SDK's points at its own tree, and
    # library_path is resolved relative to the manifest.
    mkdir -p "$C/share/2ksbox/vulkan/icd.d"
    cat > "$C/share/2ksbox/vulkan/icd.d/driver.json" <<JSON
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../../lib/2ksbox/libvulkan_kosmickrisp.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON
    echo "vulkan         $(basename "$(dirname "$sdk")") KosmicKrisp"
  else
    warn "no ~/VulkanSDK/*/macOS/lib/libvulkan_kosmickrisp.dylib; the app will carry no Vulkan driver and Direct3D will be off on a machine without one"
  fi
fi

# --- the dylib closure ------------------------------------------------
# Everything outside /usr/lib and /System (Homebrew's glib/pixman/zstd...)
# copied into lib/2ksbox and rewritten to @rpath, transitively. XQuartz's
# libGL and X11 libraries are no longer linked (patch 70).
LIBDIR="$C/lib/2ksbox"
# A library's own install name is the first line `otool -L` prints and no
# dependency: a Homebrew-built library keeps Homebrew's, which loads nothing.
# A universal library (the LunarG loader) answers `otool -D` once per
# architecture, under a header line each; one id is wanted, not a
# two-line string awk warns about.
external() {
  local id
  id=$(otool -D "$1" | grep -v ':$' | sort -u | head -1)
  otool -L "$1" | tail -n +2 | awk -v id="$id" '$1 != id {print $1}' | grep -E '^(/opt/|/usr/local/)' || true
}

# Every Mach-O in the bundle, which is not the same as every executable
# file in it. A dylib can be mode 644 and still carry a signature that a
# rewritten load command invalidates, and on arm64 a broken signature is
# SIGKILL, not a warning. (The Qt launcher's frameworks, mode 644 with no
# extension, died of "Code Signature Invalid" that way.)
machos() {
  find "$C" -type f \( -perm +111 -o -name '*.dylib' \) \
    -exec sh -c 'file -b "$1" | grep -q Mach-O && echo "$1"' _ {} \;
}

bundle_deps() {
  local file=$1 dep leaf real
  for dep in $(external "$file"); do
    real=$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$dep")
    leaf=$(basename "$dep")
    if [ ! -f "$LIBDIR/$leaf" ]; then
      [ -f "$real" ] || { warn "cannot find $dep, needed by $(basename "$file")"; continue; }
      install -m755 "$real" "$LIBDIR/$leaf"
      install_name_tool -id "@rpath/$leaf" "$LIBDIR/$leaf" 2>/dev/null || true
      # Each bundled library must find its own siblings in this same
      # directory, whatever loaded it.
      install_name_tool -add_rpath "@loader_path" "$LIBDIR/$leaf" 2>/dev/null || true
      bundle_deps "$LIBDIR/$leaf"
    fi
    install_name_tool -change "$dep" "@rpath/$leaf" "$file" 2>/dev/null || true
  done
}

for f in "$LIBDIR"/*.dylib "$C/libexec/2ksbox/qemu-img" "$C/MacOS/2ksbox" "$C/MacOS/2ksbox-player" "$C/MacOS/2ksbox-player-aarch64"; do
  [ -f "$f" ] || continue
  bundle_deps "$f"
done
# Our own libraries kept an absolute or build-tree id; @rpath is what the
# things loading them ask for.
for leaf in libqemu-embed-i386.dylib libqemu-embed-aarch64.dylib libd3dpt_exec.dylib libd3dpt_exec_remote.dylib libdxvk_d3d9.0.dylib libvulkan.1.dylib libvulkan_kosmickrisp.dylib; do
  [ -f "$LIBDIR/$leaf" ] || continue
  install_name_tool -id "@rpath/$leaf" "$LIBDIR/$leaf" 2>/dev/null || true
  install_name_tool -add_rpath "@loader_path" "$LIBDIR/$leaf" 2>/dev/null || true
done

# And the executables. The player already has @loader_path/../lib/2ksbox
# from player-mitsuami/build.rs.
for p in "$C/MacOS/2ksbox-player" "$C/MacOS/2ksbox-player-aarch64"; do
  [ -f "$p" ] && install_name_tool -add_rpath "@loader_path/../lib/2ksbox" "$p" 2>/dev/null || true
done
install_name_tool -add_rpath "@loader_path/../../lib/2ksbox" "$C/libexec/2ksbox/qemu-img" 2>/dev/null || true
# Every rpath that points out of the app has to go, from libraries as
# well as executables. meson gives libqemu-embed one LC_RPATH per Homebrew
# prefix it linked against, searched before the @loader_path added above.
# A bundle that keeps them loads this machine's Homebrew instead of its
# own copies, passing every check here and failing on the first machine
# with no Homebrew.
while read -r f; do
  while read -r rp; do
    case "$rp" in "$ROOT"/*|/opt/*|/usr/local/*)
      install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true ;;
    esac
  done < <(otool -l "$f" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}')
done < <(machos)

# Rewriting a load command breaks the signature every arm64 binary must
# have, and the kernel answers a broken one with SIGKILL and nothing else,
# which the checks below would run into. So re-sign ad hoc now.
# The real Developer ID signature replaces this further down; here it only
# has to make the staged app runnable. Windows 11 on Arm's player keeps
# its entitlements even ad hoc: without the hypervisor one HVF answers
# HV_DENIED, signed or not (packaging/macos/hypervisor.entitlements).
entitlements_of() {
  case "$1" in
    */MacOS/2ksbox-player-aarch64) echo packaging/macos/hypervisor.entitlements ;;
    *) echo packaging/macos/2ksbox.entitlements ;;
  esac
}
while read -r f; do
  case "$f" in
    */MacOS/2ksbox-player-aarch64) codesign --force --sign - --entitlements "$(entitlements_of "$f")" "$f" >/dev/null 2>&1 || true ;;
    *) codesign --force --sign - "$f" >/dev/null 2>&1 || true ;;
  esac
done < <(machos)

# --- icon -------------------------------------------------------------
# `scripts/gen-icons.sh`'s macOS set (`packaging/icon/macos/`): the one
# master on Apple's icon grid, so the three platforms draw one icon.
# Nothing is rasterized here. An .icns is a container, and iconutil accepts a
# partial set; 512@2x is there when the master made a 1024 (gen-icons.sh),
# which the App Store build requires (checked at the top).
set=$(mktemp -d)/2ksbox.iconset; mkdir -p "$set"
for s in 16 32 64 128 256 512 1024; do
  [ -f "packaging/icon/macos/2ksbox-$s.png" ] || continue
  cp "packaging/icon/macos/2ksbox-$s.png" "$set/icon_${s}x${s}.png"
done
# The @2x names Apple wants are the next size up under the previous name.
for s in 16 32 128 256 512; do
  if [ -f "$set/icon_$((s*2))x$((s*2)).png" ]; then cp "$set/icon_$((s*2))x$((s*2)).png" "$set/icon_${s}x${s}@2x.png"; fi
done
rm -f "$set/icon_1024x1024.png"
rm -f "$set/icon_64x64.png"
iconutil -c icns "$set" -o "$C/Resources/2ksbox.icns"

# --- Info.plist -------------------------------------------------------
# What the bundle's own Mach-O files require, measured rather than
# claimed; the check below fails when that is above the floor.
minos_of() { otool -l "$1" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}'; }
minos=$(machos | while read -r f; do minos_of "$f"; done | sort -V | tail -1)
minos=${minos:-$FLOOR}
# The bundle ID is the App ID registered with Apple, one per build
# (ADR-011), so the two apps are distinct to macOS and install side by
# side. Neither may ever change once shipped.
BUNDLE_ID=com.2ksbox.2ksbox
[ "$COMMUNITY" = 1 ] && BUNDLE_ID=com.2ksbox.2ksbox-community
write_plist() { sed -e "s/@VERSION@/$VERSION/" -e "s/@MINOS@/$1/" \
  -e "s/@BUNDLE_ID@/$BUNDLE_ID/" packaging/macos/Info.plist.in > "$C/Info.plist"; }
# The App Store build never gets a pre-26 version (ADR-019), whatever
# its files would run on.
plist_min=$minos
if [ "$APP_STORE" = 1 ]; then
  plist_min=$(printf '%s\n' "$minos" 26.0 | sort -V | tail -1)
fi
write_plist "$plist_min"
echo "minimum macOS $plist_min (the files: $minos; the floor: $FLOOR), bundle ID $BUNDLE_ID"

# --- the check --------------------------------------------------------
# The same question package-linux.sh asks, in the form a Mac can answer:
# does anything in here still reach outside the bundle? `env -i` from /,
# so no LAUNCHER_*/PLAYER_*/DYLD_* of this shell is what makes it work.
fail=0

# The promise the plist makes: nothing in here needs a newer macOS than the
# floor. A file that does is ours built for another target (scripts/build.sh
# again) or a library macos-bottles.py found no older build of.
above=$(machos | while read -r f; do
  m=$(minos_of "$f"); [ -n "$m" ] || continue
  [ "$(printf '%s\n' "$m" "$FLOOR" | sort -V | tail -1)" = "$FLOOR" ] || echo "  macOS $m  ${f#"$C/"}"
done)
if [ -n "$above" ]; then
  printf '%s\n' "$above" >&2
  echo "package-macos.sh: the above need a newer macOS than the floor, $FLOOR" >&2
  fail=1
fi

# And the architecture: every Mach-O carries this build's, which is the
# Mac's under Rosetta as well. A file that does not was taken from the
# other build (an arm64 KosmicKrisp in the Intel app, say), and the
# loader on the other Mac answers that with "no suitable image found".
wrong=$(machos | while read -r f; do
  archs=$(lipo -archs "$f" 2>/dev/null || true)
  case " $archs " in *" $ARCH "*) ;; *) echo "  ${archs:-?}  ${f#"$C/"}" ;; esac
done)
if [ -n "$wrong" ]; then
  printf '%s\n' "$wrong" >&2
  echo "package-macos.sh: the above are not $ARCH" >&2
  fail=1
fi

while read -r f; do
  out=$(external "$f")
  [ -z "$out" ] || { echo "package-macos.sh: $f still links $(echo "$out" | tr '\n' ' ')" >&2; fail=1; }
done < <(machos)

# And every @rpath dependency must resolve through the binary's own
# rpaths, inside the bundle. A file that fails this cannot load on any
# machine, but on *this* one it is invisible, because the Homebrew copy it
# was built against is still on disk. This catches what the prune above
# missed and a half-rewritten install name.
# dyld expands @rpath with the rpaths of every image in the chain that led
# to the load, not only the one holding the reference, so a library
# dlopened by an executable also searches the executable's rpaths. The
# check has to do the same, or it fails files that work.
exe_rpaths=""
for x in "$C/MacOS/2ksbox" "$C/MacOS/2ksbox-player" "$C/MacOS/2ksbox-player-aarch64"; do
  [ -f "$x" ] || continue
  while read -r rp; do
    rp=${rp//@loader_path/$C\/MacOS}; rp=${rp//@executable_path/$C\/MacOS}
    exe_rpaths="$exe_rpaths $rp"
  done < <(otool -l "$x" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}')
done

unresolved() {
  local f=$1 id rps dep rp cand ok
  id=$(otool -D "$f" | tail -n +2)
  rps=$(otool -l "$f" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}')
  # `|| true`: a binary with no @rpath dependency at all is the normal
  # case, and grep's empty-handed 1 would end the script under pipefail.
  otool -L "$f" | tail -n +2 | awk '{print $1}' | { grep '^@rpath/' || true; } | sort -u \
    | while read -r dep; do
        if [ "$dep" != "$id" ]; then      # its own install name is no dependency
          ok=0
          for rp in $rps $exe_rpaths; do
            cand=${rp//@loader_path/$(dirname "$f")}
            cand=${cand//@executable_path/$C\/MacOS}
            if [ -e "$cand/${dep#@rpath/}" ]; then ok=1; break; fi
          done
          [ "$ok" = 1 ] || echo "${dep#@rpath/}"
        fi
      done
}
while read -r f; do
  miss=$(unresolved "$f" | tr '\n' ' ')
  [ -z "$miss" ] || { echo "package-macos.sh: $f needs ${miss% }, which its rpaths do not reach" >&2; fail=1; }
done < <(machos)

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
resolved=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" "$C/MacOS/2ksbox" --paths)
echo "$resolved"
while read -r what path; do
  case "$what" in player|player-aarch64|qemu-img|pc-bios|guest-tools|drivers|prefix) ;; *) continue ;; esac
  case "$path" in "("*) continue ;; esac
  case "$path" in "$APP"|"$APP"/*) ;; *) echo "package-macos.sh: $what resolved outside the app: $path" >&2; fail=1 ;; esac
done <<< "$resolved"
if [ -n "$W11" ]; then
  # Present, not only inside: a launcher that names no aarch64 player or
  # no drivers disc passes the loop above.
  for want in "player-aarch64 $C/MacOS/2ksbox-player-aarch64" "drivers $C/share/2ksbox/drivers/2ksbox-drivers-arm64.iso"; do
    grep -qxF "${want%% *} ${want#* }" <<< "$(printf '%s\n' "$resolved" | tr -s ' ')" \
      || { echo "package-macos.sh: --paths does not name ${want#* }" >&2; fail=1; }
  done
fi

# The launcher's own Vulkan, which the closure above never loaded. The
# probe behind the Direct3D picker opens the app's loader by its full path
# and names the app's driver to it (launcher-core/src/host_gpu.rs). A
# leaf-name dlopen found nothing in an app that ships only
# `libvulkan.1.dylib`, so the community app on macOS 15 said "Vulkan
# loader: not present" beside the copy its executor was running on. So
# ask the staged launcher, from `/` with an empty environment, and require
# its report to name the app's loader, the loaded file to be the app's,
# and nothing to load from outside the app. The verdict itself is this
# Mac's (a Mac below 26 has a loader and no GPU behind it), so it is
# printed, not required.
if [ -f "$C/lib/2ksbox/libvulkan.1.dylib" ]; then
  hc=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
    DYLD_PRINT_LIBRARIES=1 "$C/MacOS/2ksbox" --host-check 2>&1 || true)
  report=$(printf '%s\n' "$hc" | grep -v '^dyld\[' || true)
  case "$report" in
    *"Vulkan loader: "*"(the app's own)"*) ;;
    *) printf '%s\n' "$report" | sed 's/^/  /' >&2
       echo "package-macos.sh: the staged launcher did not probe on the app's own Vulkan loader" >&2; fail=1 ;;
  esac
  vkloaded=$(printf '%s\n' "$hc" | sed -n 's|^dyld\[[0-9]*\]: <[^>]*> ||p')
  case "$vkloaded" in
    *"$C/lib/2ksbox/libvulkan.1.dylib"*) ;;
    *) echo "package-macos.sh: the staged launcher's --host-check never loaded the app's libvulkan" >&2; fail=1 ;;
  esac
  outside=$(printf '%s\n' "$vkloaded" | grep -v -e "^$APP/" -e '^/usr/lib/' -e '^/System/' || true)
  if [ -n "$outside" ]; then
    printf '%s\n' "$outside" | sed 's/^/  /' >&2
    echo "package-macos.sh: the staged launcher's --host-check loaded the above from outside the app" >&2
    fail=1
  fi
  echo "host-check     $(printf '%s\n' "$report" | sed -n 's/^Direct3D pass-through: //p' | head -1)"
fi

# What the *loader* did, which is what another Mac will test. No image
# outside the app and the system may load. An installed app has no
# build/qemu, so the @loader_path rpath (player-mitsuami/build.rs) is all there is
# to find libqemu-embed with, and every Homebrew library reached from here
# is one this machine has and another may not. `--mode-sweep` is the
# display path end to end without a guest, enough to pull the embed
# library in.
loaded=$(cd / && env -i DYLD_PRINT_LIBRARIES=1 \
  "$C/MacOS/2ksbox-player" --mode-sweep "$scratch/sweep" 2>&1 \
  | sed -n 's|^dyld\[[0-9]*\]: <[^>]*> ||p')
case "$loaded" in
  *libqemu-embed-i386.dylib*) ;;
  *) echo "package-macos.sh: the packaged player never loaded libqemu-embed" >&2; fail=1 ;;
esac
outside=$(printf '%s\n' "$loaded" | grep -v -e "^$APP/" -e '^/usr/lib/' -e '^/System/' || true)
if [ -n "$outside" ]; then
  printf '%s\n' "$outside" | sed 's/^/  /' >&2
  echo "package-macos.sh: the packaged player loaded the above from outside the app" >&2
  fail=1
else
  echo "loader         $(printf '%s\n' "$loaded" | grep -c "^$APP/") images from the app, the rest from the system"
fi

# The window, which `--paths` never opens. The launcher's own headless
# grab (`LAUNCHER_SHOT`, launcher-mitsuami/src/shot.rs) opens the machine
# window, draws it into a PNG and exits, and the loader is asked the same
# question while it runs: AppKit and the system's frameworks, our code,
# and nothing from this Mac's Homebrew. AppKit has no offscreen mode, so
# the window shows for a moment on the packager's screen.
shot="$scratch/window.png"
# `|| true`: a launcher that dies exits non-zero, and under pipefail that
# would end the script here, silently, instead of in the verdict below
# with the launcher's own words.
winout=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
  LAUNCHER_SHOT="$shot" DYLD_PRINT_LIBRARIES=1 "$C/MacOS/2ksbox" 2>&1 || true)
winloaded=$(printf '%s\n' "$winout" | sed -n 's|^dyld\[[0-9]*\]: <[^>]*> ||p')
if [ -s "$shot" ]; then
  echo "window         $(du -h "$shot" | cut -f1) grabbed by the staged launcher"
else
  printf '%s\n' "$winout" | grep -v '^dyld\[' | sed 's/^/  /' >&2
  echo "package-macos.sh: the staged launcher drew no window (LAUNCHER_SHOT)" >&2
  fail=1
fi
outside=$(printf '%s\n' "$winloaded" | grep -v -e "^$APP/" -e '^/usr/lib/' -e '^/System/' || true)
if [ -n "$outside" ]; then
  printf '%s\n' "$outside" | sed 's/^/  /' >&2
  echo "package-macos.sh: the staged launcher loaded the above from outside the app" >&2
  fail=1
fi

# The bundle-creating path end to end, as package-linux.sh does it.
(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
  "$C/MacOS/2ksbox" --wizard-new xp "Package check" 1 >/dev/null)
args=$(cd / && env -i HOME="$scratch" "$C/MacOS/2ksbox" --print-args "$scratch/machines/package-check/machine.toml")
case "$args" in
  *"-L $C/share/2ksbox/pc-bios"*) echo "qemu args      -L inside the app" ;;
  *) echo "package-macos.sh: --print-args did not point at the packaged firmware" >&2; fail=1 ;;
esac
[ -s "$scratch/machines/package-check/disk.qcow2" ] \
  || { echo "package-macos.sh: the packaged qemu-img did not create a disk" >&2; fail=1; }

# Windows 11 on Arm end to end, which nothing above loads: the staged
# launcher makes the machine and its firmware variables (the packaged
# qemu-img), its arguments must name the app's EDK2 and drivers disc, and
# the staged aarch64 player runs them under HVF until the firmware draws
# a frame, then quits through QMP. That is the hypervisor entitlement
# surviving the re-sign, the libtpms TPM, our EDK2 and the ARM64 QEMU's
# closure. The window shows for a moment.
if [ -n "$W11" ]; then
  (cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
    "$C/MacOS/2ksbox" --wizard-new win11 "Arm check" 1 >/dev/null)
  m="$scratch/machines/arm-check/machine.toml"
  (cd / && env -i HOME="$scratch" "$C/MacOS/2ksbox" --prepare "$m") \
    || { echo "package-macos.sh: --prepare failed on a Windows 11 on Arm machine" >&2; fail=1; }
  w11args=$(cd / && env -i HOME="$scratch" "$C/MacOS/2ksbox" --print-args "$m")
  for want in "file=$C/share/2ksbox/pc-bios/2ksbox-aarch64-code.fd" \
              "file=$C/share/2ksbox/drivers/2ksbox-drivers-arm64.iso" "-accel hvf"; do
    case "$w11args" in *"$want"*) ;; *) echo "package-macos.sh: Windows 11 on Arm's arguments lack $want" >&2; fail=1 ;; esac
  done
  # One QEMU at a time on this Mac: the run ends by itself through QMP,
  # or here after a minute.
  read -r -a argv <<< "$w11args"
  (cd / && env -i HOME="$scratch" PLAYER_QMP_EXEC='{"execute":"quit"}' DYLD_PRINT_LIBRARIES=1 \
    "$C/MacOS/2ksbox-player-aarch64" -- "${argv[@]}" > "$scratch/player-aarch64.txt" 2>&1) &
  pid=$!
  ( sleep 60; kill "$pid" 2>/dev/null ) & killer=$!
  if wait "$pid"; then
    kill "$killer" 2>/dev/null || true
    a64loaded=$(sed -n 's|^dyld\[[0-9]*\]: <[^>]*> ||p' "$scratch/player-aarch64.txt")
    outside=$(printf '%s\n' "$a64loaded" | grep -v -e "^$APP/" -e '^/usr/lib/' -e '^/System/' || true)
    if [ -n "$outside" ]; then
      printf '%s\n' "$outside" | sed 's/^/  /' >&2
      echo "package-macos.sh: the aarch64 player loaded the above from outside the app" >&2
      fail=1
    elif grep -q 'failed to initialize hvf' "$scratch/player-aarch64.txt"; then
      # HV_DENIED is the entitlement lost in the re-sign; a Mac in a VM
      # has no HVF at all and cannot make this app.
      grep -e hvf -e 'falling back' "$scratch/player-aarch64.txt" | sed 's/^/  /' >&2
      echo "package-macos.sh: the staged aarch64 player could not use HVF" >&2
      fail=1
    else
      echo "player-aarch64 ran Windows 11 on Arm's firmware under HVF and quit (libqemu-embed-aarch64, our EDK2, the TPM)"
    fi
  else
    kill "$killer" 2>/dev/null || true
    grep -v '^dyld\[' "$scratch/player-aarch64.txt" | tail -8 | sed 's/^/  /' >&2
    echo "package-macos.sh: the staged aarch64 player did not run Windows 11 on Arm's firmware and quit" >&2
    fail=1
  fi
fi
[ "$fail" = 0 ] || exit 1
echo "checks passed"

# --- sign -------------------------------------------------------------
# The App Store build's identity and entitlements come from its
# provisioning profile: the team in it, and the App ID it was made for,
# which must be this bundle's. A development profile (one that lists
# devices) makes a build the store refuses.
if [ "$APP_STORE" = 1 ]; then
  prof=$(mktemp); security cms -D -i "$PROVISION" > "$prof" 2>/dev/null \
    || { echo "package-macos.sh: $PROVISION is not a provisioning profile" >&2; exit 1; }
  pb() { /usr/libexec/PlistBuddy -c "Print :$1" "$prof" 2>/dev/null; }
  TEAM=$(pb TeamIdentifier:0)
  APP_ID=$(pb Entitlements:com.apple.application-identifier)
  [ "$APP_ID" = "$TEAM.$BUNDLE_ID" ] \
    || { echo "package-macos.sh: $PROVISION is for $APP_ID, not $TEAM.$BUNDLE_ID" >&2; exit 1; }
  ! pb ProvisionedDevices >/dev/null \
    || { echo "package-macos.sh: $PROVISION lists devices: a development profile, not a Mac App Store one" >&2; exit 1; }
  echo "profile        $(pb Name) ($APP_ID, expires $(pb ExpirationDate))"
  cp "$PROVISION" "$C/embedded.provisionprofile"
  launcher_ents=$(mktemp)
  sed -e "s/@APP_ID@/$APP_ID/" -e "s/@TEAM@/$TEAM/" packaging/macos/app-store.entitlements > "$launcher_ents"
  # Pick the team's own, when the keychain holds more than one team's.
  pick() { security find-identity -v -p "$1" | sed -n "s/.*\"\($2: .*($TEAM)\)\"/\1/p" | head -1; }
  [ -n "$IDENTITY" ] || IDENTITY=$(pick codesigning 'Apple Distribution')
  [ -n "$IDENTITY" ] || IDENTITY=$(pick codesigning '3rd Party Mac Developer Application')
  [ -n "$IDENTITY" ] || { echo "package-macos.sh: no Apple Distribution identity for team $TEAM; --identity" >&2; exit 1; }
  [ -n "$INSTALLER_IDENTITY" ] || INSTALLER_IDENTITY=$(pick basic 'Mac Installer Distribution')
  [ -n "$INSTALLER_IDENTITY" ] || INSTALLER_IDENTITY=$(pick basic '3rd Party Mac Developer Installer')
  [ -n "$INSTALLER_IDENTITY" ] || { echo "package-macos.sh: no Mac Installer Distribution identity for team $TEAM; --installer-identity" >&2; exit 1; }
fi
# What each Mach-O is signed with. Every executable in an App Store app
# must be sandboxed: the launcher with its own sandbox, everything it
# starts inheriting that one. A library carries no entitlements.
signing_entitlements() {
  if [ "$APP_STORE" = 0 ]; then entitlements_of "$1"; return; fi
  case "$1" in
    "$C"/MacOS/2ksbox) echo "$launcher_ents" ;;
    */MacOS/2ksbox-player-aarch64) echo packaging/macos/app-store-hypervisor.entitlements ;;
    */MacOS/2ksbox-player) echo packaging/macos/app-store-player.entitlements ;;
    */libexec/2ksbox/qemu-img) echo packaging/macos/app-store-helper.entitlements ;;
    *) file -b "$1" | grep -q 'Mach-O.*executable' \
         && { echo "package-macos.sh: no App Store entitlements for the executable ${1#"$C/"}" >&2; return 1; }
       echo "" ;;
  esac
}

if [ "$SIGN" = 1 ]; then
  if [ -z "$IDENTITY" ]; then
    IDENTITY=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -1)
    [ -n "$IDENTITY" ] || { echo "package-macos.sh: no Developer ID Application identity; --identity or --no-sign" >&2; exit 1; }
  fi
  echo "signing as $IDENTITY"
  # --options runtime is the hardened runtime notarization requires, and
  # the entitlement beside it is what lets TCG keep its JIT under it.
  #
  # --timestamp is not optional either (notarization rejects a signature
  # without one). Apple's timestamp service drops out for a few seconds
  # often enough that a bundle with this many Mach-O files hits it ("The
  # timestamp service is not available"). Only that failure is retried.
  sign_one() {
    local i out ents
    ents=$(signing_entitlements "$1") || return 1
    for i in 1 2 3 4 5; do
      if out=$(codesign --force --timestamp --options runtime \
                 ${ents:+--entitlements "$ents"} \
                 --sign "$IDENTITY" "$1" 2>&1); then
        return 0
      fi
      case "$out" in
        *"timestamp service is not available"*|*"timestamp"*"unavailable"*)
          warn "timestamp service unavailable signing $(basename "$1"); retry $i"
          sleep 5 ;;
        *) printf '%s\n' "$out" >&2; return 1 ;;
      esac
    done
    printf '%s\n' "$out" >&2
    return 1
  }
  # Inside-out: every nested Mach-O before the thing that seals it.
  while read -r f; do
    case "$f" in
      "$C"/MacOS/2ksbox) continue ;;               # the app's own executable, last
    esac
    sign_one "$f"
  done < <(machos | sort -u)
  sign_one "$C/MacOS/2ksbox"
  sign_one "$APP"
  codesign --verify --deep --strict --verbose=2 "$APP"
  if [ -n "$W11" ]; then
    codesign -d --entitlements - "$C/MacOS/2ksbox-player-aarch64" 2>/dev/null | grep -q com.apple.security.hypervisor \
      || { echo "package-macos.sh: the signed aarch64 player lost the hypervisor entitlement" >&2; exit 1; }
  fi
  if [ "$APP_STORE" = 1 ]; then
    # What App Store validation rejects first: an executable outside the
    # sandbox, or a launcher whose App ID is not the profile's.
    while read -r f; do
      file -b "$f" | grep -q 'Mach-O.*executable' || continue
      codesign -d --entitlements - --xml "$f" 2>/dev/null | grep -q com.apple.security.app-sandbox \
        || { echo "package-macos.sh: ${f#"$C/"} is not sandboxed" >&2; exit 1; }
    done < <(machos)
    codesign -d --entitlements - --xml "$C/MacOS/2ksbox" 2>/dev/null | grep -q "$APP_ID" \
      || { echo "package-macos.sh: the signed launcher lacks the App ID $APP_ID" >&2; exit 1; }
    echo "sandboxed      every executable; the launcher is $APP_ID"
    # The two upload rejections the bundle itself can cause: an icon with
    # no 512@2x (ITMS-90236), and the export compliance answer.
    ics=$(mktemp -d)/check.iconset
    iconutil -c iconset "$C/Resources/2ksbox.icns" -o "$ics"
    [ -f "$ics/icon_512x512@2x.png" ] || { echo "package-macos.sh: the icon has no 512x512@2x" >&2; exit 1; }
    [ "$(plutil -extract ITSAppUsesNonExemptEncryption raw "$C/Info.plist" 2>/dev/null)" = false ] \
      || { echo "package-macos.sh: Info.plist lacks ITSAppUsesNonExemptEncryption=false" >&2; exit 1; }
    echo "store checks   512@2x icon; ITSAppUsesNonExemptEncryption false"
  else
    # The question Gatekeeper will ask on the other Mac. Before notarization
    # it answers "not notarized", which is the one remaining step, not a
    # failure. Any other rejection is.
    spctl --assess --type execute --verbose=4 "$APP" 2>&1 | sed 's/^/  /' || true
  fi
fi

# --- the App Store package ----------------------------------------------
# What App Store Connect takes: the app as an installer package for
# /Applications, signed by the installer identity. A build signed for the
# store does not launch outside it; TestFlight is where it runs first.
if [ "$APP_STORE" = 1 ]; then
  pkg="$OUT/2ksbox-$VERSION-macos-app-store.pkg"
  rm -f "$pkg"
  productbuild --component "$APP" /Applications --sign "$INSTALLER_IDENTITY" "$pkg"
  pkgutil --check-signature "$pkg" | sed 's/^/  /'
  du -h "$pkg" | sed 's/^/package /'
  echo "upload it with Transporter (or xcrun altool --upload-package); CFBundleVersion $VERSION must be new to App Store Connect"
fi

# --- notarize ---------------------------------------------------------
staple_check() { xcrun stapler validate "$1" >/dev/null && echo "stapled        $1"; }
if [ "$NOTARIZE" = 1 ]; then
  zip="$OUT/2ksbox-$VERSION.zip"
  rm -f "$zip"
  # ditto, not zip(1): the bundle's symlinks and signature must survive.
  /usr/bin/ditto -c -k --keepParent "$APP" "$zip"
  echo "submitting $(du -h "$zip" | cut -f1) to notarytool as profile '$PROFILE'"
  xcrun notarytool submit "$zip" --keychain-profile "$PROFILE" --wait
  xcrun stapler staple "$APP"
  staple_check "$APP"
  rm -f "$zip"
fi

# --- disk image -------------------------------------------------------
if [ "$DMG" = 1 ]; then
  dmgroot=$(mktemp -d)
  cp -a "$APP" "$dmgroot/"
  ln -s /Applications "$dmgroot/Applications"
  dmg="$OUT/2ksbox-$VERSION-macos-$(uname -m).dmg"
  rm -f "$dmg"
  # Sized by hand: hdiutil measures -srcfolder by the blocks its files
  # take, and our EDK2 pair is two 64 MiB files that are sparse on APFS
  # (3 MB on disk), which then do not fit ("No space left on device").
  # Apparent sizes plus a tenth; UDZO compresses the padding away.
  kb=$(du -sAk "$dmgroot" | cut -f1)
  hdiutil create -volname "2ksbox $VERSION" -srcfolder "$dmgroot" -size "$((kb + kb / 10 + 20480))k" \
    -ov -format UDZO -quiet "$dmg"
  rm -rf "$dmgroot"
  if [ "$SIGN" = 1 ]; then
    codesign --force --timestamp --sign "$IDENTITY" "$dmg"
  fi
  if [ "$NOTARIZE" = 1 ]; then
    xcrun notarytool submit "$dmg" --keychain-profile "$PROFILE" --wait
    xcrun stapler staple "$dmg"
    staple_check "$dmg"
  fi
  du -h "$dmg" | sed 's/^/image   /'
fi

du -sh "$APP" | sed 's/^/app     /'
echo "package: $APP"
