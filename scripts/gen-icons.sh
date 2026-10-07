#!/usr/bin/env bash
# Every size of the application icon, from the one master.
#
#   scripts/gen-icons.sh            # regenerate packaging/icon/ from the master
#   scripts/gen-icons.sh --check    # fail if anything is out of date (no writes)
#
# The master is `packaging/icon/2ksbox.png`, the official icon (2026-10-07):
# the Default appearance of the user's Icon Composer export, 1024x1024
# RGBA, the rounded square filling its canvas (stored 8-bit; the export
# is 16-bit). It is the only file to replace when the icon changes, with
# `2ksbox.icon` beside it, the Icon Composer document macOS 26 draws from
# (`package-macos.sh` compiles it; it is not derived from here).
# Everything below is derived from it and checked in, because the places
# that consume an icon cannot run ImageMagick:
#
#   * `launcher-mitsuami` embeds one with `include_bytes!` at compile time,
#   * the Flatpak build is offline and installs files, it does not draw them,
#   * the Windows package is cross-built in a container without ImageMagick,
#   * a user running `packaging/linux/install.sh` from a tarball has no
#     build tools at all.
#
# What each size is for (`hicolor` is the freedesktop icon theme's
# directory-per-size layout, which is what a Linux desktop looks in):
#
#   16, 24, 32, 48   task switchers, window decorations, small menus
#   64, 128          the applications menu, GNOME Software's lists
#   256, 512         software-centre banners, macOS, HiDPI everywhere
#   1024             macOS's 512@2x, which App Store Connect requires
#                    (ITMS-90236); made only from a master of 1024 or
#                    more, so `package-macos.sh --app-store` refuses to
#                    build until the master is that large
#   2ksbox.ico       Windows: 16/32/48/256 in one file, which is what a
#                    shortcut and an .exe resource both want
#
# and, under `packaging/icon/macos/`, the same sizes on Apple's grid for
# the app's .icns (`package-macos.sh`): the icon's body 824 of 1024, centred,
# as every macOS app icon is drawn (the master fills its canvas, which is
# right on Linux and Windows but would stand oversized in the Dock);
#
# and, under `packaging/windows/Assets/`, the four logos an MSIX manifest
# names (`scripts/package-msix.sh`), at the exact sizes the Store checks:
#
#   Square44x44Logo   44x44    the taskbar, the Start list: the icon whole
#   StoreLogo         50x50    the Store listing and the installer
#   Square150x150Logo 150x150  the Start tile: the icon at three quarters,
#   Wide310x150Logo   310x150  centred, because Windows paints the tile's
#                              background behind it (BackgroundColor)
#
# Nothing is ever scaled *up*. The master is first padded with
# transparency to 512x512 (1024x1024 when the master is at least 1024 on
# a side), centred, and every size is a downscale of that.
# Padding rather than resizing keeps the drawing at its native size in the
# largest icon. If the artwork is redrawn larger, nothing here changes.
set -euo pipefail
cd "$(dirname "$0")/.."

DIR=packaging/icon
MASTER=$DIR/2ksbox.png
SIZES=(16 24 32 48 64 128 256 512)
ICO_SIZES=(16 32 48 256)

check=0
[ "${1:-}" = "--check" ] && check=1

command -v magick >/dev/null || { echo "gen-icons.sh: ImageMagick (magick) is required" >&2; exit 1; }
[ -f "$MASTER" ] || { echo "gen-icons.sh: no master at $MASTER" >&2; exit 1; }
# A scratch folder as magick can read it. In MSYS2 that is C:/...: a
# /tmp/... path behind a `PNG32:` prefix reaches the native magick
# unconverted, and it segfaults on it instead of saying so.
tmpdir() { local d; d=$(mktemp -d); command -v cygpath >/dev/null && d=$(cygpath -m "$d"); echo "$d"; }

# The square the sizes come from: the master centred on a transparent
# canvas, 1024x1024 for a master at least 1024 on a side (which adds the
# 1024 size) and 512x512 below that. A master already that size passes
# through unchanged, and one *larger* is scaled down to fit first, so this
# stays right whatever the artwork's canvas is.
side=$(magick identify -format '%[fx:max(w,h)]' "$MASTER")
canvas=512
[ "$side" -ge 1024 ] && { canvas=1024; SIZES+=(1024); }
pad=$(tmpdir)/master-$canvas.png
trap 'rm -rf "$(dirname "$pad")"' EXIT
magick "$MASTER" -background none -colorspace sRGB \
  -resize "${canvas}x${canvas}>" -gravity center -extent "${canvas}x${canvas}" -strip "PNG32:$pad"

# `-background none` keeps the alpha the master has (the icon is not a
# square: it has to sit on whatever colour a desktop puts behind it), and
# the explicit sRGB colorspace stops ImageMagick from linearising the
# downscale, which lightens a dark icon's edges.
render() { # size, out
  magick "$pad" -background none -colorspace sRGB \
    -resize "${1}x${1}" -strip "PNG32:$2"
}

out=$DIR
if [ "$check" = 1 ]; then
  out=$(tmpdir); trap 'rm -rf "$out"' EXIT
fi

for s in "${SIZES[@]}"; do render "$s" "$out/2ksbox-$s.png"; done
# A smaller master has no 1024: drop one an older, larger master made.
[ "$check" = 1 ] || [ "$canvas" = 1024 ] || rm -f "$DIR/2ksbox-1024.png"
# One .ico holding the four sizes Windows actually picks from.
ico_inputs=(); for s in "${ICO_SIZES[@]}"; do ico_inputs+=("$out/2ksbox-$s.png"); done
magick "${ico_inputs[@]}" "$out/2ksbox.ico"

# The Store logos: the icon drawn at `icon` pixels on a transparent
# `w`x`h` canvas, centred.
ASSETS=packaging/windows/Assets
aout=$ASSETS
[ "$check" = 1 ] && { aout=$out/Assets; mkdir -p "$aout"; }
logo() { # name, w, h, icon
  magick "$pad" -background none -colorspace sRGB -resize "${4}x${4}" \
    -gravity center -extent "${2}x${3}" -strip "PNG32:$aout/$1.png"
}
logo Square44x44Logo   44  44  44
logo StoreLogo         50  50  50
logo Square150x150Logo 150 150 112
logo Wide310x150Logo   310 150 112

# macOS: Apple's icon grid, the body 824/1024 of the canvas, centred.
MAC=$DIR/macos
mout=$MAC
[ "$check" = 1 ] && mout=$out/macos
mkdir -p "$mout"
for s in 16 32 64 128 256 512 1024; do
  [ "$s" -le "$canvas" ] || continue
  body=$(( (s * 824 + 512) / 1024 ))
  magick "$pad" -background none -colorspace sRGB -resize "${body}x${body}" \
    -gravity center -extent "${s}x${s}" -strip "PNG32:$mout/2ksbox-$s.png"
done
[ "$check" = 1 ] || [ "$canvas" = 1024 ] || rm -f "$MAC/2ksbox-1024.png"

# The same picture: the same bytes, or no pixel more than a fifth of a
# channel apart. ImageMagick builds resample a few edge pixels differently
# (the Store logos from Linux's and MSYS2's 7.1.2: at most 0.15 of a
# channel on 25-65 pixels), while a master that changed moves the picture.
same() { cmp -s "$1" "$2" || [ "$(magick compare -fuzz 20% -metric AE "$1" "$2" null: 2>&1 | cut -d' ' -f1)" = 0 ]; }
if [ "$check" = 1 ]; then
  rc=0
  for f in "$out"/*.png "$out"/*.ico; do
    n=$(basename "$f")
    same "$f" "$DIR/$n" || { echo "gen-icons.sh: $DIR/$n is out of date"; rc=1; }
  done
  for f in "$aout"/*.png; do
    n=$(basename "$f")
    same "$f" "$ASSETS/$n" || { echo "gen-icons.sh: $ASSETS/$n is out of date"; rc=1; }
  done
  for f in "$mout"/*.png; do
    n=$(basename "$f")
    same "$f" "$MAC/$n" || { echo "gen-icons.sh: $MAC/$n is out of date"; rc=1; }
  done
  # A 1024 left from a larger master would ship a picture the master no
  # longer is.
  [ "$canvas" = 1024 ] || [ ! -e "$DIR/2ksbox-1024.png" ] \
    || { echo "gen-icons.sh: $DIR/2ksbox-1024.png is left from an older master (this one is ${side}px)"; rc=1; }
  [ $rc = 0 ] && echo "gen-icons.sh: every size matches the master"
  exit $rc
fi

echo "gen-icons.sh: wrote $DIR/2ksbox-{$(IFS=,; echo "${SIZES[*]}")}.png, 2ksbox.ico, $MAC/*.png and $ASSETS/*.png"
