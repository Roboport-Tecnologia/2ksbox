#!/usr/bin/env bash
# Build and install the Flatpak (manifest in packaging/flatpak/).
# Everything is compiled inside the SDK, because the host's glibc is newer
# than the runtime's and host-built binaries cannot run in it.
#
# Never run this at the same time as `scripts/build-windows.sh`. Both use
# the one `qemu/` tree. The Windows build re-applies the patch queue while
# flatpak-builder copies the tree into its sandbox, the copy comes out half
# patched, and QEMU's compile fails deep inside on a header neither build
# uses (`hw/core/sysbus.h: No such file`). Run one, then the other.
#
# The runtime is `org.gnome.Platform`, because the launcher is mitsuami
# on GTK 4 (ADR-023) and GNOME's runtime carries GTK 4. The first build
# downloads the runtime + SDK pair.
#
#   scripts/package-flatpak.sh              build, install --user, smoke check
#   scripts/package-flatpak.sh --no-install just build into the repo
#   scripts/package-flatpak.sh --no-wine    skip the Wine add-on (below)
#   scripts/package-flatpak.sh --wine-only  only the Wine add-on, onto the installed app
#   scripts/package-flatpak.sh --no-kde     skip the KDE add-on (below)
#   scripts/package-flatpak.sh --kde-only   only the KDE add-on, onto the installed app
#   scripts/package-flatpak.sh --check      only re-run the smoke check
#
# Two add-ons, each a manifest of its own built after the app, because
# the app is their runtime:
# - Wine (M15 step 7), `com._2ksbox.Launcher.Wine.yml`. Building Wine
#   from source takes about half an hour more.
# - KDE, `com._2ksbox.Launcher.KDE.yml`: the launcher on Kirigami for a
#   Plasma session, with the Qt and KDE Frameworks it needs built from
#   source (about an hour more).
# The smoke check expects each unless --no-wine / --no-kde was given.
#
# Environment:
#   FLATPAK_USER_DIR    which `--user` installation to use (flatpak's own
#                       variable). Set it if ~/.local/share/flatpak is on
#                       a full filesystem.
#   FLATPAK_BUILD_DIR   where flatpak-builder works, default build/flatpak.
#                       The build tree is several GB (a whole QEMU and a
#                       release Rust workspace), so point it at a disk
#                       with room.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

APPID=com._2ksbox.Launcher
WINEID=$APPID.Wine
KDEID=$APPID.KDE
# Both manifests leave the branch to the builder, so the local build is
# `stable` like Flathub's and the add-on's `runtime-version: stable`
# resolves to this app. Every ref below names the branch, because an
# older `master` build may still be installed beside it.
BRANCH=stable
MANIFEST="packaging/flatpak/$APPID.yml"
WINE_MANIFEST="packaging/flatpak/$WINEID.yml"
KDE_MANIFEST="packaging/flatpak/$KDEID.yml"
BUILD_DIR="${FLATPAK_BUILD_DIR:-$ROOT/build/flatpak}"
INSTALL=1 ONLY_CHECK=0 WINE=1 KDE=1 APP=1
while [ $# -gt 0 ]; do
  case "$1" in
    --no-install) INSTALL=0; shift ;;
    --no-wine) WINE=0; shift ;;
    --wine-only) APP=0; KDE=0; shift ;;
    --no-kde) KDE=0; shift ;;
    --kde-only) APP=0; WINE=0; shift ;;
    --check) ONLY_CHECK=1; shift ;;
    -h|--help) sed -n '2,42p' "$0"; exit 0 ;;
    *) echo "package-flatpak.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done

smoke() {
  # The app answering from inside its own sandbox. Every companion has to
  # resolve under /app, the same property the tarball's check tests
  # against a different prefix.
  echo "==> flatpak run $APPID --paths"
  local out
  out=$(flatpak run --user --command=2ksbox "$APPID//$BRANCH" --paths) || return 1
  echo "$out"
  local fail=0
  while read -r what path; do
    case "$what" in player|player-x86_64|qemu-img|pc-bios|guest-tools|prefix) ;; *) continue ;; esac
    case "$path" in "("*) continue ;; /app*) ;; *)
      echo "package-flatpak.sh: $what resolved outside /app: $path" >&2; fail=1 ;;
    esac
  done <<< "$out"
  # The Wine add-on (M15 step 7). With it installed the launcher must
  # find the add-on's Wine and the PE pair under the mount point, and
  # nothing else: a `wine` line outside /app is the host's, which the
  # sandbox cannot run. Without it both lines say so, and that is a
  # failure only when the add-on was expected.
  if flatpak info --user "$WINEID//$BRANCH" >/dev/null 2>&1; then
    echo "==> the Wine add-on is installed: wine and wine-host must be under /app/lib/2ksbox/wine"
    while read -r what path rest; do
      case "$what" in wine|wine-host) ;; *) continue ;; esac
      case "$path" in /app/lib/2ksbox/wine/*) ;; *)
        echo "package-flatpak.sh: $what is $path $rest, not the add-on's" >&2; fail=1 ;;
      esac
    done <<< "$out"
    # And the pair under that Wine, in the sandbox, the way QEMU's remote
    # library starts it (doc 14 "The child's Wine": the same prefix and
    # overrides). With a shared file that does not exist the program
    # loads the executor DLL, says so, and exits 4; exit 3 is a DLL it
    # could not load, anything else a Wine that did not run it. The
    # first run makes the prefix (a few seconds).
    echo "==> the pair under the add-on's Wine"
    local pair
    pair=$(flatpak run --user --command=/app/lib/2ksbox/wine/bin/wine \
      --env=WINEPREFIX="$HOME/.var/app/$APPID/data/2ksbox/wine" \
      --env=WINEDLLOVERRIDES="mscoree,mshtml=" --env=WINEDEBUG=-all \
      "$APPID//$BRANCH" /app/lib/2ksbox/wine/d3dpt-exec-host.exe /nonexistent </dev/null 2>&1) && rc=0 || rc=$?
    printf '%s\n' "$pair" | grep -E '^d3dpt-exec-host' || true
    if [ "$rc" != 4 ] || ! printf '%s\n' "$pair" | grep -q 'd3dpt-exec-host: executor'; then
      echo "package-flatpak.sh: the pair did not start under the add-on's Wine (exit $rc)" >&2; fail=1
    fi
  elif [ "$WINE" = 1 ]; then
    echo "package-flatpak.sh: the Wine add-on $WINEID//$BRANCH is not installed" >&2; fail=1
  else
    echo "==> no Wine add-on installed (--no-wine): Direct3D below the Vulkan floor is off in this app"
  fi
  # The companions QEMU dlopens by name: the Direct3D executor, the DXVK
  # it runs on and the remote library. They are in no import table,
  # so nothing above would notice their absence. The packaged *player*
  # knows where they should be (`player-core/src/companions.rs`), and
  # `--companions` prints what it resolved. Inside the sandbox the answer
  # has to be under /app. "(not shipped)" means the build made one and did
  # not stage it, or did not make it at all.
  echo "==> flatpak run $APPID --companions"
  local comp
  comp=$(flatpak run --user --command=2ksbox-player "$APPID//$BRANCH" --companions) || return 1
  echo "$comp"
  while read -r what path; do
    case "$what" in d3dpt-exec|dxvk|d3dpt-remote) ;; *) continue ;; esac
    case "$path" in
      /app/*) ;;
      "(not"*) echo "package-flatpak.sh: the app ships no $what (its build step failed, or staged nothing)" >&2; fail=1 ;;
      *) echo "package-flatpak.sh: $what is $path, outside /app" >&2; fail=1 ;;
    esac
  done <<< "$comp"
  # A file picked in the portal's dialog arrives as a document-portal
  # path, which QEMU cannot lock and which hides a disc image's companion
  # files (doc 07, `browse::picked`). Export one the way the dialog
  # would; the launcher in the sandbox must keep the file's own path.
  echo "==> flatpak run $APPID --picked (a document-portal path)"
  local real="$ROOT/packaging/icon/2ksbox.png" doc kept
  if doc=$(flatpak document-export --app="$APPID" -r "$real" 2>/dev/null) && [ -n "$doc" ]; then
    kept=$(flatpak run --user --command=2ksbox "$APPID//$BRANCH" --picked "$doc" 2>/dev/null || true)
    flatpak document-unexport "$real" >/dev/null 2>&1 || true
    echo "picked         $doc -> $kept"
    if [ "$kept" != "$real" ]; then
      echo "package-flatpak.sh: a document-portal path was kept as '$kept', not the file's own path" >&2; fail=1
    fi
  else
    echo "package-flatpak.sh: flatpak document-export failed, so the picked-path check did not run" >&2; fail=1
  fi
  # The data directory is the one thing a Flatpak deliberately moves: it
  # lands under ~/.var/app/<app-id>, not ~/.local/share.
  case "$out" in *"/.var/app/$APPID/"*) ;; *)
    echo "package-flatpak.sh: the library is not under ~/.var/app/$APPID" >&2; fail=1 ;;
  esac
  # And the window, which `--paths` never reaches. The launcher is GTK 4
  # (ADR-023), out of the runtime rather than /app, so a wrong `runtime:`
  # line would break it while every check above stayed green. So ask for a
  # real window: the launcher's own headless grab (`LAUNCHER_SHOT`), on a
  # private Broadway display started inside the sandbox (the runtime's
  # `gtk4-broadwayd`), so nothing opens on the desktop, and a PNG out of
  # it.
  # Under $HOME, not /tmp. The sandbox has a /tmp of its own, so a grab
  # written there is invisible to this shell and the check fails on a good
  # package. `$HOME` is the same path on both sides, and this app has
  # `--filesystem=host`.
  local shot="$HOME/.2ksbox-flatpak-window.png"
  rm -f "$shot"
  echo "==> flatpak run $APPID (window grab on Broadway)"
  # Not in a Plasma session as far as `2ksbox` can tell, or the KDE
  # add-on would answer (below).
  flatpak run --user --command=sh --env=XDG_CURRENT_DESKTOP= --env=LAUNCHER_SHOT="$shot" "$APPID//$BRANCH" -c '
      gtk4-broadwayd :37 >/dev/null 2>&1 & b=$!
      sleep 1
      GDK_BACKEND=broadway BROADWAY_DISPLAY=:37 GTK_USE_PORTAL=0 timeout 60 2ksbox
      kill $b' >/dev/null 2>&1 || true
  if [ -s "$shot" ]; then
    echo "window         $(du -h "$shot" | cut -f1) grabbed on Broadway: GTK 4 from the runtime"
    rm -f "$shot"
  else
    echo "package-flatpak.sh: the app drew no window (LAUNCHER_SHOT on Broadway) — is GTK 4 in the runtime?" >&2
    fail=1
  fi
  # The KDE add-on: in a Plasma session `2ksbox` must start the Kirigami
  # launcher from /app/kde, which must draw a window with the add-on's own
  # Qt and frameworks. Qt's offscreen platform and Qt Quick's software
  # renderer, so nothing opens on the desktop. `2ksbox` is a script that
  # execs the launcher, so the background job's executable says which.
  if flatpak info --user "$KDEID//$BRANCH" >/dev/null 2>&1; then
    rm -f "$shot"
    echo "==> the KDE add-on is installed: flatpak run $APPID in a Plasma session (window grab offscreen)"
    local kde
    kde=$(flatpak run --user --env=XDG_CURRENT_DESKTOP=KDE --env=QT_QPA_PLATFORM=offscreen \
      --env=QT_QUICK_BACKEND=software --env=LAUNCHER_SHOT="$shot" --env=LAUNCHER_SHOT_DELAY_MS=5000 --command=sh "$APPID//$BRANCH" -c \
      '2ksbox & p=$!; sleep 2; readlink /proc/$p/exe; (sleep 60; kill $p) & wait $p' 2>/dev/null || true)
    echo "process        $(printf '%s\n' "$kde" | tail -1)"
    case "$kde" in */app/kde/2ksbox*) ;; *)
      echo "package-flatpak.sh: in a Plasma session 2ksbox did not start /app/kde/2ksbox" >&2; fail=1 ;;
    esac
    if [ -s "$shot" ]; then
      echo "window         $(du -h "$shot" | cut -f1) grabbed offscreen: Kirigami from the add-on"
      rm -f "$shot"
    else
      echo "package-flatpak.sh: the KDE launcher drew no window (LAUNCHER_SHOT offscreen)" >&2; fail=1
    fi
  elif [ "$KDE" = 1 ]; then
    echo "package-flatpak.sh: the KDE add-on $KDEID//$BRANCH is not installed" >&2; fail=1
  else
    echo "==> no KDE add-on installed (--no-kde): the GTK launcher everywhere"
  fi
  return $fail
}

if [ "$ONLY_CHECK" = 1 ]; then smoke; exit $?; fi

command -v flatpak-builder >/dev/null || { echo "flatpak-builder not installed" >&2; exit 1; }
# The patch queue runs on the host: it needs git and rsync, and the tree
# flatpak-builder copies has neither a .git nor rsync in the SDK.
[ -d qemu/hw/3dfx ] || { echo "qemu/ is not prepared: run scripts/prepare-qemu.sh first" >&2; exit 1; }
[ -f guest-tools/out/guest-tools-3dfx-*.iso ] 2>/dev/null || \
  ls guest-tools/out/guest-tools-*.iso >/dev/null 2>&1 || \
  echo "package-flatpak.sh: no guest-tools ISO built; the app will ship without it"

mkdir -p "$BUILD_DIR"
avail=$(df -Pk "$BUILD_DIR" | awk 'NR==2 {print int($4/1048576)}')
[ "$avail" -ge 12 ] || {
  echo "package-flatpak.sh: only ${avail} GB free at $BUILD_DIR; a QEMU + Rust build needs ~12 GB." >&2
  echo "Set FLATPAK_BUILD_DIR to somewhere with room." >&2; exit 1; }
echo "==> installation: ${FLATPAK_USER_DIR:-$HOME/.local/share/flatpak}"
echo "==> build dir:    $BUILD_DIR (${avail} GB free)"

args=(--user --force-clean --default-branch="$BRANCH" --state-dir "$BUILD_DIR/state")
[ "$INSTALL" = 1 ] && args+=(--install)
if [ "$APP" = 1 ]; then
  flatpak-builder "${args[@]}" --install-deps-from=flathub "$BUILD_DIR/build" "$MANIFEST"
fi
if [ "$WINE" = 1 ]; then
  # The add-on builds against the *installed* app (its runtime), so it
  # needs --install above, and the mingw SDK extension the manifest names.
  [ "$INSTALL" = 1 ] || { echo "package-flatpak.sh: the Wine add-on builds against the installed app; drop --no-install or pass --no-wine" >&2; exit 2; }
  flatpak info --user "$APPID//$BRANCH" >/dev/null 2>&1 || { echo "package-flatpak.sh: $APPID//$BRANCH is not installed; build the app first" >&2; exit 1; }
  flatpak-builder "${args[@]}" --install-deps-from=flathub "$BUILD_DIR/build-wine" "$WINE_MANIFEST"
fi

if [ "$KDE" = 1 ]; then
  # Like the Wine add-on: against the installed app, with the rust SDK
  # extension (the launcher) the manifest names
  [ "$INSTALL" = 1 ] || { echo "package-flatpak.sh: the KDE add-on builds against the installed app; drop --no-install or pass --no-kde" >&2; exit 2; }
  flatpak info --user "$APPID//$BRANCH" >/dev/null 2>&1 || { echo "package-flatpak.sh: $APPID//$BRANCH is not installed; build the app first" >&2; exit 1; }
  flatpak-builder "${args[@]}" --install-deps-from=flathub "$BUILD_DIR/build-kde" "$KDE_MANIFEST"
fi

[ "$INSTALL" = 1 ] || { echo "built (not installed): $BUILD_DIR/build"; exit 0; }
smoke
echo "installed: flatpak run $APPID//$BRANCH"
