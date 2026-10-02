#!/usr/bin/env bash
# Install this package into a prefix.
#
# You don't have to. The unpacked tree runs from wherever it is, because
# `bin/2ksbox` finds the player, qemu-img, the firmware and the
# guest-tools ISO relative to its own location. Installing adds a desktop
# entry and an icon, so the launcher shows up in the applications menu.
#
#   ./install.sh                 # into ~/.local (no root)
#   ./install.sh --prefix /opt/2ksbox
#   sudo ./install.sh --prefix /usr/local
#   ./install.sh --uninstall     # removes what a previous run put there
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
prefix=${HOME}/.local
uninstall=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --prefix=*) prefix=${1#*=}; shift ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "install.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

# The product name owns the directories and the commands; the application
# ID owns the desktop entry and the icon, because that pair is what a
# desktop matches window to launcher by (and what a Flatpak will require).
app=2ksbox
appid=com._2ksbox.Launcher
desktop_dir=$prefix/share/applications
# The icons arrive with the wholesale `share/` copy below, already under
# `share/icons/hicolor/<n>x<n>/apps/`. These name the size the desktop
# entry points at and the tree the uninstall clears (the one thing this
# installs outside `share/2ksbox`).
icon_dir=$prefix/share/icons/hicolor
icon_size=256
metainfo_dir=$prefix/share/metainfo

if [ "$uninstall" = 1 ]; then
  rm -rf "$prefix/lib/$app" "$prefix/libexec/$app" "$prefix/share/$app" "$prefix/share/doc/$app"
  rm -f "$prefix/bin/$app" "$prefix/bin/$app-player"
  rm -f "$desktop_dir/$appid.desktop" "$metainfo_dir/$appid.metainfo.xml"
  rm -f "$icon_dir"/*/apps/"$appid.png"
  command -v update-desktop-database >/dev/null && update-desktop-database "$desktop_dir" 2>/dev/null || true
  echo "removed $app from $prefix"
  exit 0
fi

if [ "$here" -ef "$prefix" ]; then
  echo "install.sh: source and destination are the same directory" >&2
  exit 1
fi

# The layout is copied wholesale: every path the launcher resolves is
# relative to bin/, so anything that arrives here as a set has to leave as
# one. `cp -a` rather than install(1) per file, because pc-bios alone is
# hundreds of files and none of them need a mode of their own.
for dir in bin lib libexec share; do
  [ -d "$here/$dir" ] || continue
  mkdir -p "$prefix/$dir"
  cp -a "$here/$dir/." "$prefix/$dir/"
done

# The desktop entry ships with a bare `Exec=2ksbox`, which is only right
# if the prefix's bin/ is on PATH. Here we know the absolute path, so
# write it in.
#
# `Icon=` likewise. The bare `Icon=<app id>` only resolves when the
# prefix's `share/` is on XDG_DATA_DIRS, which a private prefix is not, so
# the entry names one size outright. The whole set is still installed, and
# a desktop that does see the prefix picks the size it wants.
mkdir -p "$desktop_dir"
sed -e "s|^Exec=.*|Exec=$prefix/bin/$app|" \
    -e "s|^Icon=.*|Icon=$icon_dir/${icon_size}x${icon_size}/apps/$appid.png|" \
    "$here/share/$app/desktop/$appid.desktop" > "$desktop_dir/$appid.desktop"
# AppStream metadata for software centres. Copied unmodified, since
# nothing in it depends on the path.
mkdir -p "$metainfo_dir"
cp -f "$here/share/$app/desktop/$appid.metainfo.xml" "$metainfo_dir/$appid.metainfo.xml"
command -v update-desktop-database >/dev/null && update-desktop-database "$desktop_dir" 2>/dev/null || true

echo "installed into $prefix"
echo "  launcher: $prefix/bin/$app"
echo "  player:   $prefix/bin/$app-player"

# GTK 4 is the one thing this package does not carry (ADR-023): every
# distribution has it, and a copy of our own would still have to match the
# host's Wayland, OpenGL and fontconfig. Say so here rather than let the
# launcher fail with a loader error nobody can act on.
if command -v ldd >/dev/null && ldd "$prefix/bin/$app" 2>/dev/null | grep -q 'not found'; then
  echo
  echo "  GTK 4 is missing on this system, so the launcher will not start:"
  ldd "$prefix/bin/$app" | grep 'not found' | sed 's/^/    /'
  echo "  Install it from your distribution:"
  echo "    Arch          gtk4"
  echo "    Fedora        gtk4"
  echo "    Debian/Ubuntu libgtk-4-1"
fi
case ":$PATH:" in
  *":$prefix/bin:"*) ;;
  *) echo "  ($prefix/bin is not on your PATH; the desktop entry uses the absolute path)" ;;
esac
