# Patches on EDK2 (Windows 11's firmware)

`scripts/build-edk2.sh` builds, from the commit QEMU's own `roms/edk2`
submodule pins: on Arm hosts (`build.sh`'s `edk2` stage) EDK2's
ArmVirtQemu for QEMU's aarch64 `virt` board, with QEMU's build options
plus `SECURE_BOOT_ENABLE`; on Windows hosts (`build-windows.sh`'s `edk2`
stage) OvmfPkgX64 with Visual Studio 2022, with QEMU's options for its
secure build but `SMM_REQUIRE=FALSE`, since WHPX has no SMM (track M20
step 5). It applies these with `git apply`, in filename order, to a tree
it restores first. The files keep EDK2's CRLF line
endings (`.gitattributes` here), and a patch must too. The build is
stamped by the pin, these files and the script, so editing one rebuilds
the firmware on the next `scripts/build.sh`.

Why the firmware is ours at all (track M20 step 4): QEMU's prebuilt
`edk2-aarch64-code.fd` has no Secure Boot, which Windows 11's setup
requires, and no AHCI driver. With these, a stock Windows 11 on Arm ISO
installs with no setup key, onto a disk on AHCI that live snapshots can
hold (QEMU 9.2's NVMe cannot be migrated).

| Patch | What / why | Drop when |
|---|---|---|
| `01-armvirt-ahci` | ArmVirtQemu gains OVMF's SATA controller, ATA/ATAPI pass-through and ATA bus drivers (the ones OvmfPkgX64 has), so the firmware boots the disk and the CD-ROM from the ICH9 AHCI controller the launcher puts on the board. Windows on Arm has its own AHCI driver | never: upstream ArmVirtQemu has no AHCI |
| `02-ramfb-modes` | ramfb's mode list ended at 1024x768 and did not have the board's own preferred size (`PcdVideoHorizontal/VerticalResolution`, 1280x800), so the boot manager left it at 800x600, which is what Windows' Basic Display keeps. Adds 1280x800, 1440x900, 1600x900 and 1920x1080; the firmware's setup screen (Device Manager, OVMF Platform Configuration) picks another | upstream takes the size from QEMU |
| `03-ovmf-vs2022` | OvmfPkgX64 built with Visual Studio 2022 17.14 (MSVC 14.44), on Windows. Some structure copies compile to `memcpy()` calls, and EDK2 modules link no C library (DisplayEngineDxe: LNK2001): the DXE phase's module types get CryptoPkg's IntrinsicLib (not SEC, whose ResetVector is a raw image). And VS2022 links every X64 module with `/ALIGN:4096` and a 0x200 file alignment, which GenFv cannot rebase for the PEI phase's execute-in-place modules ("Section-Alignment and File-Alignment do not match", PeiCore): SEC, PEI_CORE and PEIM link with `/ALIGN:32 /FILEALIGN:32` | upstream builds OvmfPkgX64 with this Visual Studio |
