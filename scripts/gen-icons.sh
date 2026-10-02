#!/usr/bin/env bash
# Every size of the application icon, from the one master.
#
#   scripts/gen-icons.sh            # regenerate packaging/icon/ from the master
#   scripts/gen-icons.sh --check    # fail if anything is out of date (no writes)
#
# The master is `packaging/icon/2ksbox.png`, a transparent RGBA render of
# a beige CRT on a 500x500 canvas it does not fill (the drawing is
# 431x436). It is the only file to replace when the icon changes.
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
#   2ksbox.ico       Windows: 16/32/48/256 in one file, which is what a
#                    shortcut and an .exe resource both want
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
# transparency to 512x512, centred, and every size is a downscale of that.
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

# The square the sizes come from: the master centred on a 512x512
# transparent canvas. A master already 512x512 passes through unchanged,
# and one *larger* than that is scaled down to fit first, so this stays
# right whatever the artwork's canvas is.
pad=$(tmpdir)/master-512.png
trap 'rm -rf "$(dirname "$pad")"' EXIT
magick "$MASTER" -background none -colorspace sRGB \
  -resize '512x512>' -gravity center -extent 512x512 -strip "PNG32:$pad"

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
  [ $rc = 0 ] && echo "gen-icons.sh: every size matches the master"
  exit $rc
fi

echo "gen-icons.sh: wrote $DIR/2ksbox-{$(IFS=,; echo "${SIZES[*]}")}.png, 2ksbox.ico and $ASSETS/*.png"
