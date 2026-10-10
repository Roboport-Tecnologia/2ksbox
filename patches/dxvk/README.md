# DXVK patch queue

DXVK's d3d9 is the host executor's Direct3D 9 for the paravirtual
Direct3D device (doc 14, ADR-006/007) on Linux, macOS and Windows. Only
`d3d9` is built: `scripts/configure-dxvk.sh` → `build/dxvk`, and with
`--windows` → `build/win/dxvk`, built with MSVC (shipped as
`dxvk_d3d9.dll`). The macOS
Vulkan setup (KosmicKrisp, the SDK, the app's own loader) is in
`docs/build-macos.md`.

`scripts/prepare-dxvk.sh` applies these patches in filename order to the
pinned submodule `third_party/dxvk` (3.1.0, master `d7ac258`), restoring
every tracked file a patch touches first, and then `dxbc-spirv/*.patch`
inside DXVK's own `subprojects/dxbc-spirv` submodule (its SM1-3
compiler), paths relative to that repository.

| Patch | What / why | Drop when |
|---|---|---|
| `01-native-macos` | dxvk-native builds on macOS: the Windows shim (`util_win32_compat.h`) was `#if __unix__`, which Apple clang does not define; `getExePath` via `_NSGetExecutablePath`; one-argument `pthread_setname_np`; the `libvulkan.1.dylib` loader names; the d3d9 export list as an ld64 `-exported_symbols_list` (`d3d9.exports`, from `d3d9.sym`) | upstream accepts a macOS port |
| `02-geometry-shader-optional` | `geometryShader` required → optional. d3d9 never creates one, Vulkan-on-Metal drivers have none, and DXVK uses the flag only for a stage mask. Must be required again if d3d10/11 are ever built | KosmicKrisp grows geometry shaders, or upstream scopes the requirement per API |
| `03-portability-enumeration` | enables `VK_KHR_portability_enumeration` when offered and sets `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`; the loader hides portability ICDs (MoltenVK, KosmicKrisp) from apps that do not opt in. Without it: "Failed to create Vulkan instance" (`VK_ERROR_INCOMPATIBLE_DRIVER`) | upstream opts in |
| `04-wsi-headless` | a window-less WSI driver (`src/wsi/headless`, `DXVK_WSI_DRIVER=Headless`): one fake monitor with a D3D9-era mode list, no valid window, `createSurface` fails. A swapchain with a NULL window has no presenter and `Present` is a no-op, so the executor renders off-screen and reads the back buffer. The only WSI we build; the patch drops upstream's "SDL3, SDL2, or GLFW are required", and `configure-dxvk.sh` disables all three | never; upstream has no headless WSI |
| `05-fill-mode-non-solid-optional` | `fillModeNonSolid` required → optional; `BindRasterizerState` clamps wireframe and point fill to solid when the driver lacks it, as KosmicKrisp does | KosmicKrisp exposes `fillModeNonSolid` |
| `06-vulkan-loader-beside-us` | on macOS, `@loader_path/libvulkan.1.dylib` (and the unversioned name) is tried before the bare names. macOS ships no Vulkan, the app carries its own loader and ICD beside the executor, and `DYLD_*` is stripped from a hardened, notarized process. Inert in a checkout | upstream has a macOS bundle story |
| `07-ff-bumpenvmap-luminance` | `D3DTOP_BUMPENVMAPLUMINANCE` in the fixed-function shader applies its luminance. The previous stage's value went into a shadowing variable (the outer one stayed 0), and the luminance came from the environment map's texel instead of the bump map's, so BUMPTEST drew full intensity where L = ½ was asked. `tools/d3dpt-dp2-test.cpp`'s luminance case fails without it | upstream fixes both lines |
| `08-wsi-headless-windows` | patch 04's headless WSI on Windows too, beside Win32 (still the default when `DXVK_WSI_DRIVER` is unset). The executor asks for `Headless` on every host, since none of its devices has a window | never, as 04 |
| `09-singleton-acquire-throw` | `Singleton<T>::acquire` (`util_singleton.h`, holder of d3d9's process-wide `DxvkInstance`) counted a user *before* constructing the object. When the constructor threw (an ICD that reports no GPU, such as KosmicKrisp on macOS 15 where `vkEnumeratePhysicalDevices` fails, or a loader with no ICD), the count stayed at one and the object null, and the next `Direct3DCreate9` in the process faulted on that null instance. The executor names the same DXVK by full path and by leaf name, so the community app on macOS 15 died at the adapter's realize instead of moving on to the Wine executor. It now counts after `new`. The executor also never asks a library twice (`d3dpt_exec.cpp`, `refused`); the `exec-no-device` check in `scripts/test.sh` tests the built artefacts | upstream counts after constructing |
| `10-shader-cache-version` | the on-disk shader cache (`DxvkShaderCache`, `~/.cache/dxvk` natively) is keyed by `DXVK_VERSION`, which a shallow submodule makes a plain `3.1.0` forever, so shaders compiled before a patch below kept being served after it. The cache now carries its own version, `DXVK_VERSION "+2ksbox.N"`: **bump N with every patch that changes what DXVK or dxbc-spirv emits for a shader**. `tools/winetest-dxvk.sh` runs with `DXVK_SHADER_CACHE=0` | never, while we patch the compiler |
| `11-fog-z-rule` | table fog uses Z (not W) whenever the device's projection transform is not a perspective one, for programmable vertex shaders and pre-transformed vertices too; upstream took W fog for both (the latter for "some D3D6 jank", no title named: a D3D6 title whose fog looks wrong on the driver points here). And linear vertex fog with FOGSTART == FOGEND fogs every vertex fully. The cards of the era, and the rig (Wine's `fog_with_shader_test`, `test_table_fog_zw`, `fog_test`, `fog_special_test`; M16) | upstream matches the hardware |
| `12-depth-clip-tl-vertices` | depth clip is per draw (`WantDepthClip`): untransformed geometry is always clipped, pre-transformed vertices only while `D3DRS_CLIPPING` and the Z test are on. Upstream always clipped, and flattened a pre-transformed vertex's Z to 0 with the Z test off to escape the clip, which table fog then read as 0. The executor turns `D3DRS_CLIPPING` off for pre-transformed draws, since the display driver claims no `D3DPMISCCAPS_CLIPTLVERTS` and the guest runtime clipped them already; the rig then draws them unclipped in depth (Wine's `z_range_test`, `depth_clamp_test`; M16 finding 24). Under Wine, where DXVK claims the cap, `depth_clamp_test`'s cap branch fails 5 checks by design | never: it follows our driver's caps |
| `13-flat-shading-programmable-ps` | `D3DSHADE_FLAT` never reached a programmable pixel shader: the pipeline asks the fragment shader's metadata for flat inputs, and `DxvkIrShader` never filled it; and ps_1_x / ps_2_x colour inputs, which have no semantic `dcl`, were never in the mask (Wine's `test_shademode`) | upstream fixes both |
| `14-ffp-position-w` | the fixed-function vertex path takes an untransformed position's W as 1.0 whatever the vertex carries (an XYZW FVF, a FLOAT4 position); DXVK used the input W, so such geometry drew at 1/W the size (Wine's `test_ffp_w`) | upstream matches the hardware |
| `15-vertex-input-eq-divisors` | `DxvkGraphicsPipelineVertexInputState::eq` stops at the first difference in the divisor loop too. It assigned `eq` there without `&& eq`, so a matching divisor overwrote a `false` from the attributes and two vertex layouts with the same counts and divisors compared equal (d3d9's null binding gives every fixed-function draw a divisor). libstdc++'s `unordered_map` compares stored hashes before calling `eq`, MSVC's does not, so only the MSVC build (the Windows one since 2026-10-04) reused the wrong pipeline: the oracle's two-stream fixed-function, cube and volume checks failed. `docs/build-windows.md` "DXVK under MSVC" | upstream adds `&& eq` |
| `16-create-no-throw` | `CreateD3D9`, behind every `Direct3DCreate9*` export, catches what constructing the interface throws (`DxvkInstance` with no usable device) and returns null / `D3DERR_NOTAVAILABLE`. A native build links libstdc++ statically with its symbols hidden, so an exception that left DXVK reached the executor's `catch` through the system libstdc++'s personality and DXVK's own unwinder, and the process aborted (SIGABRT, `exec-no-device` on Linux). MSVC and macOS share one runtime with the caller, so it was caught there | upstream catches at the export |
| `17-no-loader-no-crash` | `LibraryLoader::sym` answers null when no Vulkan loader was found. `LibraryFn`'s members look themselves up as it is constructed, through the null `vkGetInstanceProcAddr`, so a process with no loader (a Mac without `DYLD_LIBRARY_PATH`, the native harnesses) died of SIGSEGV in `LibraryFn::LibraryFn` before DxvkInstance could throw "Failed to load vulkan-1 library"; now that is thrown, patch 16 catches it, and `Direct3DCreate9` returns null. The executor probes for a loader before opening DXVK and never reached it. `dxvk-no-loader` in `scripts/test.sh` | upstream checks the loader before the lookups |
| `dxbc-spirv/01-vs-fog-default` | a vertex shader that never writes `oFog` leaves fog at 0.0 (fully fogged), not 1.0, as the cards of the era do (`fog_with_shader_test`). Applied inside `subprojects/dxbc-spirv` | upstream matches the hardware |
| `dxbc-spirv/02-ps2-point-sprite-texcoords` | point sprites replace a ps_2_x shader's `t` registers too (declared as `eTexture`, which the adjustment skipped), so a particle system's ps_2_0 read one texel over the whole sprite (`test_pointsize`) | upstream fixes it |

## Building and testing

`scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C
build/dxvk`; `scripts/build.sh` runs it. The oracles are
`tools/dxvk-d3d9-test.cpp` (build line in its header) and the native
reference scene; `scripts/test.sh` runs DXVK under
`DXVK_WSI_DRIVER=Headless` in the `d3dgame9-nat`, `d3dpt-exec` and
`d3dpt-dp2` checks.

On macOS the library `dlopen`s the loader by leaf name, so a run from a
checkout needs `DYLD_LIBRARY_PATH=/opt/homebrew/opt/vulkan-loader/lib`
(never all of `/opt/homebrew/lib`; see `docs/00-status.md`) and
`VK_ICD_FILENAMES` naming KosmicKrisp's ICD; `test.sh` sets both. On
KosmicKrisp the reference scene's frame 300 is 1095 pixels beyond
tolerance 8 against the rig golden (1089 on Linux RADV). MoltenVK refuses
(`Device does not support required feature 'shaderCullDistance'`), as
expected.

## Regenerating

Run `prepare-dxvk.sh`, edit inside `third_party/dxvk`, then `git -C
third_party/dxvk diff -- <files>` (plain `a/` `b/` prefixes; `git add -N`
a new file first so it appears with `--- /dev/null`); a dxbc-spirv patch is
`git -C third_party/dxvk/subprojects/dxbc-spirv diff -- <files>`. Prove it
from pristine: `prepare-dxvk.sh` twice must succeed. A patch that changes
compiled shaders bumps patch 10's `+2ksbox.N`.

The Direct3D behaviour patches (11 onwards, M16) follow the rule in
`docs/tracks/m16-dx9-ddi.md`: only where the rig's Wine-suite run and
DXVK's own disagree and a title could meet it. The quick loop is
`tools/winetest-dxvk.sh` (Wine on the host, no guest, 25 s for d3d9
visual); the proof is the guest runs against the rig's baselines.
