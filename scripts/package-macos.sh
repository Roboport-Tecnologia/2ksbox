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
# * `macdeployqt` puts Qt in the app. The launcher is `launcher-qt`
#   (ADR-015), so the bundle needs the Qt frameworks, the cocoa platform
#   plugin and the QtQuick QML module tree. Only the frameworks appear in
#   a load command, so the closure would never find the other two.
#   `macdeployqt` runs first and the closure runs over what it leaves. Our
#   QML is compiled into the binary as a Qt resource, so `-qmldir` points
#   `macdeployqt` at `launcher-qt/qml` to find the imports. Without it the
#   import scanner sees no QML and deploys no modules. The two passes
#   after it prune and rewire what it collects, because it deploys whole
#   plugin categories and QML module trees from Homebrew's shared Qt
#   prefix and leaves stale Homebrew rpaths behind.
# * The bundle is the prefix. `Contents` has the `lib/libexec/share`
#   shape of a Unix prefix, and `launcher_core::paths` finds it by the same
#   `share/2ksbox` marker. `MacOS/` takes the place of `bin/`, because it
#   is the one directory macOS launches an executable from.
# * Signing goes inside-out and the floor is ours. Every Mach-O is
#   signed before the thing that contains it. The oldest macOS the app
#   runs on is scripts/macos-floor.sh's number, and scripts/build.sh
#   builds everything for it: QEMU's libraries and Qt from source
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
COMMUNITY=0 X86_64=""
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
    -h|--help) sed -n '2,69p' "$0"; exit 0 ;;
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
QB=build/qemu TD=target/release QTD=launcher-qt/target/release D3DPT=build/d3dpt DXVK=build/dxvk CT=()
if [ -n "$ROSETTA" ]; then
  QB=build/x86_64/qemu TD=target/x86_64-apple-darwin/release
  QTD=launcher-qt/target/x86_64-apple-darwin/release D3DPT=build/x86_64/d3dpt DXVK=build/x86_64/dxvk
  CT=(--target x86_64-apple-darwin)
  OUT="${OUT:-$ROOT/build/macos-x86_64}"
fi
OUT="${OUT:-$ROOT/build/macos}"
# The Intel app is the community build and nothing else (ADR-019): the
# App Store build is Apple Silicon only.
if [ "$ARCH" = x86_64 ]; then COMMUNITY=1; fi
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

# The macOS the app is for (scripts/macos-floor.sh), which scripts/build.sh
# built everything for. Exported so that a cargo build below links for it
# too.
MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(scripts/macos-floor.sh)}"
case "$MACOSX_DEPLOYMENT_TARGET" in *.*) ;; *) MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET.0" ;; esac
export MACOSX_DEPLOYMENT_TARGET
FLOOR=$MACOSX_DEPLOYMENT_TARGET

# Our Qt (scripts/build-deps.sh), the one the launcher was built against,
# and the one the build below must use: cxx-qt-build finds Qt through
# QMAKE, else a qmake6 on PATH, and a Homebrew Qt there (6.11 on the Air,
# 2026-09-24) compiled `qt_version_tag_6_11` into the launcher and the
# link against our 6.9.3 frameworks failed. scripts/build.sh's qt stage
# exports the same; this used to set it only after the build.
QMAKE=${QMAKE:-$ROOT/build/deps/$ARCH/bin/qmake}
command -v "$QMAKE" >/dev/null || { echo "package-macos.sh: no $QMAKE (scripts/build.sh deps); the launcher is Qt 6" >&2; exit 1; }
export QMAKE

if [ "$BUILD" = 1 ]; then
  cargo build --release -p player ${CT[@]+"${CT[@]}"}
  # Its own cargo workspace (ADR-015), so its own build command. That
  # boundary keeps Qt 6 off a plain `cargo build`.
  ( cd launcher-qt && cargo build --release ${CT[@]+"${CT[@]}"} )
fi
need "$QTD/launcher-qt" "scripts/build.sh qt"
need "$TD/player"

# --- stage -----------------------------------------------------------
rm -rf "$APP"
mkdir -p "$C"/{MacOS,Resources,lib/2ksbox,libexec/2ksbox,share/2ksbox,share/doc/2ksbox}

install -m755 "$QTD/launcher-qt" "$C/MacOS/2ksbox"
install -m755 "$TD/player"   "$C/MacOS/2ksbox-player"
install -m755 "$QB/libqemu-embed-i386.dylib" "$C/lib/2ksbox/"
install -m755 "$QB/qemu-img" "$C/libexec/2ksbox/"
cp -a qemu/pc-bios "$C/share/2ksbox/pc-bios"
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
# names them through the environment instead (player/src/companions.rs);
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

# --- Info.plist, first pass -------------------------------------------
# `macdeployqt` below reads CFBundleExecutable out of the plist to know
# which binary to follow, so it has to exist before Qt is deployed rather
# than after everything is staged. It is written again at the end, when
# the floor can be measured from what the bundle actually carries.
write_plist() { sed -e "s/@VERSION@/$VERSION/" -e "s/@MINOS@/$1/" \
  packaging/macos/Info.plist.in > "$C/Info.plist"; }
write_plist "$FLOOR"

# --- Qt ---------------------------------------------------------------
# The frameworks, the cocoa platform plugin and the QtQuick QML modules
# (ADR-015). `-qmldir` is not optional: our QML is compiled into the
# binary as a Qt resource, so the import scanner has nothing to read
# unless it is pointed at the sources, and a bundle deployed without it
# starts and then dies on `module "QtQuick" is not installed`.
# Our Qt (QMAKE, exported above the build).
QT_BINS=$("$QMAKE" -query QT_HOST_BINS)
QT_PLUGINS=$("$QMAKE" -query QT_INSTALL_PLUGINS)
MACDEPLOYQT=${MACDEPLOYQT:-$QT_BINS/macdeployqt}
[ -x "$MACDEPLOYQT" ] || { echo "package-macos.sh: no macdeployqt at $MACDEPLOYQT (MACDEPLOYQT=)" >&2; exit 1; }
echo "qt             $("$QMAKE" -query QT_VERSION) from $("$QMAKE" -query QT_INSTALL_PREFIX)"
#
# Its "ERROR: Cannot resolve rpath @rpath/Qt<X>.framework/…" pairs are
# folded into one line: they are the over-collection described below,
# they come two lines each and by the dozen, and real trouble here is a
# different shape and a non-zero exit. What it collected is checked
# afterwards, at the bottom, rather than read out of this scroll.
"$MACDEPLOYQT" "$APP" -qmldir="$ROOT/launcher-qt/qml" -no-strip 2>&1 | awk '
  /^ERROR: Cannot resolve rpath/ { n++; pair=1; next }
  /^ERROR:  using QList/ && pair  { pair=0; next }
  { pair=0; print }
  END { if (n) printf "macdeployqt    %d unresolved framework references, pruned below\n", n }'

# What macdeployqt left half-rewritten. It copies a plain dependency
# dylib into Frameworks/ and rewrites the *reference* to it. When the
# reference was already `@rpath/libfoo.dylib` (more and more Homebrew
# dylibs, brotli 1.2.0 among them) there is nothing to rewrite, and the
# copy keeps its Homebrew install name and Homebrew's `@loader_path/../lib`
# rpath. From Contents/Frameworks that rpath points at Contents/lib, where
# nothing of Qt's lives, so the sibling resolves nowhere and the first
# thing to load it (QtNetwork -> libbrotlidec -> libbrotlicommon) dies on a
# machine with no Homebrew. So give every plain dylib there what the
# closure below gives its own: an `@rpath` id, and `@loader_path` to find
# its siblings with.
for f in "$C/Frameworks"/*.dylib; do
  [ -f "$f" ] || continue
  install_name_tool -id "@rpath/$(basename "$f")" "$f" 2>/dev/null || true
  install_name_tool -add_rpath "@loader_path" "$f" 2>/dev/null || true
done

# What macdeployqt collected that can never load. Homebrew's Qt is
# modular (qtbase, qtdeclarative, qtvirtualkeyboard... each its own
# prefix), and every installed formula symlinks its plugins and QML
# modules into one shared tree, while macdeployqt deploys plugin
# *categories* and QML module *directories* whole. So a launcher that
# imports QtQuick, Controls, Dialogs and Layouts also carries
# QtQuick.VirtualKeyboard, Scene2D/3D, Pdf, Timeline and
# QtQml.StateMachine, whose frameworks live in prefixes macdeployqt never
# walked into. That is the "ERROR: Cannot resolve rpath
# @rpath/QtVirtualKeyboard.framework/..." it prints and carries on from. A
# plugin whose framework is not in the bundle cannot be dlopened on any
# machine, so drop it rather than sign 25 MB that would fail its first
# import. A real unresolved dependency then shows up as a missing file in
# the check below instead of one more line in that scroll. Pruning too
# much is caught too, because the offscreen window further down opens on
# the QML modules that survive this.
#
# A QML module's plugin under Resources/qml is a **symlink** into
# PlugIns, not a copy. So the binaries are pruned first and the modules
# left holding a dangling link go after them. That also keeps a module
# from surviving as .qml files with no plugin, where an import would fail
# as "plugin cannot be loaded" instead of "module is not installed".
missing_fw() {  # frameworks a Mach-O names that the bundle does not carry
  otool -L "$1" | awk '{print $1}' \
    | sed -n 's|^@rpath/\([^/]*\.framework\)/.*|\1|p' | sort -u \
    | while read -r fw; do [ -d "$C/Frameworks/$fw" ] || echo "$fw"; done
}

pruned=0 prunedk=0
while read -r f; do
  miss=$(missing_fw "$f" | tr '\n' ' ')
  [ -n "$miss" ] || continue
  echo "qt prune       ${f#"$C/"} (needs ${miss% })"
  prunedk=$((prunedk + $(du -k "$f" | cut -f1))); pruned=$((pruned+1))
  rm -f "$f"
done < <(find "$C/PlugIns" "$C/Resources/qml" -type f -name '*.dylib' 2>/dev/null | sort)

# The modules those plugins belonged to (a dangling link is a plugin that
# just went). Take the whole module directory, but only once nothing
# under it still resolves, so a module sharing a directory with a plugin
# we kept is never taken with it.
while read -r l; do
  [ -L "$l" ] || continue        # its module went with an earlier link
  mod=$(dirname "$l")
  if [ -f "$mod/qmldir" ] && [ "$mod" != "$C/Resources/qml" ] \
     && [ -z "$(find "$mod" -name '*.dylib' -exec test -e {} \; -print -quit)" ]; then
    echo "qt prune       ${mod#"$C/"} (its plugin went)"
    prunedk=$((prunedk + $(du -sk "$mod" | cut -f1)))
    rm -rf "$mod"
  else
    rm -f "$l"
  fi
done < <(find "$C/Resources/qml" -type l ! -exec test -e {} \; -print 2>/dev/null | sort -r)
[ "$pruned" = 0 ] || echo "qt prune       $pruned plugins and their modules, $((prunedk/1024)) MB"

# The offscreen platform plugin, which macdeployqt does not deploy (it
# brings the cocoa one, which is the only one an app needs to run). The
# check below asks the staged launcher for a real window without opening
# one on the packager's screen, and that is the plugin it asks through.
if [ -f "$QT_PLUGINS/platforms/libqoffscreen.dylib" ]; then
  mkdir -p "$C/PlugIns/platforms"
  install -m755 "$QT_PLUGINS/platforms/libqoffscreen.dylib" "$C/PlugIns/platforms/"
else
  warn "no libqoffscreen.dylib in $QT_PLUGINS/platforms; the window check below will be skipped"
fi

# Every plugin has to reach the bundle's own Frameworks, and on this Qt
# none can. They arrive carrying only Homebrew's
# `@loader_path/../../../../lib`, which from Contents/PlugIns/<category>
# points at the *build directory* (the path in macdeployqt's own "using
# QList(...)" complaints). That works for plugins whose Qt references it
# rewrote to @executable_path. The ones Homebrew built with @rpath
# references (libqsvg, libqsvgicon, the multimedia plugin) resolve nowhere
# and fail their dlopen with the framework sitting right there. So give
# each both ways in: from itself, and from whatever executable loaded it.
# The second covers a QML plugin, which Resources/qml reaches through a
# symlink, so its @loader_path is not the directory the file is in.
while read -r f; do
  install_name_tool -add_rpath "@loader_path/../../Frameworks" "$f" 2>/dev/null || true
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$f" 2>/dev/null || true
done < <(find "$C/PlugIns" -type f -name '*.dylib' 2>/dev/null)

# --- the dylib closure ------------------------------------------------
# Everything outside /usr/lib and /System (Homebrew's glib/pixman/zstd...)
# copied into lib/2ksbox and rewritten to @rpath, transitively. XQuartz's
# libGL and X11 libraries are no longer linked (patch 70).
LIBDIR="$C/lib/2ksbox"
# A library's own install name is the first line `otool -L` prints and no
# dependency: Qt's framework binaries keep Homebrew's, which loads nothing.
# A universal library (the LunarG loader) answers `otool -D` once per
# architecture, under a header line each; one id is wanted, not a
# two-line string awk warns about.
external() {
  local id
  id=$(otool -D "$1" | grep -v ':$' | sort -u | head -1)
  otool -L "$1" | tail -n +2 | awk -v id="$id" '$1 != id {print $1}' | grep -E '^(/opt/|/usr/local/)' || true
}

# Every Mach-O in the bundle, which is not the same as every executable
# file in it. A QML module's plugin can be mode 644 and still carry a
# signature that a rewritten load command invalidates, and on arm64 a
# broken signature is SIGKILL, not a warning. The framework binaries
# (Frameworks/QtQuick.framework/Versions/A/QtQuick, no extension, mode
# 644) are the same. macdeployqt signs what it rewrites, so leaving them
# out went unnoticed until macos-bottles.py replaced them; then the ad-hoc
# pass below skipped every one and the launcher died of "Code Signature
# Invalid" mapping its first framework.
machos() {
  find "$C" -type f \( -perm +111 -o -name '*.dylib' -o -path '*.framework/Versions/*' \) \
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

for f in "$LIBDIR"/*.dylib "$C/libexec/2ksbox/qemu-img" "$C/MacOS/2ksbox" "$C/MacOS/2ksbox-player"; do
  [ -f "$f" ] || continue
  bundle_deps "$f"
done
# Our own libraries kept an absolute or build-tree id; @rpath is what the
# things loading them ask for.
for leaf in libqemu-embed-i386.dylib libd3dpt_exec.dylib libd3dpt_exec_remote.dylib libdxvk_d3d9.0.dylib libvulkan.1.dylib libvulkan_kosmickrisp.dylib; do
  [ -f "$LIBDIR/$leaf" ] || continue
  install_name_tool -id "@rpath/$leaf" "$LIBDIR/$leaf" 2>/dev/null || true
  install_name_tool -add_rpath "@loader_path" "$LIBDIR/$leaf" 2>/dev/null || true
done

# And the executables. The player already has @loader_path/../lib/2ksbox
# from player/build.rs.
install_name_tool -add_rpath "@loader_path/../lib/2ksbox" "$C/MacOS/2ksbox-player" 2>/dev/null || true
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
# has to make the staged app runnable.
while read -r f; do codesign --force --sign - "$f" >/dev/null 2>&1 || true; done \
  < <(machos)

# --- icon -------------------------------------------------------------
# The same PNGs the Linux package installs (`scripts/gen-icons.sh`), so
# the three platforms draw one icon from one master. Nothing is
# rasterized here. An .icns is a container, and iconutil accepts a
# partial set; 512@2x would need a 1024 the artwork does not have.
set=$(mktemp -d)/2ksbox.iconset; mkdir -p "$set"
for s in 16 32 64 128 256 512; do
  cp "packaging/icon/2ksbox-$s.png" "$set/icon_${s}x${s}.png"
done
# The @2x names Apple wants are the next size up under the previous name.
for s in 16 32 128 256; do cp "$set/icon_$((s*2))x$((s*2)).png" "$set/icon_${s}x${s}@2x.png"; done
rm -f "$set/icon_64x64.png"
iconutil -c icns "$set" -o "$C/Resources/2ksbox.icns"

# --- Info.plist -------------------------------------------------------
# What the bundle's own Mach-O files require, measured rather than
# claimed; the check below fails when that is above the floor.
minos_of() { otool -l "$1" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}'; }
minos=$(machos | while read -r f; do minos_of "$f"; done | sort -V | tail -1)
minos=${minos:-$FLOOR}
write_plist "$minos"
echo "minimum macOS $minos (the floor: $FLOOR)"

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
# missed and a half-rewritten install name (see the Frameworks pass).
# dyld expands @rpath with the rpaths of every image in the chain that led
# to the load, not only the one holding the reference. That is why a Qt
# plugin whose own rpath is Homebrew's stale `@loader_path/../../../../lib`
# still finds QtSvg: the executable that dlopened it has
# `@executable_path/../Frameworks`. The check has to do the same, or it
# fails files that work.
exe_rpaths=""
for x in "$C/MacOS/2ksbox" "$C/MacOS/2ksbox-player"; do
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
  case "$what" in player|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
  case "$path" in "("*) continue ;; esac
  case "$path" in "$APP"|"$APP"/*) ;; *) echo "package-macos.sh: $what resolved outside the app: $path" >&2; fail=1 ;; esac
done <<< "$resolved"

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
# build/qemu, so the @loader_path rpath (player/build.rs) is all there is
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

# The window, which `--paths` never opens and the closure cannot check.
# Qt loads its platform plugin and every QtQuick module by name at run
# time, out of PlugIns/ and Resources/qml, and no load command names them.
# So open one offscreen, off the packager's screen, and ask the loader the
# same question while it runs. A PNG out of `LAUNCHER_QT_SHOT` (doc 07)
# means the QML engine ran; the image list shows it ran on our copy of Qt
# and not this Mac's Homebrew one.
if [ -f "$C/PlugIns/platforms/libqoffscreen.dylib" ]; then
  shot="$scratch/window.png"
  # `|| true`: a launcher that fails to load its QML exits non-zero, and
  # under pipefail that would end the script here, silently, instead of
  # in the verdict below with the launcher's own words.
  qtout=$(cd / && env -i HOME="$scratch" LAUNCHER_LIBRARY_DIR="$scratch/machines" \
    QT_QPA_PLATFORM=offscreen LAUNCHER_QT_SHOT="$shot" LAUNCHER_QT_DELAY=1500 \
    DYLD_PRINT_LIBRARIES=1 "$C/MacOS/2ksbox" 2>&1 || true)
  qtloaded=$(printf '%s\n' "$qtout" | sed -n 's|^dyld\[[0-9]*\]: <[^>]*> ||p')
  if [ -s "$shot" ]; then
    echo "window         $(du -h "$shot" | cut -f1) grabbed offscreen: QML, plugins and all"
  else
    printf '%s\n' "$qtout" | grep -v '^dyld\[' | sed 's/^/  /' >&2
    echo "package-macos.sh: the staged launcher opened no window offscreen (Qt plugins or QML modules missing)" >&2
    fail=1
  fi
  outside=$(printf '%s\n' "$qtloaded" | grep -v -e "^$APP/" -e '^/usr/lib/' -e '^/System/' || true)
  if [ -n "$outside" ]; then
    printf '%s\n' "$outside" | sed 's/^/  /' >&2
    echo "package-macos.sh: the staged launcher loaded the above from outside the app" >&2
    fail=1
  fi
else
  echo "window         (no offscreen plugin staged; not checked)"
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
[ "$fail" = 0 ] || exit 1
echo "checks passed"

# --- sign -------------------------------------------------------------
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
    local i out
    for i in 1 2 3 4 5; do
      if out=$(codesign --force --timestamp --options runtime \
                 --entitlements packaging/macos/2ksbox.entitlements \
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
  # Inside-out: every nested Mach-O before the thing that seals it, and a
  # framework signed as the bundle it is rather than as the file inside
  # it. `codesign` seals a framework by its directory, and
  # `--verify --deep --strict` rejects one sealed only at Versions/A/QtCore.
  while read -r f; do
    case "$f" in
      "$C"/Frameworks/*.framework/*) continue ;;   # signed as a bundle below
      "$C"/MacOS/2ksbox) continue ;;               # the app's own executable, last
    esac
    sign_one "$f"
  done < <(machos | sort -u)
  for fw in "$C"/Frameworks/*.framework; do
    [ -d "$fw" ] && sign_one "$fw"
  done
  sign_one "$C/MacOS/2ksbox"
  sign_one "$APP"
  codesign --verify --deep --strict --verbose=2 "$APP"
  # The question Gatekeeper will ask on the other Mac. Before notarization
  # it answers "not notarized", which is the one remaining step, not a
  # failure. Any other rejection is.
  spctl --assess --type execute --verbose=4 "$APP" 2>&1 | sed 's/^/  /' || true
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
  hdiutil create -volname "2ksbox $VERSION" -srcfolder "$dmgroot" -ov -format UDZO -quiet "$dmg"
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
