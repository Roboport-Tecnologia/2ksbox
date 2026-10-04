#!/usr/bin/env bash
# Build the libraries the macOS app carries from their upstream sources,
# ourselves, into build/deps/<arch>. For QEMU: glib (with pcre2, and the
# libffi and stub libintl its own tarball carries as subprojects, pinned
# by its wrap files), pixman, libslirp, zstd, and libtpms with its
# libcrypto, static, so libqemu-embed and qemu-img carry them and the app ships no
# dependency dylib for QEMU's side; and spice-protocol's headers, for the
# clipboard's `qemu-vdagent` (nothing of it is linked). The launcher needs nothing here: it
# is AppKit (launcher-mitsuami, ADR-023). Every version is pinned with its
# checksum below, and nothing is fetched after the tarballs.
#
#   scripts/build-deps.sh                 this Mac's architecture, the floor
#   scripts/build-deps.sh --arch x86_64   the Intel build's (build/deps/x86_64)
#   scripts/build-deps.sh --clean         from scratch (a recipe changed for
#                                         the same version: the stamps are
#                                         name, version, patch set and floor)
#
# patches/deps/<name>/*.patch are applied to a package's unpacked tree
# (patches/deps/README.md); a changed set rebuilds that package alone.
#
# Why not Homebrew's (docs/build-macos.md, "The libraries", user decision
# 2026-09-23): Homebrew builds every library for the macOS it runs on and
# publishes bottles for three releases back, so the app's floor was
# Homebrew's floor, its Intel build ended when Homebrew's installer
# refused Intel Macs, and one `brew upgrade` changed what shipped. These
# builds target MACOSX_DEPLOYMENT_TARGET (scripts/build.sh exports the
# floor), for the architecture named, from tarballs whose checksums are
# here, and nothing of the Mac's package manager is in the app.
#
# meson, ninja, pkg-config and a C compiler are still needed to *build*;
# they ship nothing. Under Rosetta (scripts/build.sh --x86_64) the arch
# defaults to x86_64, as everywhere else in that build.
#
# On Linux it builds QEMU's glib (pcre2, glib, and libslirp, the one
# other library QEMU links that links glib) and libtpms with its
# libcrypto (the TPM 2.0 of a Windows 11 box), static, and
# spice-protocol's headers (the clipboard), into
# build/deps/<arch>, which scripts/configure-qemu.sh links by default
# there. QEMU's main loop iterates glib's global default GMainContext on
# QEMU's thread; with one glib shared with its process, that is the
# context a toolkit's own loop runs on (docs/tracks/m22-mitsuami-player.md, "Why QEMU links a GLib of its own").
# Everything else stays the distribution's. The Flatpak runs this script
# in its SDK, offline, with the tarballs as declared sources.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OS="$(uname -s)"
case "$OS" in Darwin|Linux) ;; *) echo "build-deps.sh: macOS and Linux only" >&2; exit 1 ;; esac

ARCH="$(uname -m)"; CLEAN=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH=$2; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-deps.sh: unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [ "$OS" = Linux ]; then
  [ "$ARCH" = "$(uname -m)" ] || { echo "build-deps.sh: Linux builds this machine's architecture only" >&2; exit 2; }
  # The stamps' target part: nothing here targets an OS version.
  T=linux; FOR="Linux"
else
  case "$ARCH" in arm64|x86_64) ;; *) echo "build-deps.sh: --arch arm64|x86_64" >&2; exit 2 ;; esac
  T="${MACOSX_DEPLOYMENT_TARGET:-$("$ROOT/scripts/macos-floor.sh")}"
  case "$T" in *.*) ;; *) T="$T.0" ;; esac
  export MACOSX_DEPLOYMENT_TARGET="$T"
  SDK="$(xcrun --show-sdk-path)"
  FOR="macOS $T"
fi
SRC="$ROOT/build/deps/src"
PREFIX="$ROOT/build/deps/$ARCH"
WORK="$ROOT/build/deps/work-$ARCH"
[ -z "$CLEAN" ] || rm -rf "$PREFIX" "$WORK"
mkdir -p "$SRC" "$PREFIX" "$WORK"

TOOLS="meson ninja pkg-config cc"
for t in $TOOLS; do
  command -v "$t" >/dev/null || { echo "build-deps.sh: no $t (meson, ninja and pkg-config build these; they ship nothing)" >&2; exit 1; }
done

# name  version  tarball  sha256  url
PKGS='
pcre2 10.48 pcre2-10.48.tar.bz2 b6c68fdf6f3ac31388b50aa89ff0fc49c00c987c16e7b5146491d12003f2c8ed https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.48/pcre2-10.48.tar.bz2
glib 2.90.0 glib-2.90.0.tar.xz 17d15cac2af80a33271127408e0abc2748eb297c595c2a26409e81e14e7d1b8f https://download.gnome.org/sources/glib/2.90/glib-2.90.0.tar.xz
pixman 0.46.4 pixman-0.46.4.tar.gz d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c https://cairographics.org/releases/pixman-0.46.4.tar.gz
libslirp 4.9.5 libslirp-v4.9.5.tar.gz f43e68b60b580647574ec4a0e2b6c600a56281e6c39f79426510832dc810f483 https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.5/libslirp-v4.9.5.tar.gz
openssl 3.5.9 openssl-3.5.9.tar.gz 603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
libtpms 0.10.2 libtpms-0.10.2.tar.gz edac03680f8a4a1c5c1d609a10e3f41e1a129e38ff5158f0c8deaedc719fb127 https://github.com/stefanberger/libtpms/archive/refs/tags/v0.10.2.tar.gz
spice-protocol 0.14.5 spice-protocol-0.14.5.tar.xz baf58449f6e89d19f475899ad5fb9196fdc46c03cc53233f4e39cf2978f9cff7 https://www.spice-space.org/download/releases/spice-protocol-0.14.5.tar.xz
zstd 1.5.7 zstd-1.5.7.tar.gz eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3 https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
'
[ "$OS" = Darwin ] || PKGS=$(printf '%s\n' "$PKGS" | grep -E '^(pcre2|glib|libslirp|openssl|libtpms|spice-protocol) ')

# Our patches on a package: patches/deps/<name>/*.patch (git-format
# diffs, filename order; patches/deps/README.md). The set is named by a
# hash of the files, in the unpacked tree (`.patches`) and in the build
# stamp, so a changed set unpacks the tarball again and rebuilds that
# package alone. Empty when the package has none.
patchset() { # name -> hash or ""
  ls "$ROOT/patches/deps/$1"/*.patch >/dev/null 2>&1 || return 0
  cat "$ROOT/patches/deps/$1"/*.patch | shasum -a 256 | cut -c1-8
}

fetch() { # name tarball sha256 url -> the unpacked, patched source directory
  local name=$1 tar=$2 sha=$3 url=$4 dir set p
  if [ ! -f "$SRC/$tar" ]; then
    echo "==> fetch $url" >&2
    curl -fsSL -o "$SRC/$tar.part" "$url" && mv "$SRC/$tar.part" "$SRC/$tar"
  fi
  [ "$(shasum -a 256 "$SRC/$tar" | cut -d' ' -f1)" = "$sha" ] || {
    echo "build-deps.sh: $tar does not match its pinned sha256 ($sha); delete it to fetch again" >&2; exit 1; }
  dir=$(tar -tf "$SRC/$tar" | head -1 | cut -d/ -f1)
  set=$(patchset "$name")
  if [ -d "$SRC/$dir" ] && [ "$(cat "$SRC/$dir/.patches" 2>/dev/null)" != "$set" ]; then
    echo "==> $name: the patch set changed; unpacking $tar again" >&2
    rm -rf "$SRC/$dir"
  fi
  if [ ! -d "$SRC/$dir" ]; then
    tar -xf "$SRC/$tar" -C "$SRC"
    for p in "$ROOT/patches/deps/$name"/*.patch; do
      [ -f "$p" ] || continue
      echo "==> $name: patch ${p##*/}" >&2
      patch -p1 -s -N --no-backup-if-mismatch -d "$SRC/$dir" < "$p" >&2 || {
        echo "build-deps.sh: $name: ${p##*/} does not apply to $dir" >&2; exit 1; }
    done
    printf '%s\n' "$set" > "$SRC/$dir/.patches"
  fi
  echo "$SRC/$dir"
}

# Every build sees only our prefix and the SDK's own .pc files (libffi,
# for gio); never Homebrew's, or glib would take its gettext and QEMU its
# libpng. The compiler flags name the architecture and the floor, so a
# meson build of the other architecture gets a cross file saying the same
# in meson's terms (pixman picks its SIMD paths by host_machine.cpu_family).
# On Linux the distribution's own .pc files come after ours (zlib and
# libffi for gio and gobject, shared: neither links glib).
if [ "$OS" = Linux ]; then
  export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$(pkg-config --variable pc_path pkg-config)"
  FLAGS="-fPIC"
else
  export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SDK/usr/lib/pkgconfig"
  FLAGS="-arch $ARCH -mmacosx-version-min=$T"
fi
unset PKG_CONFIG_PATH
export CFLAGS="$FLAGS -O2 -I$PREFIX/include" CXXFLAGS="$FLAGS -O2" LDFLAGS="$FLAGS -L$PREFIX/lib"
export CC=cc CXX=c++
MESON=(meson setup --prefix="$PREFIX" --libdir=lib --buildtype=release --default-library=static -Db_staticpic=true)
if [ "$OS" = Darwin ] && { [ "$ARCH" != "$(uname -m)" ] || [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ]; }; then
  cat > "$WORK/cross.ini" <<INI
[binaries]
c = 'cc'
cpp = 'c++'
objc = 'cc'
objcpp = 'c++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T', '-I$PREFIX/include']
c_link_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T', '-L$PREFIX/lib']
cpp_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T']
cpp_link_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T']
objc_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T', '-I$PREFIX/include']
objc_link_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T', '-L$PREFIX/lib']
objcpp_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T']
objcpp_link_args = ['-arch', '$ARCH', '-mmacosx-version-min=$T']
[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = '$ARCH'
cpu = '$ARCH'
endian = 'little'
[properties]
needs_exe_wrapper = false
INI
  MESON+=(--cross-file "$WORK/cross.ini")
fi
say() { printf '\n\033[1m==> deps: %s\033[0m\n' "$*"; }
# A step's output goes to $WORK/<name>.log; a failure shows the log's
# tail and stops, so a configure that died is never a mystery.
run() {
  "$@" >> "$WORK/$name.log" 2>&1 || {
    echo "build-deps.sh: $name: '$1' failed; the last lines of $WORK/$name.log:" >&2
    tail -40 "$WORK/$name.log" >&2; exit 1; }
}

built=()
while read -r name ver tar sha url; do
  [ -n "$name" ] || continue
  set=$(patchset "$name")
  stamp="$PREFIX/.built-$name-$ver${set:+-p$set}-$T"
  if [ -f "$stamp" ]; then echo "    $name $ver${set:+ (patched $set)} built for $FOR $ARCH"; continue; fi
  src=$(fetch "$name" "$tar" "$sha" "$url")
  b="$WORK/$name"; : > "$WORK/$name.log"
  rm -rf "$b"
  say "$name $ver ($ARCH, $FOR)"
  case "$name" in
    pcre2)
      ( cd "$src" && run ./configure --prefix="$PREFIX" --disable-shared --enable-static \
          --disable-pcre2grep-libz --disable-pcre2grep-libbz2 --disable-pcre2test-libreadline \
          --enable-jit --quiet && run make -j8 && run make install && run make distclean ) ;;
    glib)
      # What QEMU uses and nothing that would need more of the system.
      # libffi (gobject) and libintl (a libc without ngettext) come from
      # the subprojects in glib's own tarball (subprojects/packagecache):
      # a stub libintl, since nothing here has a translation to load, and
      # both installed into the prefix as archives beside glib's.
      run "${MESON[@]}" "$b" "$src" -Dtests=false -Dintrospection=disabled -Dglib_debug=disabled \
        -Dman-pages=disabled -Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled \
        -Dselinux=disabled -Dlibmount=disabled -Dlibelf=disabled -Dnls=disabled \
        -Dxattr=false -Dglib_assert=false -Dglib_checks=false
      run ninja -C "$b" install ;;
    pixman)
      run "${MESON[@]}" "$b" "$src" -Dtests=disabled -Ddemos=disabled -Dgtk=disabled -Dlibpng=disabled \
        -Dopenmp=disabled
      run ninja -C "$b" install ;;
    libslirp)
      run "${MESON[@]}" "$b" "$src"
      run ninja -C "$b" install ;;
    openssl)
      # libcrypto for libtpms (the TPM 2.0 behind `-tpmdev libtpms`, track
      # M20). Static, no programs, no engines or loadable providers: the
      # default provider is compiled in. The LTS line (3.5).
      case "$OS-$ARCH" in
        Darwin-arm64) target=darwin64-arm64-cc ;;
        Darwin-x86_64) target=darwin64-x86_64-cc ;;
        *) target="" ;;
      esac
      ( cd "$src" && run ./Configure $target --prefix="$PREFIX" --libdir=lib \
          no-shared no-tests no-docs no-apps no-engine no-module no-dso \
          && run make -j8 build_libs && run make install_dev && run make distclean ) ;;
    libtpms)
      # The TPM 2.0 itself (IBM's, from the TCG reference code), linked
      # into QEMU by our backend (tpm/qemu/tpm_libtpms.c). The GitHub
      # tarball has no configure; autogen.sh makes it and runs it.
      ( cd "$src" && run ./autogen.sh --prefix="$PREFIX" --libdir="$PREFIX/lib" \
          --disable-shared --enable-static --with-openssl --with-tpm2 \
          && run make -j8 && run make install && run make distclean ) ;;
    spice-protocol)
      # Headers only, nothing linked: QEMU's `qemu-vdagent` chardev (the
      # clipboard, track M23) builds when they are there. The .pc goes to
      # share/pkgconfig, outside the prefix's search path, so it moves.
      run "${MESON[@]}" "$b" "$src"
      run ninja -C "$b" install
      mkdir -p "$PREFIX/lib/pkgconfig"
      mv -f "$PREFIX/share/pkgconfig/spice-protocol.pc" "$PREFIX/lib/pkgconfig/" ;;
    zstd)
      # The library alone: no programs, no shared build.
      run make -C "$src/lib" -j8 libzstd.a CFLAGS="$CFLAGS"
      run make -C "$src/lib" install-static install-includes install-pc PREFIX="$PREFIX" LIBDIR="$PREFIX/lib"
      run make -C "$src/lib" clean ;;
  esac
  rm -f "$PREFIX/.built-$name-$ver"*"-$T"   # the version's stamp with another patch set
  touch "$stamp"
  built+=("$name")
done <<< "$PKGS"

# Nothing shared may be left for a link to prefer over the archives.
rm -f "$PREFIX"/lib/*.dylib "$PREFIX"/lib/*.so "$PREFIX"/lib/*.so.*
# An archive-only prefix: every .pc file's private link line (what a
# static glib needs: pcre2, intl, iconv, ffi, the frameworks) becomes
# public, so `pkg-config --libs` answers the whole static link and no
# consumer has to ask for `--static`. QEMU's meson cannot be asked to
# (its prefer_static means `-static`, fatal on macOS).
python3 - "$PREFIX"/lib/pkgconfig/*.pc <<'PY'
import re, sys
for p in sys.argv[1:]:
    s = open(p).read()
    pub = {}
    for key in ("Libs", "Requires"):
        priv = re.search(r"^%s\.private:[ \t]*(.*)$" % key, s, re.M)
        if not priv or not priv.group(1).strip():
            continue
        m = re.search(r"^%s:[ \t]*(.*)$" % key, s, re.M)
        merged = ((m.group(1).strip() + " ") if m else "") + priv.group(1).strip()
        s = re.sub(r"^%s\.private:.*$\n?" % key, "", s, flags=re.M)
        if m:
            s = re.sub(r"^%s:.*$" % key, lambda _: "%s: %s" % (key, merged), s, count=1, flags=re.M)
        else:
            s = s.rstrip("\n") + "\n%s: %s\n" % (key, merged)
    open(p, "w").write(s)
PY
echo
echo "deps ($ARCH, $FOR): $PREFIX"
ls "$PREFIX/lib"/*.a | sed 's|.*/|    |'
[ ${#built[@]} -eq 0 ] || echo "    built now: ${built[*]}"
