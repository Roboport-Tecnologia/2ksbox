# Khronos OpenGL and EGL headers (vendored)

`GL/glcorearb.h` and `KHR/khrplatform.h` from the Khronos OpenGL registry
(https://registry.khronos.org/OpenGL/), MIT-licensed (see the SPDX header
in each file), fetched 2026-09-02, and `GL/wgl.h` from the same
registry, fetched 2026-10-04: qemu-3dfx's WGL backend
(`hw/mesa/mglcntx_mingw.c`) includes it, which mingw-w64 carries and
Visual Studio's SDK does not (the MSVC build, `WIN_QEMU_CC=msvc`).

qemu-3dfx's `hw/mesa/mesagl_pfn.h` includes `<GL/glcorearb.h>`. Linux has
it from libglvnd / Mesa, but macOS has no `GL/` headers outside XQuartz,
which the Mac build does not need. `scripts/configure-qemu.sh` puts this
directory on the include path on every platform, so every build sees one
known header version.

`EGL/egl.h`, `EGL/eglext.h` and `EGL/eglplatform.h` from the Khronos EGL
registry (https://registry.khronos.org/EGL/), Apache-2.0 (the SPDX
header in each file), fetched 2026-10-04. libepoxy's EGL dispatch needs
them, and QEMU takes OpenGL only with `epoxy/egl.h`: on Windows the
MSVC build's libepoxy (`scripts/build-deps.sh`) builds against these,
where MSYS2's comes with mingw's copy.
