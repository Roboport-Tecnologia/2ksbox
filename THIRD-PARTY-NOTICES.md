# Third-party notices

The packages are built from this project's own source plus the
components below. This file meets the attribution terms of their
licences (Apache-2.0 §4 in particular) and lets anyone who packages or
redistributes a build see what is in it.

"The packages" are four builds. **Linux** is the tarball
(`scripts/package-linux.sh`) and the Flatpak, which stages the same tree.
**macOS** is both Mac apps (`scripts/package-macos.sh`). **Windows** is
the zip and the Store's MSIX, which is the zip's tree
(`scripts/package-windows.sh`, `scripts/package-msix.sh`). An entry says
which packages carry it when that is not all of them.

## Where the licence texts are

Every package carries the texts this file names, in a `licenses`
directory beside this file:

| Package | Directory |
|---|---|
| Linux | `share/doc/2ksbox/licenses/` |
| macOS | `2ksbox.app/Contents/share/doc/2ksbox/licenses/` |
| Windows | `doc\licenses\` |

At its top are the generic texts: `GPL-2.0.txt` (this project's
`COPYING`), `LGPL-2.1.txt`, `GPL-3.0.txt`, `LGPL-3.0.txt`,
`Apache-2.0.txt`, `MIT.txt`, `Zlib.txt`, `BSL-1.0.txt` and `MPL-2.0.txt`.
Each component below has a directory of its own with its project's own
licence files, named in the entry as `licenses/<name>/`. Every Rust crate
has `licenses/crates/<name>-<version>/` with the licence files its
package carries. A crate whose package carries none gets a note there
naming its licence and the generic text.

`python3 tools/third-party-notices.py stage` fills the directory, and
each packager runs it. It fails when a file is missing, so a package
never ships without a text this file points at. Its table of components
is the same list as this file; change both together.

## Corresponding source

The source of every package is this repository at the release's tag,
with its submodules (`qemu`, `third_party/*`). The libraries that
`scripts/build-deps.sh` builds come from the tarballs it pins by URL and
sha256, with the patches in `patches/deps/`. `docs/development.md` has
the build. The About window links the tag of the version it is.

### Written offer

For at least three years after we last distribute a version of 2ksbox
in binary form, Roboport Tecnologia (David Rios Gomes Ltda) will give
anyone who asks the complete corresponding source code of that
version, for the parts under the GPL and the LGPL included, the
libraries that `scripts/build-deps.sh` downloads among them. We charge
no more than our cost of physically performing the copy. Write to
contato@roboport.com.br and name the version, which the About window
shows.

## QEMU

The players link `libqemu-embed-<target>`, which is QEMU
(https://www.qemu.org), **GPL-2.0-only** as a whole, plus this project's
patch queue in `patches/qemu/` and the qemu-3dfx overlay
(https://github.com/kjliew/qemu-3dfx, **GPL-2.0**). `qemu-img` is QEMU
too. QEMU's own licence files are in `licenses/qemu/`. The launcher does
not link QEMU.

The firmware in the packages' `pc-bios/` is QEMU's own, and
`pc-bios/README` names each file's project and licence. Two files are
not QEMU's builds: the VGA BIOSes `vgabios-stdvga.bin` and
`vgabios-cirrus.bin`. They are **SeaBIOS** (https://www.seabios.org,
**LGPL-3.0-only**, `licenses/LGPL-3.0.txt` with `licenses/GPL-3.0.txt`)
at the commit QEMU pins (`qemu/roms/seabios`), modified by
`patches/seabios/` and built by `scripts/build-vgabios.sh`. That tree
plus those patches is the corresponding source.

The Voodoo 2 device (`-device voodoo2`) links 86Box's Voodoo emulation
(https://github.com/86Box/86Box, **GPL-2.0-or-later**, derived from
Sarah Walker's PCem), copied verbatim into `voodoo/86box/`. The files
keep their authors' headers, and `voodoo/86box/UPSTREAM` names the
commit.

## Libraries linked into QEMU

`scripts/build-deps.sh` builds these from their release tarballs and
links them **statically** into `libqemu-embed` and `qemu-img`. On Linux
only GLib, PCRE2, libslirp, libcrypto and libtpms are linked this way;
the others are the distribution's shared libraries, which the tarball
does not carry.

| Library | Licence | Packages | Texts |
|---|---|---|---|
| **GLib** (https://gitlab.gnome.org/GNOME/glib) | LGPL-2.1-or-later | all | `licenses/glib/` |
| **PCRE2** (https://github.com/PCRE2Project/pcre2), GLib's regular expressions | BSD-3-Clause WITH PCRE2-exception | all | `licenses/pcre2/` |
| **libslirp** (https://gitlab.freedesktop.org/slirp/libslirp), guest networking | BSD-3-Clause AND MIT | all | `licenses/libslirp/` |
| **OpenSSL**'s libcrypto (https://www.openssl.org), for libtpms | Apache-2.0 | all | `licenses/openssl/` |
| **libtpms** (https://github.com/stefanberger/libtpms), Windows 11's TPM 2.0 | BSD-3-Clause and TCG | all | `licenses/libtpms/` |
| **spice-protocol** (https://gitlab.freedesktop.org/spice/spice-protocol), headers only, for the clipboard | BSD-3-Clause | all | `licenses/spice-protocol/` |
| **pixman** (https://pixman.org) | MIT | macOS, Windows | `licenses/pixman/` |
| **zstd** (https://github.com/facebook/zstd) | BSD-3-Clause | macOS | `licenses/zstd/` |
| **zlib** (https://zlib.net) | Zlib | Windows | `licenses/zlib/` |
| **libepoxy** (https://github.com/anholt/libepoxy), OpenGL dispatch | MIT | Windows | `licenses/libepoxy/` |
| **libffi** (https://sourceware.org/libffi/), from GLib's subproject | MIT | macOS, Windows | `licenses/libffi/` |
| **proxy-libintl** (https://github.com/frida/proxy-libintl), a gettext stub from GLib's subproject | LGPL-2.0-or-later | macOS, Windows | `licenses/proxy-libintl/` |

libtpms is IBM's code under BSD-3-Clause plus the TPM 2.0 reference
code under the Trusted Computing Group's own BSD-style terms; its
`LICENSE` has both.

**GLib and proxy-libintl are LGPL and linked statically.** Both are used
unmodified, from the tarballs `scripts/build-deps.sh` pins. What they are
linked into (`libqemu-embed`, `qemu-img`) is GPL-2.0, and its complete
source and build scripts are this repository. So a recipient can rebuild
it against a modified GLib (`scripts/build-deps.sh`, then
`scripts/build.sh`) and run the result, which is what LGPL-2.1 §6 asks
of a static link. The LGPL-2.1 text is `licenses/LGPL-2.1.txt`, and the
music engines below are covered the same way.

## Windows 11's firmware and drivers

The packages' `pc-bios/2ksbox-aarch64-code.fd` and `-vars.fd` (macOS,
Apple Silicon) and `pc-bios/2ksbox-x86_64-code.fd` and `-vars.fd`
(Windows) are **EDK2** (https://github.com/tianocore/edk2,
**BSD-2-Clause-Patent**) at the commit QEMU pins (`qemu/roms/edk2`),
modified by `patches/edk2/` and built by `scripts/build-edk2.sh`. That
tree plus those patches is the corresponding source. QEMU's own EDK2
builds are in `pc-bios/` too, and `licenses/edk2/` has QEMU's
`edk2-licenses.txt` for both.

The drivers discs in `share/2ksbox/drivers/` (`drivers\` on Windows;
x64 on Linux and Windows, ARM64 on Apple Silicon) carry three things:

- Drivers from **virtio-win**
  (https://github.com/virtio-win/kvm-guest-drivers-windows,
  **BSD-3-Clause**): Microsoft-signed binaries from its 0.1.302 ISO, cut
  by `scripts/build-virtio-win.sh`.
- This project's own build of virtio-win's display driver, viogpudo, with
  the patches in `patches/viogpudo/`, built by `scripts/build-viogpudo.sh`.
  Its resolution helpers replace the ISO's in `2ksbox\viogpudo`,
  and the driver itself, which Microsoft has not signed, is in
  `2ksbox\experimental-display` for test mode. Same licence and source
  repository as virtio-win's.
- The 2ksbox agent (`guest-agent/`, **GPL-2.0-or-later**), whose crates
  are listed below.

Each disc carries the ISO's own `virtio-win_license.txt`, and
`licenses/virtio-win/` has the same text.

## The music engines

The **OPL3, General MIDI and MT-32 engines** are crates linked into QEMU
through `libsynth/` (doc 20): `nuked-opl3` (**LGPL-2.1-or-later**, a Rust
port of Nuked-OPL3), `moont` (**LGPL-2.1-or-later**, a Rust port of Munt's
CM-32L) and `rustysynth` (**MIT**). The two LGPL crates are used
unmodified, and their sources are the published crates. A recipient may
relink against their own copies, which is what the LGPL's §6 asks of a
static link, and everything they are linked into is GPL-2.0 source in
this repository.

## Direct3D and Vulkan

- **DXVK** (https://github.com/doitsujin/dxvk, **Zlib**), built as
  `libdxvk_d3d9` by `scripts/configure-dxvk.sh` with the patch queue in
  `patches/dxvk/`. The Direct3D executor (doc 14) loads it at run time.
  All packages; `licenses/dxvk/`.
- The **Vulkan loader** (https://github.com/KhronosGroup/Vulkan-Loader,
  **Apache-2.0**, `licenses/Apache-2.0.txt`) and **KosmicKrisp**, Mesa's
  Vulkan driver for macOS (https://mesa3d.org, **MIT**,
  `licenses/mesa/`), both as the LunarG Vulkan SDK builds them. macOS on
  Apple Silicon only. Linux and Windows use the system's Vulkan.
- The executor's Windows build for Wine (`lib/2ksbox/wine/`, Linux and
  the macOS community build) links **mingw-w64**'s runtime and
  winpthreads statically (ZPL-2.1, MIT and BSD terms;
  `licenses/mingw-w64/`).

## The launcher's installer reader

- **inno** (https://github.com/russellbanks/inno, **MIT OR Apache-2.0**)
  0.7.6, vendored in `third_party/inno` with two changes (a file entry
  keeps its conditions, and upstream's later bounded string reads;
  `third_party/inno/README-2ksbox.md`). The
  launcher reads a game's installer with it (the disc shelf's Game
  installer…). All packages; `licenses/inno/`.

## The guest-tools disc

The guest-tools ISO in `share/2ksbox/guest-tools/` (`guest-tools\` on
Windows) is this project's guest code, plus:

- qemu-3dfx's guest wrappers (**GPL-2.0**, `licenses/qemu-3dfx/`): the
  per-game `OPENGL\OPENGL32.DLL` and the device mapper in `MAPPER\`,
  built by `guest-tools/build-wrappers.sh` from `third_party/qemu-3dfx`.
- `DOSMODE\UIDE.SYS`, Jack R. Ellis's DOS CD-ROM driver as FreeDOS 1.4
  packages it (`third_party/uide`). Its FreeDOS listing says "Free
  w/sources", so its source, `DOSMODE\UIDE.ASM`, is on the disc beside
  it; `licenses/uide/` has the listing and where it came from.
- mingw-w64's runtime, static in every guest program (`licenses/mingw-w64/`).

## The General MIDI bank

`soundfonts/TimGM6mb.sf2`, **GPL-2**, by Tim Brechbill (2004) with later
work by David Bolton (2010). MuseScore 0.9.6–1.3 shipped it, and Debian
packages it as `timgm6mb-soundfont`. This copy is byte for byte
Debian's `timgm6mb-soundfont_1.3.orig.tar.gz` one (sha256
`c5378b62028c920cb11e4803327983fee2f2cdff5dc89c708e39da417e51c854`). It is
what the General MIDI synthesizer plays through unless the user names
another bank, and it is redistributed under the same GPLv2 as the rest of
this package (`COPYING`). `soundfonts/README.md` says why this bank.

Nothing of Roland's is in this repository. The MT-32 / CM-32L option needs
the user's own ROM images (doc 20 §4).

## Shader presets

The launcher can download libretro's `slang-shaders`
(https://github.com/libretro/slang-shaders) at the user's request, and the
`third_party/slang-shaders` submodule is the same collection. Those presets
carry their own per-file licences. A Linux or Windows package built with
`--with-shaders` redistributes them unmodified in `share/2ksbox/shaders/`
(`shaders\` on Windows), each file with its own licence header. The Mac
apps and the packages built without that option do not carry them.

## The Flatpak's add-ons

The Flatpak's two add-ons are built from other projects' release
sources, declared with checksums in their manifests: Qt 6 and KDE
Frameworks for the Kirigami launcher
(`packaging/flatpak/com._2ksbox.Launcher.KDE.yml`, LGPL-3.0 and
LGPL-2.1 for the most part), and **Wine** for the Direct3D executor
below the Vulkan floor (`com._2ksbox.Launcher.Wine.yml`,
**LGPL-2.1-or-later**). Each module's licence files are its own source's.

## Rust crates

Every Rust binary links Rust's standard library (**MIT OR Apache-2.0**).
The crates each binary links are listed below, by declared licence; the
ones marked **Apache-2.0** alone have no alternative licence. None of
those ships a `NOTICE` file, so attribution here and the licence text
are the whole of the obligation. `mitsuami` (https://crates.io/crates/mitsuami,
**MIT OR Apache-2.0**) is the launcher's and the player's toolkit.

For how the Apache-2.0-only crates interact with the player's
GPL-2.0-only status, see **ADR-010** in `docs/10-decisions.md`. The
launcher and `shader-chain` are `GPL-2.0-or-later` for that reason
(**ADR-009**).

## Crates, by declared licence

Generated by `python3 tools/third-party-notices.py list --write THIRD-PARTY-NOTICES.md` for `x86_64-unknown-linux-gnu`, `aarch64-unknown-linux-gnu`, `aarch64-apple-darwin`, `x86_64-apple-darwin`, `x86_64-pc-windows-msvc` (`guest-agent`: Windows). Each crate's own licence files are in the packages' `licenses/crates/`.

### `libdisc` and `libsynth`, linked into QEMU: 29 third-party crates

**MIT OR Apache-2.0** (11): `bitflags`, `lazy_static`, `log`, `num-complex`, `num-traits`, `proc-macro2`, `quote`, `regex-lite`, `serde_core`, `smallvec`, `syn`

**MPL-2.0** (10): `symphonia`, `symphonia-bundle-flac`, `symphonia-bundle-mp3`, `symphonia-codec-pcm`, `symphonia-codec-vorbis`, `symphonia-common`, `symphonia-core`, `symphonia-format-ogg`, `symphonia-format-riff`, `symphonia-metadata`

**MIT** (3): `extended`, `libm`, `rustysynth`

**LGPL-2.1-or-later** (2): `moont`, `nuked-opl3`

**Zlib OR Apache-2.0 OR MIT** (2): `bytemuck`, `bytemuck_derive`

**(MIT OR Apache-2.0) AND Unicode-3.0** (1): `unicode-ident`

### `launcher-mitsuami`, the launcher: 311 third-party crates

**MIT OR Apache-2.0** (155): `allocator-api2`, `arc-swap`, `arrayvec`, `as-raw-xcb-connection`, `ash`, `base64`, `bitflags`, `block-buffer`, `cfg-if`, `chacha20`, `chrono`, `cpufeatures`, `crc`, `crc-catalog`, `crc32fast`, `crossbeam-deque`, `crossbeam-epoch`, `crossbeam-utils`, `crypto-common`, `deranged`, `digest`, `directories`, `dirs-next`, `dirs-sys`, `dirs-sys-next`, `displaydoc`, `document-features`, `either`, `errno`, `fdeflate`, `field-offset`, `fixedbitset`, `flate2`, `futures-channel`, `futures-core`, `futures-executor`, `futures-io`, `futures-macro`, `futures-task`, `futures-util`, `getrandom`, `glslang`, `glslang-sys`, `gpu-allocator`, `half`, `hashbrown`, `heck`, `http`, `httparse`, `hybrid-array`, `image`, `itertools`, `itoa`, `libc`, `litrs`, `lock_api`, `log`, `md-5`, `mitsuami`, `mitsuami-appkit`, `mitsuami-core`, `mitsuami-gtk`, `mitsuami-kirigami`, `mitsuami-linux`, `mitsuami-macros`, `mitsuami-reactive`, `mitsuami-widgets`, `mitsuami-winui`, `naga`, `naga-types`, `num-conv`, `num-derive`, `num-traits`, `once_cell`, `parking_lot`, `parking_lot_core`, `percent-encoding`, `petgraph`, `png`, `powerfmt`, `presser`, `proc-macro-crate`, `proc-macro2`, `profiling`, `quote`, `rand`, `rand_core`, `range-alloc`, `raw-window-metal`, `rayon`, `rayon-core`, `regex`, `regex-automata`, `regex-syntax`, `renderdoc-sys`, `rustls-pki-types`, `rustversion`, `scopeguard`, `serde`, `serde_core`, `serde_derive`, `serde_json`, `serde_spanned`, `sha1`, `sha2`, `simdutf8`, `smallvec`, `spirv-cross-sys`, `spirv-cross2`, `spirv-cross2-derive`, `static_assertions`, `syn`, `tar`, `thiserror`, `thiserror-impl`, `time`, `time-core`, `toml`, `toml_datetime`, `toml_edit`, `toml_parser`, `toml_writer`, `typenum`, `unic-langid`, `unic-langid-impl`, `unicode-segmentation`, `unicode-width`, `unty`, `ureq`, `ureq-proto`, `utf8-zero`, `wgpu`, `wgpu-core`, `wgpu-core-deps-apple`, `wgpu-core-deps-windows-linux-android`, `wgpu-hal`, `wgpu-naga-bridge`, `wgpu-types`, `windows`, `windows-collections`, `windows-core`, `windows-future`, `windows-implement`, `windows-interface`, `windows-link`, `windows-numerics`, `windows-reference`, `windows-result`, `windows-strings`, `windows-sys`, `windows-threading`, `windows-time`, `x11rb`, `x11rb-protocol`, `xattr`

**MIT** (55): `array-concat`, `bincode`, `bincode_derive`, `block2`, `bytes`, `cairo-rs`, `cairo-sys-rs`, `data-encoding`, `dlib`, `gdk-pixbuf`, `gdk-pixbuf-sys`, `gdk4`, `gdk4-sys`, `gio`, `gio-sys`, `glib`, `glib-macros`, `glib-sys`, `gobject-sys`, `graphene-rs`, `graphene-sys`, `gsk4`, `gsk4-sys`, `gtk4`, `gtk4-macros`, `gtk4-sys`, `libadwaita`, `libadwaita-sys`, `libm`, `memoffset`, `nom`, `nom_locate`, `objc2`, `objc2-encode`, `objc2-foundation`, `ordered-float`, `pango`, `pango-sys`, `platform-dirs`, `quick-xml`, `simd-adler32`, `slab`, `strumbra`, `taffy`, `unsigned-varint`, `vec_extract_if_polyfill`, `virtue`, `wayland-backend`, `wayland-client`, `wayland-protocols`, `wayland-scanner`, `wayland-sys`, `winnow`, `zigzag`, `zmij`

**Zlib OR Apache-2.0 OR MIT** (17): `bytemuck`, `bytemuck_derive`, `dispatch2`, `objc2-app-kit`, `objc2-cloud-kit`, `objc2-core-data`, `objc2-core-foundation`, `objc2-core-graphics`, `objc2-core-image`, `objc2-core-text`, `objc2-core-video`, `objc2-game-controller`, `objc2-io-surface`, `objc2-metal`, `objc2-quartz-core`, `objc2-quick-look-thumbnailing`, `objc2-uniform-type-identifiers`

**Apache-2.0 OR MIT** (16): `bit-set`, `bit-vec`, `codepage`, `const-oid`, `dos-date-time`, `equivalent`, `fluent-bundle`, `fluent-langneg`, `fluent-syntax`, `indexmap`, `intl-memoizer`, `multiversion_no_op`, `nt-time`, `pin-project-lite`, `rustc-hash`, `zeroize`

**MIT/Apache-2.0** (9): `core_detect`, `downcast-rs`, `filetime`, `fs2`, `khronos-egl`, `linked-hash-map`, `scoped-tls`, `type-map`, `winapi`

**MPL-2.0 OR GPL-3.0-only** (9): `librashader`, `librashader-cache`, `librashader-common`, `librashader-pack`, `librashader-preprocess`, `librashader-presets`, `librashader-reflect`, `librashader-runtime`, `librashader-runtime-wgpu`

**Unlicense OR MIT** (7): `aho-corasick`, `byteorder-lite`, `jiff`, `jiff-core`, `memchr`, `termcolor`, `winapi-util`

**Apache-2.0** (6): `codespan-reporting`, `gethostname`, `glutin_wgl_sys`, `lzma-rust2`, `rspirv`, `spirv`

**Apache-2.0/MIT** (5): `bytecount`, `halfbrown`, `intl_pluralrules`, `pollster`, `rustc-hash`

**MIT OR Apache-2.0 OR Zlib** (4): `glow`, `raw-window-handle`, `zune-core`, `zune-jpeg`

**ISC** (3): `libloading`, `rustls-webpki`, `untrusted`

**Unicode-3.0** (3): `tinystr`, `zerofrom`, `zerovec`

**Zlib** (3): `foldhash`, `slotmap`, `zlib-rs`

**Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT** (2): `linux-raw-sys`, `rustix`

**BSD-2-Clause OR Apache-2.0 OR MIT** (2): `zerocopy`, `zerocopy-derive`

**BSD-3-Clause OR Apache-2.0** (2): `moxcms`, `pxfm`

**MPL-2.0** (2): `option-ext`, `persy`

**(Apache-2.0 OR MIT) AND BSD-3-Clause** (1): `encoding_rs`

**(MIT OR Apache-2.0) AND Unicode-3.0** (1): `unicode-ident`

**0BSD OR MIT OR Apache-2.0** (1): `adler2`

**Apache-2.0 AND ISC** (1): `ring`

**Apache-2.0 OR GPL-2.0-only** (1): `self_cell`

**Apache-2.0 OR ISC OR MIT** (1): `rustls`

**BSD-3-Clause** (1): `subtle`

**CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception** (1): `blake3`

**CC0-1.0 OR MIT-0 OR Apache-2.0** (1): `constant_time_eq`

**CDLA-Permissive-2.0** (1): `webpki-roots`

**MIT OR Zlib OR Apache-2.0** (1): `miniz_oxide`

**MPL-2.0+** (1): `smartstring`

### `player-mitsuami`, the player: 340 third-party crates

**MIT OR Apache-2.0** (162): `aes`, `allocator-api2`, `arboard`, `arc-swap`, `arrayvec`, `as-raw-xcb-connection`, `ash`, `bitflags`, `block-buffer`, `cfg-if`, `chacha20`, `chrono`, `cipher`, `cmac`, `cpufeatures`, `crc`, `crc-catalog`, `crc32fast`, `crossbeam-deque`, `crossbeam-epoch`, `crossbeam-utils`, `crypto-common`, `dasp_sample`, `dbl`, `deranged`, `digest`, `directories`, `dirs-next`, `dirs-sys`, `dirs-sys-next`, `displaydoc`, `document-features`, `either`, `errno`, `fdeflate`, `field-offset`, `fixedbitset`, `flate2`, `futures-channel`, `futures-core`, `futures-executor`, `futures-io`, `futures-macro`, `futures-task`, `futures-util`, `getrandom`, `glslang`, `glslang-sys`, `gpu-allocator`, `half`, `hashbrown`, `heck`, `hmac`, `hybrid-array`, `image`, `inout`, `itertools`, `itoa`, `libc`, `litrs`, `lock_api`, `log`, `md-5`, `md4`, `mitsuami`, `mitsuami-appkit`, `mitsuami-core`, `mitsuami-gtk`, `mitsuami-kirigami`, `mitsuami-linux`, `mitsuami-macros`, `mitsuami-reactive`, `mitsuami-widgets`, `mitsuami-winui`, `naga`, `naga-types`, `notify-types`, `num-conv`, `num-derive`, `num-traits`, `once_cell`, `parking_lot`, `parking_lot_core`, `percent-encoding`, `petgraph`, `png`, `powerfmt`, `presser`, `proc-macro-crate`, `proc-macro2`, `profiling`, `quote`, `rand`, `rand_core`, `range-alloc`, `raw-window-metal`, `rayon`, `rayon-core`, `regex`, `regex-automata`, `regex-syntax`, `renderdoc-sys`, `rustversion`, `scopeguard`, `serde`, `serde_core`, `serde_derive`, `serde_json`, `serde_spanned`, `sha1`, `sha2`, `simdutf8`, `smallvec`, `spirv-cross-sys`, `spirv-cross2`, `spirv-cross2-derive`, `static_assertions`, `syn`, `tar`, `thiserror`, `thiserror-impl`, `time`, `time-core`, `toml`, `toml_datetime`, `toml_edit`, `toml_parser`, `toml_writer`, `typenum`, `unic-langid`, `unic-langid-impl`, `unicode-segmentation`, `unicode-width`, `unty`, `wgpu`, `wgpu-core`, `wgpu-core-deps-apple`, `wgpu-core-deps-windows-linux-android`, `wgpu-hal`, `wgpu-naga-bridge`, `wgpu-types`, `windows`, `windows-collections`, `windows-core`, `windows-future`, `windows-implement`, `windows-interface`, `windows-link`, `windows-numerics`, `windows-reference`, `windows-result`, `windows-strings`, `windows-sys`, `windows-targets`, `windows-threading`, `windows-time`, `windows_x86_64_gnu`, `windows_x86_64_msvc`, `wl-clipboard-rs`, `x11rb`, `x11rb-protocol`, `xattr`

**MIT** (63): `alsa-sys`, `array-concat`, `bincode`, `bincode_derive`, `block2`, `cairo-rs`, `cairo-sys-rs`, `data-encoding`, `dlib`, `fsevent-sys`, `gdk-pixbuf`, `gdk-pixbuf-sys`, `gdk4`, `gdk4-sys`, `generic-array`, `gio`, `gio-sys`, `glib`, `glib-macros`, `glib-sys`, `gobject-sys`, `graphene-rs`, `graphene-sys`, `gsk4`, `gsk4-sys`, `gtk4`, `gtk4-macros`, `gtk4-sys`, `libadwaita`, `libadwaita-sys`, `libm`, `libudev-sys`, `memoffset`, `mio`, `nix`, `nom`, `nom_locate`, `objc2`, `objc2-encode`, `objc2-foundation`, `ordered-float`, `os_pipe`, `pango`, `pango-sys`, `platform-dirs`, `quick-xml`, `simd-adler32`, `slab`, `strumbra`, `taffy`, `tree_magic_mini`, `unsigned-varint`, `vec_extract_if_polyfill`, `virtue`, `wayland-backend`, `wayland-client`, `wayland-protocols`, `wayland-protocols-wlr`, `wayland-scanner`, `wayland-sys`, `winnow`, `zigzag`, `zmij`

**Zlib OR Apache-2.0 OR MIT** (21): `bytemuck`, `bytemuck_derive`, `dispatch2`, `objc2-app-kit`, `objc2-audio-toolbox`, `objc2-cloud-kit`, `objc2-core-audio`, `objc2-core-audio-types`, `objc2-core-data`, `objc2-core-foundation`, `objc2-core-graphics`, `objc2-core-image`, `objc2-core-text`, `objc2-core-video`, `objc2-game-controller`, `objc2-io-kit`, `objc2-io-surface`, `objc2-metal`, `objc2-quartz-core`, `objc2-quick-look-thumbnailing`, `objc2-uniform-type-identifiers`

**Apache-2.0 OR MIT** (16): `bit-set`, `bit-vec`, `codepage`, `const-oid`, `dos-date-time`, `equivalent`, `fluent-bundle`, `fluent-langneg`, `fluent-syntax`, `indexmap`, `intl-memoizer`, `multiversion_no_op`, `nt-time`, `pin-project-lite`, `rustc-hash`, `uuid`

**MIT/Apache-2.0** (11): `core_detect`, `coreaudio-rs`, `downcast-rs`, `filetime`, `fs2`, `khronos-egl`, `linked-hash-map`, `scoped-tls`, `type-map`, `vec_map`, `winapi`

**MPL-2.0 OR GPL-3.0-only** (9): `librashader`, `librashader-cache`, `librashader-common`, `librashader-pack`, `librashader-preprocess`, `librashader-presets`, `librashader-reflect`, `librashader-runtime`, `librashader-runtime-wgpu`

**Apache-2.0/MIT** (8): `alsa`, `bytecount`, `gilrs`, `gilrs-core`, `halfbrown`, `intl_pluralrules`, `pollster`, `rustc-hash`

**Apache-2.0** (7): `codespan-reporting`, `cpal`, `gethostname`, `glutin_wgl_sys`, `lzma-rust2`, `rspirv`, `spirv`

**Unlicense OR MIT** (7): `aho-corasick`, `byteorder-lite`, `jiff`, `jiff-core`, `memchr`, `termcolor`, `winapi-util`

**MIT OR Apache-2.0 OR Zlib** (4): `glow`, `raw-window-handle`, `zune-core`, `zune-jpeg`

**ISC** (3): `inotify`, `inotify-sys`, `libloading`

**Unicode-3.0** (3): `tinystr`, `zerofrom`, `zerovec`

**Zlib** (3): `foldhash`, `slotmap`, `zlib-rs`

**Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT** (2): `linux-raw-sys`, `rustix`

**BSD-2-Clause OR Apache-2.0 OR MIT** (2): `zerocopy`, `zerocopy-derive`

**BSD-3-Clause OR Apache-2.0** (2): `moxcms`, `pxfm`

**BSL-1.0** (2): `clipboard-win`, `error-code`

**MPL-2.0** (2): `option-ext`, `persy`

**Unlicense/MIT** (2): `same-file`, `walkdir`

**(Apache-2.0 OR MIT) AND BSD-3-Clause** (1): `encoding_rs`

**(MIT OR Apache-2.0) AND Unicode-3.0** (1): `unicode-ident`

**0BSD OR MIT OR Apache-2.0** (1): `adler2`

**Apache-2.0 / MIT** (1): `fnv`

**Apache-2.0 OR GPL-2.0-only** (1): `self_cell`

**BSD-2-Clause OR MIT OR Apache-2.0** (1): `mach2`

**BSD-3-Clause** (1): `subtle`

**CC0-1.0** (1): `notify`

**CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception** (1): `blake3`

**CC0-1.0 OR MIT-0 OR Apache-2.0** (1): `constant_time_eq`

**MIT OR Zlib OR Apache-2.0** (1): `miniz_oxide`

**MPL-2.0+** (1): `smartstring`

### `guest-agent`, on Windows 11's drivers disc: 3 third-party crates

**MIT OR Apache-2.0** (3): `windows-sys`, `windows-targets`, `windows_x86_64_gnu`
