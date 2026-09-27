# Wine's Direct3D tests: our patch queue

`guest-tools/build-winetests.sh` fetches Wine's `dlls/d3d8/tests/`,
`dlls/d3d9/tests/` and `include/wine/` at a pinned tag (a sparse
checkout in `build/winetest/<tag>/`, not vendored: the tests are LGPL)
and applies these in filename order onto restored sources before every
build. None changes what a test checks. 01 to 03 are behind a macro the
script defines; **04, 05, 07 and 08 add or remove no line** (06 adds lines to `test.h` only, which moves no test file's key) (the baselines in
`reference/winetest/` are keyed by source line, so a patch that moves
lines makes every later key read as new). Track M16
(`docs/tracks/m16-dx9-ddi.md`).

Beside the queue, the script force-includes
`guest-tools/src/winetest/w98compat.h` into every test file: Win98's
user32 exports the Unicode display calls the tests use
(`EnumDisplaySettingsW`, `ChangeDisplaySettingsExW`, `GetMonitorInfoW`,
...) as stubs failing with `ERROR_CALL_NOT_IMPLEMENTED`, and each
wrapper falls back to the ANSI call only on that error, so XP's results
do not move. It also defines 04's trace macro.

| Patch | What | Why | Drop when |
|---|---|---|---|
| `01-d3d9-device-no-d3d9on12` | `WINETEST_NO_D3D9ON12` fences `test_d3d9on12` and its includes in d3d9 `device.c` | it needs `d3d9on12.h` and `dxgi1_4.h`, which mingw lacks; D3D9On12 is Windows 10, so the test skips on XP and 98 anyway | never |
| `02-d3d8-visual-no-wow64` | `WINETEST_NO_WOW64` fences the WoW64 probe in d3d8 `visual.c`'s `START_TEST` | it reads `TEB64` / `PEB64` and a `TEB` field mingw's headers do not have; a 32-bit XP or 98 guest is never WoW64 | never |
| `04-trace-create-device` | the tests' `create_device` (d3d8 `device.c` and `visual.c`, d3d9 `device.c`) traces a failed `CreateDevice`: hr, adapter, size, format, windowed, depth format, flags (`WINETEST_TRACE_CREATE_DEVICE` in `w98compat.h`) | the rig's Win98 made no d3d8 device at all, and the tests only say they skipped | when Win98's device creation is understood |
| `05-d3d9-visual-yuv-no-surface` | d3d9 `visual.c`'s `yuv_color_test` goes on to the next format when `CreateOffscreenPlainSurface` fails (the check still fails) | on the rig's Win98 the surface failed and the test locked the NULL pointer, ending the file at `visual.c:12859` | never |
| `07-device-volume-texture-guard` | d3d8 and d3d9 `device.c`'s `test_resource_type` skips its volume checks when `CreateVolumeTexture(2, 4, 8, 4 levels, SYSTEMMEM)` fails (the check still fails); the guard opens on the `ok` line and closes on the `Release` line | Win98's DirectDraw refuses that volume (a mip count above log2 of the larger of width and height, doc 19 §45), and the test then called through the NULL texture and ended both files | never |
| `08-visual-updatetexture-no-texture` | d3d8 and d3d9 `visual.c`'s `test_updatetexture` goes on to the next case when a source or target texture was not made (the check still fails), releasing the source; each guard sits on its `ok` line | on the rig's Win98 the GeForce 6200 claims ATI2, the system-memory ATI2 source fails in Win98's HEL, and the test then crashed inside `D3D8.DLL`, which left DirectDraw locked for every later process of that boot | never |
| `06-test-h-exception-module` | `include/wine/test.h`'s exception filter also prints the faulting module and offset and the registers | an address alone says nothing on Win98, where system DLLs live in the shared arena (it named `3DFX32V2.DLL`) | never |
| `03-d3d9-visual-null-device-skip` | `WINETEST_NULL_DEVICE_SKIP` returns from d3d9 `visual.c`'s `test_desktop_window` when the device with a NULL window was not made | XP's runtime refuses a windowed device with neither a focus nor a device window (the documented rule), and the test then calls through the NULL device and ends the file with 17 tests unrun | never |

Regenerate a patch as in `patches/qemu/README.md`: `git diff --no-index
--no-prefix` between a pristine copy and the edited one, then apply it
to a pristine checkout (`git -C build/winetest/<tag> checkout -- .`)
before committing.
