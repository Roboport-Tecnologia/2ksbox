# Third-party notices

The binaries this project ships are built from its own source plus the
crates listed below and, for the player, QEMU itself. This file meets
the attribution terms of those licences (Apache-2.0 §4 in particular)
and lets anyone packaging or redistributing a build see what is in it
without resolving the dependency graph.

The crate listings are derived, not hand-maintained. Regenerate them
with `cargo metadata`:

```sh
cargo metadata --format-version 1 > /tmp/meta.json
python3 tools/third-party-notices.py /tmp/meta.json player
(cd launcher-mitsuami && cargo metadata --format-version 1) > /tmp/meta-mitsuami.json
python3 tools/third-party-notices.py /tmp/meta-mitsuami.json launcher-mitsuami
```

## QEMU

The **player** links `libqemu-embed-<target>`, which is QEMU
(https://www.qemu.org), **GPL-2.0-only** as a whole, plus this project's patch queue in
`patches/qemu/` and the qemu-3dfx overlay
(https://github.com/kjliew/qemu-3dfx). QEMU's own licence text is in
`qemu/LICENSE`, and the GPLv2 text this project distributes under is
`COPYING`. The **launcher** does not link QEMU.

The firmware in the packages' `pc-bios/` is QEMU's own, except the two
VGA BIOSes `vgabios-stdvga.bin` and `vgabios-cirrus.bin`. Those are
**SeaBIOS** (https://www.seabios.org, **LGPL-3.0-only**) at the commit QEMU
pins
(`qemu/roms/seabios`), modified by `patches/seabios/` and built by
`scripts/build-vgabios.sh`. That tree plus those patches is the
corresponding source.

The Voodoo 2 device (`-device voodoo2`) links 86Box's Voodoo emulation
(https://github.com/86Box/86Box, **GPL-2.0-or-later**, derived from
Sarah Walker's PCem), copied verbatim into `voodoo/86box/`. The files
keep their authors' headers, and `voodoo/86box/UPSTREAM` names the
commit.

## Windows 11's TPM, firmware and drivers

QEMU links two libraries statically for the TPM 2.0 behind
`-tpmdev libtpms` (track M20), built from pinned sources by
`scripts/build-deps.sh`: **libtpms** 0.10.2
(https://github.com/stefanberger/libtpms, BSD-style; its `LICENSE`) and
OpenSSL 3.5.9's **libcrypto** (https://www.openssl.org, **Apache-2.0**).

The macOS app's `pc-bios/2ksbox-aarch64-code.fd` and `-vars.fd` are
**EDK2** (https://github.com/tianocore/edk2, **BSD-2-Clause-Patent**) at
the commit QEMU pins (`qemu/roms/edk2`), modified by `patches/edk2/` and
built by `scripts/build-edk2.sh`. That tree plus those patches is the
corresponding source.

The drivers discs in `share/2ksbox/drivers/` carry drivers from
**virtio-win** (https://github.com/virtio-win/kvm-guest-drivers-windows,
**BSD-3-Clause**), Microsoft-signed binaries from its 0.1.302 ISO, cut by
`scripts/build-virtio-win.sh`. Each disc carries the ISO's own
`virtio-win_license.txt` beside them.

## The music engines

The **OPL3, General MIDI and MT-32 engines** are crates linked into QEMU
through `libsynth/` (doc 20): `nuked-opl3` (**LGPL-2.1-or-later**, a Rust
port of Nuked-OPL3), `moont` (**LGPL-2.1-or-later**, a Rust port of Munt's
CM-32L) and `rustysynth` (**MIT**). The two LGPL crates are used
unmodified, and their sources are the published crates. A recipient may
relink against their own copies, which is what the LGPL's §6 asks of a
static link, and everything they are linked into is GPL-2.0 source in
this repository.

## Host-side libraries built from `third_party/`

One library is built from vendored source and shipped beside the player.
It is not linked into anything; QEMU `dlopen`s it at run time.

- **DXVK** (https://github.com/doitsujin/dxvk), **zlib/libpng**, built as
  `libdxvk_d3d9` by `scripts/configure-dxvk.sh` with the patch queue in
  `patches/dxvk/`. It is the host executor of the paravirtual Direct3D
  device (doc 14). Its licence text is `third_party/dxvk/LICENSE`.

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
carry their own per-file licences and are neither modified nor redistributed
by this project.

## Apache-2.0 components

Several crates are **Apache-2.0 with no alternative licence**. In the
player they are `winit`, `cpal`, `ab_glyph`, `ab_glyph_rasterizer`,
`owned_ttf_parser`, `codespan-reporting`, `rspirv`, `spirv`, `gethostname`,
`glutin_wgl_sys`, `gl_generator`, `khronos_api`; the launcher adds
`ring` (`Apache-2.0 AND ISC`). `dpi` is `Apache-2.0 AND MIT`, and both
apply. None of them ships a `NOTICE` file, so attribution here is the
whole of the obligation.

For how those interact with the player's GPL-2.0-only status, see
**ADR-010** in `docs/10-decisions.md`. The launcher and `shader-chain` are
`GPL-2.0-or-later` for that reason (**ADR-009**).

## Crates, by declared licence

### `player`: 340 third-party crates

**MIT OR Apache-2.0** (151): `ahash`, `allocator-api2`, `android-activity`, `android_system_properties`, `arc-swap`, `arrayvec`, `as-raw-xcb-connection`, `ash`, `bitflags`, `bumpalo`, `cc`, `cfg-if`, `chacha20`, `core-foundation`, `core-foundation-sys`, `core-graphics`, `core-graphics-types`, `cpufeatures`, `crc`, `crc-catalog`, `crc32fast`, `crossbeam-deque`, `crossbeam-epoch`, `crossbeam-utils`, `dasp_sample`, `dirs-next`, `dirs-sys-next`, `document-features`, `either`, `errno`, `fdeflate`, `find-msvc-tools`, `fixedbitset`, `flate2`, `futures-core`, `futures-task`, `futures-util`, `getrandom`, `glob`, `glslang`, `glslang-sys`, `gpu-allocator`, `half`, `hashbrown`, `hermit-abi`, `image`, `itoa`, `jni`, `jni-macros`, `jni-sys`, `jni-sys-macros`, `jobserver`, `js-sys`, `libc`, `litrs`, `lock_api`, `log`, `memmap2`, `naga`, `naga-types`, `ndk`, `ndk-context`, `ndk-sys`, `num-derive`, `num-traits`, `once_cell`, `parking_lot`, `parking_lot_core`, `percent-encoding`, `petgraph`, `pkg-config`, `png`, `presser`, `proc-macro-crate`, `proc-macro2`, `profiling`, `quote`, `rand`, `rand_core`, `range-alloc`, `raw-window-metal`, `rayon`, `rayon-core`, `regex`, `regex-automata`, `regex-syntax`, `renderdoc-sys`, `rustc_version`, `rustversion`, `scopeguard`, `semver`, `serde`, `serde_core`, `serde_derive`, `serde_json`, `shlex`, `simdutf8`, `smallvec`, `smol_str`, `spirv-cross-sys`, `spirv-cross2`, `spirv-cross2-derive`, `static_assertions`, `syn`, `thiserror`, `thiserror-impl`, `toml_datetime`, `toml_edit`, `toml_parser`, `ttf-parser`, `unicode-segmentation`, `unicode-width`, `unty`, `wasm-bindgen`, `wasm-bindgen-futures`, `wasm-bindgen-macro`, `wasm-bindgen-macro-support`, `wasm-bindgen-shared`, `web-sys`, `web-time`, `wgpu`, `wgpu-core`, `wgpu-core-deps-apple`, `wgpu-core-deps-emscripten`, `wgpu-core-deps-windows-linux-android`, `wgpu-hal`, `wgpu-naga-bridge`, `wgpu-types`, `windows`, `windows-collections`, `windows-core`, `windows-future`, `windows-implement`, `windows-interface`, `windows-link`, `windows-numerics`, `windows-result`, `windows-strings`, `windows-sys`, `windows-targets`, `windows-threading`, `windows_aarch64_gnullvm`, `windows_aarch64_msvc`, `windows_i686_gnu`, `windows_i686_gnullvm`, `windows_i686_msvc`, `windows_x86_64_gnu`, `windows_x86_64_gnullvm`, `windows_x86_64_msvc`, `x11rb`, `x11rb-protocol`

**MIT** (71): `alsa-sys`, `android-properties`, `array-concat`, `bincode`, `bincode_derive`, `block2`, `bytes`, `calloop`, `calloop-wayland-source`, `cfg_aliases`, `combine`, `crunchy`, `data-encoding`, `dispatch`, `dlib`, `libm`, `libredox`, `libudev-sys`, `nix`, `nom`, `nom_locate`, `objc-sys`, `objc2`, `objc2-app-kit`, `objc2-cloud-kit`, `objc2-contacts`, `objc2-core-data`, `objc2-core-image`, `objc2-core-location`, `objc2-encode`, `objc2-foundation`, `objc2-link-presentation`, `objc2-metal`, `objc2-quartz-core`, `objc2-symbols`, `objc2-ui-kit`, `objc2-uniform-type-identifiers`, `objc2-user-notifications`, `orbclient`, `ordered-float`, `platform-dirs`, `quick-xml`, `redox_syscall`, `redox_users`, `sctk-adwaita`, `simd-adler32`, `slab`, `smithay-client-toolkit`, `strict-num`, `strumbra`, `tracing`, `tracing-core`, `unsigned-varint`, `vec_extract_if_polyfill`, `virtue`, `wayland-backend`, `wayland-client`, `wayland-csd-frame`, `wayland-cursor`, `wayland-protocols`, `wayland-protocols-plasma`, `wayland-protocols-wlr`, `wayland-scanner`, `wayland-sys`, `winnow`, `x11-dl`, `xcursor`, `xkbcommon-dl`, `xml-rs`, `zigzag`, `zmij`

**Apache-2.0 OR MIT** (16): `atomic-waker`, `autocfg`, `bit-set`, `bit-vec`, `concurrent-queue`, `equivalent`, `indexmap`, `pin-project`, `pin-project-internal`, `pin-project-lite`, `polling`, `portable-atomic`, `portable-atomic-util`, `rustc-hash`, `simd_cesu8`, `uuid`

**MIT/Apache-2.0** (16): `bitflags`, `coreaudio-rs`, `downcast-rs`, `foreign-types`, `foreign-types-macros`, `foreign-types-shared`, `fs2`, `khronos-egl`, `linked-hash-map`, `plain`, `scoped-tls`, `vec_map`, `version_check`, `winapi`, `winapi-i686-pc-windows-gnu`, `winapi-x86_64-pc-windows-gnu`

**Zlib OR Apache-2.0 OR MIT** (13): `bytemuck`, `bytemuck_derive`, `dispatch2`, `objc2-audio-toolbox`, `objc2-avf-audio`, `objc2-core-audio`, `objc2-core-audio-types`, `objc2-core-foundation`, `objc2-core-graphics`, `objc2-io-kit`, `objc2-io-surface`, `objc2-metal`, `objc2-quartz-core`

**Apache-2.0** (12): `ab_glyph`, `ab_glyph_rasterizer`, `codespan-reporting`, `cpal`, `gethostname`, `gl_generator`, `glutin_wgl_sys`, `khronos_api`, `owned_ttf_parser`, `rspirv`, `spirv`, `winit`

**MPL-2.0 OR GPL-3.0-only** (9): `librashader`, `librashader-cache`, `librashader-common`, `librashader-pack`, `librashader-preprocess`, `librashader-presets`, `librashader-reflect`, `librashader-runtime`, `librashader-runtime-wgpu`

**Apache-2.0/MIT** (7): `alsa`, `bytecount`, `gilrs`, `gilrs-core`, `halfbrown`, `pollster`, `rustc-hash`

**MIT OR Apache-2.0 OR Zlib** (6): `cursor-icon`, `glow`, `raw-window-handle`, `xkeysym`, `zune-core`, `zune-jpeg`

**Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT** (5): `linux-raw-sys`, `rustix`, `wasi`, `wasip2`, `wit-bindgen`

**Unlicense OR MIT** (5): `aho-corasick`, `byteorder-lite`, `memchr`, `termcolor`, `winapi-util`

**ISC** (3): `inotify`, `inotify-sys`, `libloading`

**Zlib** (3): `foldhash`, `slotmap`, `zlib-rs`

**BSD-2-Clause OR Apache-2.0 OR MIT** (2): `zerocopy`, `zerocopy-derive`

**BSD-3-Clause** (2): `tiny-skia`, `tiny-skia-path`

**BSD-3-Clause OR Apache-2.0** (2): `moxcms`, `pxfm`

**BSD-3-Clause OR MIT OR Apache-2.0** (2): `num_enum`, `num_enum_derive`

**Unlicense/MIT** (2): `same-file`, `walkdir`

**(Apache-2.0 OR MIT) AND BSD-3-Clause** (1): `encoding_rs`

**(MIT OR Apache-2.0) AND Unicode-3.0** (1): `unicode-ident`

**0BSD OR MIT OR Apache-2.0** (1): `adler2`

**Apache-2.0 / MIT** (1): `fnv`

**Apache-2.0 AND MIT** (1): `dpi`

**BSD-2-Clause** (1): `arrayref`

**BSD-2-Clause OR MIT OR Apache-2.0** (1): `mach2`

**CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception** (1): `blake3`

**CC0-1.0 OR MIT-0 OR Apache-2.0** (1): `constant_time_eq`

**MIT OR Apache-2.0 OR LGPL-2.1-or-later** (1): `r-efi`

**MIT OR Zlib OR Apache-2.0** (1): `miniz_oxide`

**MPL-2.0** (1): `persy`

**MPL-2.0+** (1): `smartstring`

### `launcher-mitsuami`: 340 third-party crates

**MIT OR Apache-2.0** (171): `allocator-api2`, `android_system_properties`, `arc-swap`, `arrayvec`, `as-raw-xcb-connection`, `ash`, `base64`, `bitflags`, `bumpalo`, `cc`, `cfg-expr`, `cfg-if`, `chacha20`, `cpufeatures`, `crc`, `crc-catalog`, `crc32fast`, `crossbeam-deque`, `crossbeam-epoch`, `crossbeam-utils`, `directories`, `dirs-next`, `dirs-sys`, `dirs-sys-next`, `displaydoc`, `document-features`, `either`, `errno`, `fdeflate`, `field-offset`, `find-msvc-tools`, `fixedbitset`, `flate2`, `futures-channel`, `futures-core`, `futures-executor`, `futures-io`, `futures-macro`, `futures-task`, `futures-util`, `getrandom`, `glob`, `glslang`, `glslang-sys`, `gpu-allocator`, `half`, `hashbrown`, `heck`, `http`, `httparse`, `image`, `itoa`, `jni-sys`, `jni-sys-macros`, `jobserver`, `js-sys`, `libc`, `litrs`, `lock_api`, `log`, `mitsuami`, `mitsuami-appkit`, `mitsuami-core`, `mitsuami-gtk`, `mitsuami-linux`, `mitsuami-macros`, `mitsuami-reactive`, `mitsuami-widgets`, `mitsuami-winui`, `naga`, `naga-types`, `ndk-sys`, `num-derive`, `num-traits`, `once_cell`, `parking_lot`, `parking_lot_core`, `percent-encoding`, `petgraph`, `pkg-config`, `png`, `presser`, `proc-macro-crate`, `proc-macro2`, `profiling`, `quote`, `rand`, `rand_core`, `range-alloc`, `raw-window-metal`, `rayon`, `rayon-core`, `regex`, `regex-automata`, `regex-syntax`, `renderdoc-sys`, `rustc_version`, `rustls-pki-types`, `rustversion`, `scopeguard`, `semver`, `serde`, `serde_core`, `serde_derive`, `serde_json`, `serde_spanned`, `shlex`, `simdutf8`, `smallvec`, `spirv-cross-sys`, `spirv-cross2`, `spirv-cross2-derive`, `static_assertions`, `syn`, `system-deps`, `tar`, `thiserror`, `thiserror-impl`, `toml`, `toml_datetime`, `toml_edit`, `toml_parser`, `toml_writer`, `unic-langid`, `unic-langid-impl`, `unicode-segmentation`, `unicode-width`, `unty`, `ureq`, `ureq-proto`, `utf8-zero`, `wasm-bindgen`, `wasm-bindgen-futures`, `wasm-bindgen-macro`, `wasm-bindgen-macro-support`, `wasm-bindgen-shared`, `web-sys`, `wgpu`, `wgpu-core`, `wgpu-core-deps-apple`, `wgpu-core-deps-emscripten`, `wgpu-core-deps-windows-linux-android`, `wgpu-hal`, `wgpu-naga-bridge`, `wgpu-types`, `windows`, `windows-collections`, `windows-core`, `windows-future`, `windows-implement`, `windows-interface`, `windows-link`, `windows-numerics`, `windows-reference`, `windows-result`, `windows-strings`, `windows-sys`, `windows-targets`, `windows-threading`, `windows-time`, `windows_aarch64_gnullvm`, `windows_aarch64_msvc`, `windows_i686_gnu`, `windows_i686_gnullvm`, `windows_i686_msvc`, `windows_x86_64_gnu`, `windows_x86_64_gnullvm`, `windows_x86_64_msvc`, `x11rb`, `x11rb-protocol`, `xattr`

**MIT** (63): `array-concat`, `bincode`, `bincode_derive`, `block2`, `bytes`, `cairo-rs`, `cairo-sys-rs`, `cfg_aliases`, `crunchy`, `data-encoding`, `dlib`, `gdk-pixbuf`, `gdk-pixbuf-sys`, `gdk4`, `gdk4-sys`, `gio`, `gio-sys`, `glib`, `glib-macros`, `glib-sys`, `gobject-sys`, `graphene-rs`, `graphene-sys`, `gsk4`, `gsk4-sys`, `gtk4`, `gtk4-macros`, `gtk4-sys`, `libadwaita`, `libadwaita-sys`, `libm`, `libredox`, `memoffset`, `nom`, `nom_locate`, `objc2`, `objc2-encode`, `objc2-foundation`, `ordered-float`, `pango`, `pango-sys`, `platform-dirs`, `quick-xml`, `redox_syscall`, `redox_users`, `simd-adler32`, `slab`, `strumbra`, `taffy`, `tokio`, `unsigned-varint`, `vec_extract_if_polyfill`, `version-compare`, `virtue`, `wayland-backend`, `wayland-client`, `wayland-protocols`, `wayland-scanner`, `wayland-sys`, `winnow`, `xml-rs`, `zigzag`, `zmij`

**Zlib OR Apache-2.0 OR MIT** (17): `bytemuck`, `bytemuck_derive`, `dispatch2`, `objc2-app-kit`, `objc2-cloud-kit`, `objc2-core-data`, `objc2-core-foundation`, `objc2-core-graphics`, `objc2-core-image`, `objc2-core-text`, `objc2-core-video`, `objc2-game-controller`, `objc2-io-surface`, `objc2-metal`, `objc2-quartz-core`, `objc2-quick-look-thumbnailing`, `objc2-uniform-type-identifiers`

**Apache-2.0 OR MIT** (15): `autocfg`, `bit-set`, `bit-vec`, `equivalent`, `fluent-bundle`, `fluent-langneg`, `fluent-syntax`, `indexmap`, `intl-memoizer`, `multiversion_no_op`, `pin-project-lite`, `portable-atomic`, `portable-atomic-util`, `rustc-hash`, `zeroize`

**MIT/Apache-2.0** (12): `core_detect`, `downcast-rs`, `filetime`, `fs2`, `khronos-egl`, `linked-hash-map`, `scoped-tls`, `type-map`, `version_check`, `winapi`, `winapi-i686-pc-windows-gnu`, `winapi-x86_64-pc-windows-gnu`

**MPL-2.0 OR GPL-3.0-only** (9): `librashader`, `librashader-cache`, `librashader-common`, `librashader-pack`, `librashader-preprocess`, `librashader-presets`, `librashader-reflect`, `librashader-runtime`, `librashader-runtime-wgpu`

**Apache-2.0** (7): `codespan-reporting`, `gethostname`, `gl_generator`, `glutin_wgl_sys`, `khronos_api`, `rspirv`, `spirv`

**Apache-2.0/MIT** (5): `bytecount`, `halfbrown`, `intl_pluralrules`, `pollster`, `rustc-hash`

**Unlicense OR MIT** (5): `aho-corasick`, `byteorder-lite`, `memchr`, `termcolor`, `winapi-util`

**MIT OR Apache-2.0 OR Zlib** (4): `glow`, `raw-window-handle`, `zune-core`, `zune-jpeg`

**Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT** (3): `linux-raw-sys`, `rustix`, `wasi`

**ISC** (3): `libloading`, `rustls-webpki`, `untrusted`

**Unicode-3.0** (3): `tinystr`, `zerofrom`, `zerovec`

**Zlib** (3): `foldhash`, `slotmap`, `zlib-rs`

**BSD-2-Clause OR Apache-2.0 OR MIT** (2): `zerocopy`, `zerocopy-derive`

**BSD-3-Clause OR Apache-2.0** (2): `moxcms`, `pxfm`

**MPL-2.0** (2): `option-ext`, `persy`

**(Apache-2.0 OR MIT) AND BSD-3-Clause** (1): `encoding_rs`

**(MIT OR Apache-2.0) AND Unicode-3.0** (1): `unicode-ident`

**0BSD OR MIT OR Apache-2.0** (1): `adler2`

**Apache-2.0 AND ISC** (1): `ring`

**Apache-2.0 OR GPL-2.0-only** (1): `self_cell`

**Apache-2.0 OR ISC OR MIT** (1): `rustls`

**Apache-2.0 WITH LLVM-exception** (1): `target-lexicon`

**BSD-3-Clause** (1): `subtle`

**CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception** (1): `blake3`

**CC0-1.0 OR MIT-0 OR Apache-2.0** (1): `constant_time_eq`

**CDLA-Permissive-2.0** (1): `webpki-roots`

**MIT OR Apache-2.0 OR LGPL-2.1-or-later** (1): `r-efi`

**MIT OR Zlib OR Apache-2.0** (1): `miniz_oxide`

**MPL-2.0+** (1): `smartstring`

