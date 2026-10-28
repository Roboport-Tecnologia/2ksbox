# Ubuntu 26.04 guest-tools build notes

Observed with x86_64 Ubuntu 26.04, host GCC 15.2.0,
`i686-w64-mingw32-gcc (GCC) 13-win32`, and GNU Binutils 2.46.

## Classic MSVCRT on older MinGW-w64 toolchains

Some MinGW-w64 GCC builds do not recognize `-mcrtdll=msvcrt-os` even
though they already link `msvcrt.dll` by default. On this setup, a
minimal Windows executable built with no CRT override imported
`KERNEL32.dll` and `msvcrt.dll`.

The XP guest-tools scripts now probe whether the explicit CRT flag is
accepted. If not, they try the compiler default and inspect the PE
imports. A compiler that produces UCRT-linked executables is rejected;
the build does **not** silently fall back to UCRT.

## GNU Binutils 2.46 and qemu-3dfx wrapper export checks

The qemu-3dfx submodule's existing `exports-check` targets can count
more than the named exports if they match bracketed entries throughout
the `objdump` output. In the reported environment, the broad filter
counted twice the correct named-export totals.

The generated DLLs had these verified named-export counts when parsing
only the `[Ordinal/Name Pointer] Table` from `objdump -p`:

| DLL | Named exports |
| --- | ---: |
| `glide.dll` | 131 |
| `glide2x.dll` | 144 |
| `glide3x.dll` | 94 |
| `opengl32.dll` | 2977 |

The export-counting fix is **not** included in this branch because the
affected Makefiles belong to the independently versioned
`third_party/qemu-3dfx` Git submodule. An upstream fix or an
appropriately attributed build patch should be discussed separately.

## Other observations

A stale user-local Meson executable pointing to deleted temporary
sources broke the GLib dependency build; prioritizing the working
system Meson fixed that local environment issue. QEMU configuration
required `uv`, and the Mitsuami launcher required `libadwaita-1-dev`.

After local qemu-3dfx export-check adjustments, the Windows XP driver
and guest-tools ISOs built and Windows XP SP3 x86 installed and booted.
The host test suite returned **44 passed, 2 failed, 6 skipped**; the
failures were for missing EDK2 artifacts after an incomplete firmware
build. Native Direct3D tests also logged crashes despite some tests
being reported as passing. Guest-side 3D stability has not yet been
established.

Do not upload Windows installation media, guest disk images, license
keys, generated guest ISOs, third-party binaries, or other
non-redistributable artifacts with build reports.
