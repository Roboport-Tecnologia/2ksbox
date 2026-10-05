# 1. Goals and non-goals

What 2ksbox is for, what it is not, and the pillars the work is
organised around. The architecture is doc 02, the milestones doc 08,
the decisions doc 10.

## Vision

A virtual machine manager for Linux, Windows and macOS that runs current
systems (Windows 11 first, track M20) and is best at vintage ones
(ADR-024). Modern guests get the same launcher, snapshots and player as
vintage ones, and run under hardware virtualization: KVM, WHPX, or on
Apple Silicon an ARM64 guest under Hypervisor.framework. The rest of
this doc is about the vintage boxes, which is where the original work
is.

A Windows 98 or XP machine that behaves like the real thing around
1998 to 2005. Games install from your own disc dumps, copy protection
included. Direct3D and Glide titles run accelerated. The picture looks
like a shadow-mask CRT fed by a VGA card, not a blurry stretched
rectangle in a window.

## Goals

1. **Cross-platform, open source.** Linux, Windows, macOS, with Apple
   Silicon a first-class target. Open source ruled out VMware.
   VirtualBox has had no 3D for pre-Win7 guests since 6.1.
2. **Real guest 3D**, host-accelerated, on Win98 and XP. Direct3D goes
   through our display adapter's driver and its host executor (docs 14,
   15, 19). Glide runs on an emulated Voodoo 2 (doc 21). OpenGL goes
   through the qemu-3dfx pass-through.
3. **Pixel-accurate video.** The player captures the raw guest
   framebuffer before scaling and presents it with the correct aspect
   (non-square modes like 320×200 included), integer scaling and a CRT
   shader chain (libretro slang shaders through librashader on wgpu).
4. **A faithful CD-ROM drive.** Raw dumps (cue/bin, CCD, MDS, ISO) mount
   as a drive with CD-DA, subchannel data, period error behaviour and a
   raw TOC. A host folder mounts as a disc (`isodir:`). Copy protection
   passes because the drive is faithful; nothing is patched.
5. **Low latency.** QEMU runs in-process with the front end, and the
   display, input and audio paths are built for latency (doc 03).
6. **UTM-style UX.** A machine library, guided creation for six
   families (Windows 98, XP, 7 and 11, DOS, Other), sane defaults, a one-click
   guest-tools disc, no 40-flag QEMU command lines. It ships as a player
   (one machine per window) and a launcher (doc 07).

## Non-goals

- **Cycle-accurate hardware.** 86Box and PCem do that. We target a fast
  machine of the era. The one concession: a DOS machine's processor is
  throttled to a calibrated instruction rate (doc 06).
- **Bypassing DRM.** No-CD patches, key generators and activation
  workarounds are out of scope. Users supply their own media, licences
  and dumps.
- **Integration features** (shared folders beyond a folder disc,
  clipboard sync) were later nice-to-haves; since 2026-10-02 they are
  track M23 (doc 24, ADR-027), Windows 11 first.

## The pillars

| # | Pillar | Scope | Novelty |
|---|---|---|---|
| P1 | QEMU fork + CPU fast paths | a slimmed QEMU with TCG fast paths (x87, SSE, SIMD, REP strings, SMC, lookup and TLB work; docs 13, 16, 22) | integration and optimisation |
| P2 | Guest display drivers | `d3dpt-vga` drivers: XP miniport + display driver with DirectDraw and a DirectX 9 Direct3D DDI (doc 15); Win9x mini-VDD + display driver (doc 19); a WDDM driver for Windows 7 and Aero (track M18) | original work |
| P3 | Paravirtual Direct3D | the protocol and the host executor (doc 14) | original work |
| P4 | Player display pipeline | in-process embed, mode analysis, event-driven geometry, CRT shading on wgpu + librashader (docs 03, 11) | original work |
| P5 | Launcher | `launcher-core` with the mitsuami front end (ADR-023) and a C ABI: library, form, disc shelf, snapshots, shader profiles (doc 07) | original work |
| P6 | Raw CD-ROM backend | `libdisc`: raw disc model and formats, ATAPI device, CD-DA, disc shelf, folder discs (docs 05, 17) | original work |

No existing project provides P2 to P6. They are written to be reusable
by other retro-VM projects.
