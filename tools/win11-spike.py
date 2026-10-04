#!/usr/bin/env python3
"""M20 step 1: Windows 11 on our x86_64 QEMU with EDK2 and a TPM 2.0.

  tools/win11-spike.py install <win11-x64.iso>   unattended install, then a report
  tools/win11-spike.py boot                      boot the installed disk, time the desktop

ARCH=aarch64 (M20 step 4) is Windows 11 on Arm on QEMU's `virt` board
under the host's hypervisor (HVF on a Mac; ACCEL=hvf), from Microsoft's
Arm64 ISO: the launcher's machine (`bundle::Machine::arm_args`) on our own
EDK2 (scripts/build-edk2.sh: Secure Boot capable, AHCI), the disk and CDs
on AHCI, `tpm-tis-device`, `ramfb`, HD Audio and the libtpms TPM. COM1 is
the board's PL011, which Windows does not make a COM port, so the guest
also reports on a FAT disk on USB (OUT/report.img, read with mtools).

Stock Windows 11 setup with no bypass keys: its TPM, Secure Boot and CPU
checks run. The TPM is the distro's swtpm (TPM=swtpm) or our libtpms
backend inside QEMU (TPM=libtpms, patch 75, M20 step 2), which takes over
swtpm's state the first time so Windows keeps its TPM. The disk, firmware
variables and TPM state live in
OUT, the machine's stand-in for a bundle. The guest reports on COM1
(tools/win11-spike/spike.ps1); this script timestamps each line into
OUT/com1.log and dumps the screen every SHOT seconds into OUT/shots/.

Environment:
  OUT=/mnt/data2/david/w11   keep it short (AF_UNIX path limit)
  ACCEL=kvm|tcg        kvm: -cpu host; tcg: -cpu max (time the Air with this)
  SMP=4 MEM=4096       Windows 11's minimum is 2 cores and 4 GB
  EDITION="Windows 11 Pro"   the install.wim image name setup installs
  QEMU=build/qemu/qemu-system-x86_64
  FW=build/qemu/qemu-bundle/usr/local/share/qemu   the EDK2 files
  VNC=20               watch on 127.0.0.1:5920 (needs ninja -C build/qemu
                       pc-bios/keymaps/en-us); default none
  SHOT=60              seconds between screen dumps
  SETTLE=60            boot: seconds on the desktop before the power button
  TPM=swtpm|libtpms    the TPM backend (libtpms: OUT/tpm.permall)
  ARCH=x86_64|aarch64  the guest (aarch64: QEMU=build/qemu/qemu-system-aarch64,
                       FW=qemu/pc-bios, TPM=libtpms, ACCEL=hvf on a Mac)
  FW_ARM=2ksbox        aarch64 firmware: ours (2ksbox-aarch64-*.fd), or edk2
                       for QEMU's prebuilt one (no Secure Boot, no AHCI: the
                       disk goes on NVMe, the CDs on USB)
  GPU=ramfb            aarch64: the screens, a comma list: ramfb (the firmware,
                       and Windows' Basic Display), virtio-gpu-pci (viogpudo,
                       from the drivers disc, once installed), or both, as the
                       launcher's board has them (the player shows the live
                       one; the shots here are of the first)
  NET=1                a network card (aarch64: virtio-net, whose driver is on
                       the drivers disc; x86_64: e1000e) on QEMU's user network
  DRIVERS=<iso>        aarch64: the drivers disc in a CD drive of its own
                       (default build/virtio-win/2ksbox-drivers-arm64.iso when
                       it exists; DRIVERS= for none)
  SB_BYPASS=1          install: setup's LabConfig BypassSecureBootCheck
                       (needed on FW_ARM=edk2 only)
  LANG_ISO=en-US       the ISO's language, when 7z is not there to read it
  PROBE=1              boot, ARCH=aarch64: on the desktop, tools/win11-spike/
                       probe.ps1 from the REPORT disk, elevated the same way
                       (UAC's Yes at UAC_YES_ARM), appends W11-PROBE lines
  PROBE_PS1=<file>     the script PROBE=1 runs instead of probe.ps1, as
                       main.ps1 under stub.ps1, which logs a refusal
                       (tools/win11-spike/smb.ps1: the shared folder, M23)
  PROBE_WAIT=60        seconds a probe may run after its first line
  CLIPBOARD=1          the clipboard's channel: QEMU's qemu-vdagent on a
                       virtio-serial port named com.redhat.spice.0 (M23);
                       the host's end is the player's (QEMU=tools/player-as-qemu.sh)
  SMB=<socket>         NET=1: the guest's 10.0.2.4:445 forwarded to the Unix
                       socket an SMB server listens on (smbserve --unix,
                       track M23; QEMU patch 79)
  TPM_PPI=off          aarch64: the TPM without its 1 KiB PPI RAM region, which
                       11.1's HVF aborted on before QEMU patch 82 (unmapping a
                       range smaller than a 16 KiB page); kept for an A/B
  REPORT=1             boot: on the desktop, an elevated PowerShell (Win+R,
                       Ctrl+Shift+Enter, UAC's Yes clicked on the tablet;
                       Windows 11's UAC has no Alt+Y) writes Get-Tpm and the
                       endorsement key's hash to COM1 (W11-TPM)

Ends a run with the ACPI power button, never a kill, unless Windows does
not answer it.
"""
import os, queue, shutil, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qmpc  # noqa: E402

OUT = os.environ.get("OUT", "/mnt/data2/david/w11")
ARCH = os.environ.get("ARCH", "x86_64")
ARM = ARCH == "aarch64"
OURS = os.environ.get("FW_ARM", "2ksbox") == "2ksbox"
SB_BYPASS = os.environ.get("SB_BYPASS", "") == "1"
NET = os.environ.get("NET", "") == "1"
GPU = os.environ.get("GPU", "ramfb")
DRIVERS = os.environ.get("DRIVERS", os.path.join(ROOT, "build/virtio-win/2ksbox-drivers-arm64.iso"))
ACCEL = os.environ.get("ACCEL", "hvf" if ARM and sys.platform == "darwin" else "kvm")
SMP = os.environ.get("SMP", "4")
MEM = os.environ.get("MEM", "4096")
EDITION = os.environ.get("EDITION", "Windows 11 Pro")
QEMU = os.environ.get("QEMU", os.path.join(ROOT, "build/qemu/qemu-system-" + ARCH))
FW = os.environ.get("FW", os.path.join(ROOT, "qemu/pc-bios" if ARM else "build/qemu/qemu-bundle/usr/local/share/qemu"))
VNC = os.environ.get("VNC", "")
SHOT = int(os.environ.get("SHOT", "60"))
SETTLE = int(os.environ.get("SETTLE", "60"))
TPM = os.environ.get("TPM", "libtpms" if ARM else "swtpm")
REPORT = os.environ.get("REPORT", "") == "1"
PROBE = os.environ.get("PROBE", "") == "1"
SMB = os.environ.get("SMB", "")
CLIPBOARD = os.environ.get("CLIPBOARD", "") == "1"
TPM_PPI = os.environ.get("TPM_PPI", "on") != "off"
PROBE_WAIT = int(os.environ.get("PROBE_WAIT", "60"))
SPIKE = os.path.join(ROOT, "tools/win11-spike")
PROBE_PS1 = os.environ.get("PROBE_PS1", os.path.join(SPIKE, "probe.ps1"))
ARM_CODE = "2ksbox-aarch64-code.fd" if OURS else "edk2-aarch64-code.fd"
ARM_VARS = "2ksbox-aarch64-vars.fd" if OURS else "edk2-arm-vars.fd"
ARM_FW = FW          # where the two above are


def arm_firmware():
    """QEMU keeps its own aarch64 pair compressed, and nothing unpacks it
    (the launcher's is ours); FW_ARM=edk2 unpacks it into OUT."""
    global ARM_FW
    if OURS:
        return
    for f in (ARM_CODE, ARM_VARS):
        dst = os.path.join(OUT, f)
        if not os.path.exists(dst):
            with open(dst, "wb") as out:
                subprocess.run(["bunzip2", "-kc", os.path.join(FW, f + ".bz2")], stdout=out, check=True)
    ARM_FW = OUT

DISK = os.path.join(OUT, "disk.qcow2")
VARS = os.path.join(OUT, "vars.fd")
TPMDIR = os.path.join(OUT, "tpm")
TPMSTATE = os.path.join(OUT, "tpm.permall")


def die(msg):
    print("win11-spike:", msg, file=sys.stderr)
    sys.exit(1)


def log(msg):
    line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(os.path.join(OUT, "spike.log"), "a") as f:
        f.write(line + "\n")


def iso_language(iso):
    """The ISO's own UI language, from sources/lang.ini (setup refuses a
    language its media lacks)."""
    if os.environ.get("LANG_ISO"):
        return os.environ["LANG_ISO"]
    # 7z: Microsoft's ISO keeps its files in UDF, and xorriso reads only
    # the ISO 9660 side (a README)
    ini = subprocess.run(["7z", "e", "-so", iso, "sources/lang.ini"],
                         check=True, capture_output=True).stdout
    section = None
    for raw in ini.decode("utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith("["):
            section = line
        elif section == "[Available UI Languages]" and "=" in line:
            return line.split("=")[0].strip()
    die("no language in the ISO's sources/lang.ini")


LABCONFIG_SB = """      <RunSynchronous>
        <RunSynchronousCommand wcm:action="add">
          <Order>1</Order>
          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>
        </RunSynchronousCommand>
      </RunSynchronous>"""


def unattend_iso(lang):
    d = os.path.join(OUT, "unattend")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    xml = open(os.path.join(SPIKE, "autounattend.xml")).read()
    xml = xml.replace("@LANG@", lang).replace("@EDITION@", EDITION)
    if ARM:
        xml = xml.replace('processorArchitecture="amd64"', 'processorArchitecture="arm64"')
    if SB_BYPASS:
        # QEMU's aarch64 EDK2 has no Secure Boot, which setup requires
        # ("The PC must support Secure Boot"); its TPM check passes
        xml = xml.replace("</UserData>", "</UserData>\n" + LABCONFIG_SB, 1)
    open(os.path.join(d, "autounattend.xml"), "w").write(xml)
    shutil.copy(os.path.join(SPIKE, "spike.ps1"), d)
    iso = os.path.join(OUT, "unattend.iso")
    subprocess.run(["xorriso", "-as", "mkisofs", "-quiet", "-o", iso,
                    "-V", "UNATTEND", "-J", "-r", d], check=True)
    return iso


def start_swtpm():
    os.makedirs(TPMDIR, exist_ok=True)
    sock = os.path.join(OUT, "swtpm.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    p = subprocess.Popen(["swtpm", "socket", "--tpm2",
                          "--tpmstate", "dir=" + TPMDIR,
                          "--ctrl", "type=unixio,path=" + sock,
                          "--terminate", "--log", "file=%s/swtpm.log" % OUT])
    for _ in range(50):
        if os.path.exists(sock):
            return p, sock
        time.sleep(0.1)
    die("swtpm made no socket (OUT/swtpm.log)")


def swtpm_blob(path):
    """libtpms's permanent state out of swtpm's file: a header (version,
    min_version, hdrsize, flags, totlen), then tag-length-value records,
    of which tag 1 is the plain blob (2 would be an encrypted one)."""
    raw = open(path, "rb").read()
    hdrsize = int.from_bytes(raw[2:4], "big")
    tag = int.from_bytes(raw[hdrsize:hdrsize + 2], "big")
    n = int.from_bytes(raw[hdrsize + 2:hdrsize + 6], "big")
    if tag != 1 or hdrsize + 6 + n > len(raw):
        die("%s: not a plain swtpm state (tag %d)" % (path, tag))
    return raw[hdrsize + 6:hdrsize + 6 + n]


def tpm_args():
    """(swtpm process or None, QEMU's -tpmdev arguments)"""
    if TPM == "libtpms":
        old = os.path.join(TPMDIR, "tpm2-00.permall")
        if not os.path.exists(TPMSTATE) and os.path.exists(old):
            open(TPMSTATE, "wb").write(swtpm_blob(old))
            log("libtpms: took over swtpm's TPM (%s)" % old)
        return None, ["-tpmdev", "libtpms,id=tpm0,state=" + TPMSTATE]
    swtpm, sock = start_swtpm()
    return swtpm, ["-chardev", "socket,id=chrtpm,path=" + sock,
                   "-tpmdev", "emulator,id=tpm0,chardev=chrtpm"]


REPORT_IMG = os.path.join(OUT, "report.img")


def report_disk():
    """Windows on Arm's stand-in for COM1: a FAT disk labelled REPORT,
    which spike.ps1 appends its lines to (w11.log)."""
    if not os.path.exists(REPORT_IMG):
        with open(REPORT_IMG, "wb") as f:
            f.truncate(32 << 20)
        subprocess.run(["mformat", "-i", REPORT_IMG, "-v", "REPORT", "-F", "::"], check=True)
    return ["-drive", "if=none,id=rep,format=raw,file=" + REPORT_IMG,
            "-device", "usb-storage,drive=rep,removable=on"]


class ReportLog(threading.Thread):
    """The report disk's w11.log, read from the host every few seconds;
    each new line goes to the same queue as COM1's, timestamped when it
    was seen."""

    def __init__(self, serial):
        super().__init__(daemon=True)
        self.serial, self.seen = serial, None

    def run(self):
        while True:
            time.sleep(3)
            out = subprocess.run(["mtype", "-i", REPORT_IMG, "::w11.log"],
                                 capture_output=True).stdout.decode(errors="replace")
            lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
            if self.seen is None:
                self.seen = len(lines)      # what a previous run left
                continue
            for text in lines[self.seen:]:
                t = time.monotonic() - self.serial.t0
                with open(os.path.join(OUT, "com1.log"), "a") as f:
                    f.write("%8.1f %s\n" % (t, text))
                self.serial.lines.put((t, text))
            self.seen = max(self.seen, len(lines))


class Serial(threading.Thread):
    """COM1 through a socket chardev; every line timestamped from t0."""

    def __init__(self, path, t0):
        super().__init__(daemon=True)
        self.path, self.t0, self.lines = path, t0, queue.Queue()

    def run(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                s.connect(self.path)
                break
            except OSError:
                time.sleep(0.1)
        buf = b""
        with open(os.path.join(OUT, "com1.log"), "a") as f:
            while True:
                data = s.recv(4096)
                if not data:
                    return
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    text = raw.decode(errors="replace").strip()
                    if not text:
                        continue
                    t = time.monotonic() - self.t0
                    f.write("%8.1f %s\n" % (t, text))
                    f.flush()
                    self.lines.put((t, text))


def run_qemu(cds):
    for name in ("qmp.sock", "qmp2.sock", "com1.sock"):
        p = os.path.join(OUT, name)
        if os.path.exists(p):
            os.unlink(p)
    swtpm, tpmdev = tpm_args()
    cpu = "host" if ACCEL == "kvm" else "max"
    if ARM:
        args = arm_args(tpmdev)
    else:
        args = x86_args(tpmdev, cpu)
    args += ["-chardev", "socket,id=com1,path=%s/com1.sock,server=on,wait=off" % OUT,
             "-serial", "chardev:com1",
             "-qmp", "unix:%s/qmp.sock,server=on,wait=off" % OUT,
             # a second monitor for hand clicks while this script holds the
             # first (tools/qmpc.py OUT/qmp2.sock click X Y 1280 800)
             "-qmp", "unix:%s/qmp2.sock,server=on,wait=off" % OUT]
    for i, iso in enumerate(cds):
        args += ["-drive", "if=none,id=cd%d,media=cdrom,readonly=on,file=%s" % (i, iso)]
        boot = ",bootindex=0" if i == 0 else ""
        if ARM and not OURS:
            # EDK2's aarch64 build has no AHCI driver: the CDs are USB
            # mass storage, which the firmware and setup both read
            args += ["-device", "usb-bot,id=ub%d" % i,
                     "-device", "scsi-cd,bus=ub%d.0,drive=cd%d%s" % (i, i, boot)]
        else:
            args += ["-device", "ide-cd,drive=cd%d,bus=ide.%d%s" % (i, i + (ARM and OURS), boot)]
    if ARM and DRIVERS and os.path.exists(DRIVERS):
        n = len(cds) + (ARM and OURS)
        args += ["-drive", "if=none,id=drv,media=cdrom,readonly=on,file=" + DRIVERS,
                 "-device", "ide-cd,drive=drv,bus=ide.%d" % n]
    if NET:
        nic = "virtio-net-pci" if ARM else "e1000e"
        netdev = "user,id=n0"
        if SMB:
            netdev += ",guestfwd=tcp:10.0.2.4:445-unix:" + SMB
        args += ["-netdev", netdev, "-device", nic + ",netdev=n0"]
    if CLIPBOARD:
        args += ["-chardev", "qemu-vdagent,id=vda,clipboard=on,mouse=off",
                 "-device", "virtio-serial-pci",
                 "-device", "virtserialport,chardev=vda,name=com.redhat.spice.0"]
    if ARM:
        # after the CDs: plugged first, the firmware made its one USB
        # boot entry for this disk and never tried the ISO
        args += report_disk()
    return start(args, swtpm)


def arm_args(tpmdev):
    """Windows 11 on Arm: the launcher's `virt` machine (M20 step 4). The
    AHCI controller is named `ide`, so the disk is ide.0 and the CDs
    follow from ide.1 (the controller has six ports)."""
    if OURS:
        disk = ["-device", "ich9-ahci,id=ide",
                "-device", "ide-hd,drive=d0,bus=ide.0,bootindex=1"]
    else:
        disk = ["-device", "nvme,drive=d0,serial=w11arm,bootindex=1"]
    return [QEMU, "-L", FW,
            "-machine", "virt,gic-version=3", "-accel", ACCEL,
            "-cpu", "max", "-smp", SMP, "-m", MEM,
            "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s/%s" % (ARM_FW, ARM_CODE),
            "-drive", "if=pflash,format=raw,unit=1,file=" + VARS,
            *tpmdev,
            "-device", "tpm-tis-device,tpmdev=tpm0" + ("" if TPM_PPI else ",ppi=off"),
            "-drive", "if=none,id=d0,format=qcow2,file=" + DISK,
            *disk,
            *[a for g in GPU.split(",") for a in ("-device", g)],
            "-device", "qemu-xhci", "-device", "usb-tablet", "-device", "usb-kbd",
            *([] if NET else ["-nic", "none"]),
            # HD Audio, the launcher's Windows 11 sound card
            "-audiodev", "none,id=snd",
            "-device", "ich9-intel-hda,addr=0x1b", "-device", "hda-duplex,audiodev=snd",
            "-display", "vnc=127.0.0.1:" + VNC if VNC else "none"]


def x86_args(tpmdev, cpu):
    return [QEMU, "-L", FW,
            "-machine", "q35,smm=on,accel=" + ACCEL,
            "-cpu", cpu, "-smp", SMP, "-m", MEM,
            "-global", "driver=cfi.pflash01,property=secure,value=on",
            "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s/edk2-x86_64-secure-code.fd" % FW,
            "-drive", "if=pflash,format=raw,unit=1,file=" + VARS,
            *tpmdev,
            "-device", "tpm-crb,tpmdev=tpm0",
            "-drive", "if=none,id=d0,format=qcow2,file=" + DISK,
            "-device", "nvme,drive=d0,serial=w11spike,bootindex=1",
            "-device", "qemu-xhci", "-device", "usb-tablet", "-device", "usb-kbd",
            *([] if NET else ["-nic", "none"]),
            "-vga", "std",
            "-display", "vnc=127.0.0.1:" + VNC if VNC else "none"]


def start(args, swtpm):
    with open(os.path.join(OUT, "qemu.cmd"), "w") as f:
        f.write(" ".join(args) + "\n")
    qlog = open(os.path.join(OUT, "qemu.log"), "a")
    t0 = time.monotonic()
    q = subprocess.Popen(args, stdout=qlog, stderr=subprocess.STDOUT)
    serial = Serial(os.path.join(OUT, "com1.sock"), t0)
    serial.start()
    if ARM:
        ReportLog(serial).start()
    qmp = None
    for _ in range(100):
        try:
            qmp = qmpc.connect(os.path.join(OUT, "qmp.sock"))
            qmpc.cmd(qmp, "qmp_capabilities")
            break
        except OSError:
            if q.poll() is not None:
                die("QEMU exited at once (OUT/qemu.log)")
            time.sleep(0.1)
    log("QEMU up, %s, %s vCPUs, %s MB, TPM %s; the screen is OUT/shots/latest.png"
        % (ACCEL, SMP, MEM, TPM))
    return q, swtpm, qmp, serial, t0


class Shots:
    def __init__(self, qmp, t0):
        self.qmp, self.t0, self.next = qmp, t0, 0.0
        os.makedirs(os.path.join(OUT, "shots"), exist_ok=True)

    def take(self, tag=None):
        t = time.monotonic() - self.t0
        png = os.path.join(OUT, "shots", tag or ("t%05d.png" % t))
        ppm = png + ".ppm"
        r = qmpc.cmd(self.qmp, "screendump", {"filename": ppm})
        if "error" in r:
            return None
        time.sleep(0.3)
        try:
            qmpc.ppm_to_png(ppm, png)
            os.unlink(ppm)
        except (OSError, AssertionError):
            return None
        shutil.copy(png, os.path.join(OUT, "shots", "latest.png"))
        return png

    def tick(self):
        if SHOT and time.monotonic() - self.t0 >= self.next:
            self.take()
            self.next += SHOT


def wait(q, serial, shots, until, timeout, keys_for=0):
    """Wait for a COM1 line starting with `until` (None: QEMU exiting).
    Returns (t, line) or None on timeout / exit."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if keys_for and time.monotonic() - shots.t0 < keys_for:
            # the ISO's "Press any key to boot from CD or DVD"
            qmpc.cmd(shots.qmp, "send-key", {"keys": [{"type": "qcode", "data": "ret"}]})
        try:
            t, line = serial.lines.get(timeout=0.5)
            log("COM1 %7.1fs  %s" % (t, line))
            if until and line.startswith(until):
                return t, line
        except queue.Empty:
            pass
        if q.poll() is not None:
            return None if until else (time.monotonic() - shots.t0, "exit")
        shots.tick()
    return None


def power_off(q, qmp):
    try:
        qmpc.cmd(qmp, "system_powerdown")
    except OSError:
        pass
    try:
        q.wait(180)
        return True
    except subprocess.TimeoutExpired:
        log("no answer to the power button in 180 s; quitting QEMU")
        qmpc.cmd(qmp, "quit")
        q.wait(30)
        return False


def install(iso):
    if os.path.exists(DISK):
        die("%s exists; a new install wants an empty OUT" % DISK)
    os.makedirs(OUT, exist_ok=True)
    lang = iso_language(iso)
    log("install %s (%s, %s)" % (iso, lang, EDITION))
    subprocess.run([os.path.join(os.path.dirname(QEMU), "qemu-img"), "create", "-q",
                    "-f", "qcow2", DISK, "80G"], check=True)
    shutil.copy(os.path.join(ARM_FW, ARM_VARS) if ARM else os.path.join(FW, "edk2-i386-vars.fd"), VARS)
    q, swtpm, qmp, serial, t0 = run_qemu([iso, unattend_iso(lang)])
    shots = Shots(qmp, t0)
    r = wait(q, serial, shots, "W11-INFO done", 3 * 3600, keys_for=40)
    if r is None:
        shots.take("fail.png")
        log("FAIL: no report from the guest (shots/fail.png, shots/latest.png)")
        power_off(q, qmp)
        return 1
    log("installed in %.1f min" % (r[0] / 60))
    exited = wait(q, serial, shots, None, 300)
    if exited is None:
        power_off(q, qmp)
    if swtpm:
        swtpm.wait(10)
    return 0


# Typed into the Run box (259 characters at most), run elevated.
REPORT_CMD = ("powershell -c \"$p=new-object System.IO.Ports.SerialPort COM1;$p.Open();"
              "$t=get-tpm;$e=(Get-TpmEndorsementKeyInfo -Hash sha256).PublicKeyHash;"
              "$p.WriteLine('W11-TPM ready='+$t.TpmReady+' owned='+$t.TpmOwned+' ek='+$e);"
              "$p.Close()\"")


def keys(qmp, *names):
    qmpc.send(qmp, list(names))


def click(qmp, x, y, w=1280, h=800):
    """The USB tablet's absolute pointer, then a left click."""
    move = [{"type": "abs", "data": {"axis": "x", "value": x * 32767 // w}},
            {"type": "abs", "data": {"axis": "y", "value": y * 32767 // h}}]
    qmpc.cmd(qmp, "input-send-event", {"events": move})
    time.sleep(0.15)
    for down in (True, False):
        qmpc.cmd(qmp, "input-send-event",
                 {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})
        time.sleep(0.1)


UAC_YES = (538, 546)    # 1280x800; focus starts on No
UAC_YES_ARM = (298, 444)    # 800x600: QEMU's aarch64 firmware's ramfb (FW_ARM=edk2)


PROBE_CMD = ("powershell -ep bypass -c \"& ((Get-Volume -FileSystemLabel REPORT).DriveLetter"
             "+':\\probe.ps1')\"")


def probe(q, qmp, serial, shots):
    """probe.ps1 from the REPORT disk, elevated (Windows on Arm, M20 step 4).
    A launch that writes no line at all (UAC's Yes missed, or the shell
    refused it: "Windows cannot access the specified device, path, or
    file" was seen once, M23) is launched again, after an Esc for the
    error box it may have left (Esc on a bare desktop does nothing)."""
    time.sleep(20)
    for launch in range(3):
        keys(qmp, "esc")
        time.sleep(1)
        keys(qmp, "meta_l", "r")
        time.sleep(3)
        type_text(qmp, PROBE_CMD)
        keys(qmp, "ctrl", "shift", "ret")
        started = None
        for attempt in range(3):
            time.sleep(10)
            shots.take("uac-%d-%d.png" % (launch, attempt))
            if OURS:
                click(qmp, *UAC_YES)            # our firmware's 1280x800
            else:
                click(qmp, *UAC_YES_ARM, w=800, h=600)
            started = wait(q, serial, shots, "W11-PROBE", 20)
            if started:
                break
        if started:
            if started[1].startswith("W11-PROBE done") or wait(q, serial, shots, "W11-PROBE done", PROBE_WAIT):
                return
            break
        shots.take("probe-nothing-%d.png" % launch)
        log("probe launch %d wrote nothing (shots/probe-nothing-%d.png); again" % (launch, launch))
    shots.take("probe-fail.png")
    log("FAIL: no W11-PROBE done (shots/uac-*.png, shots/probe-fail.png)")


def type_text(qmp, text):
    for ch in text:
        k = qmpc.KEYMAP.get(ch)
        if k is None:
            k = ("shift", ch.lower()) if ch.isupper() else ch
        keys(qmp, *(k if isinstance(k, tuple) else (k,)))


def report(q, qmp, serial, shots):
    """Windows' own view of the TPM, from an elevated PowerShell."""
    time.sleep(20)                       # let the desktop finish starting
    keys(qmp, "meta_l", "r")
    time.sleep(3)
    for ch in REPORT_CMD:
        k = qmpc.KEYMAP.get(ch)
        if k is None:
            k = ("shift", ch.lower()) if ch.isupper() else ch
        keys(qmp, *(k if isinstance(k, tuple) else (k,)))
    keys(qmp, "ctrl", "shift", "ret")
    # UAC's prompt can take a while; a click on its Yes where there is no
    # prompt lands on the desktop and does nothing
    r = None
    for attempt in range(4):
        time.sleep(10)
        shots.take("uac-%d.png" % attempt)
        click(qmp, *UAC_YES)
        r = wait(q, serial, shots, "W11-TPM", 20)
        if r:
            break
    if r is None:
        shots.take("report-fail.png")
        log("FAIL: no W11-TPM line (shots/uac-*.png, shots/report-fail.png)")


def boot():
    if not os.path.exists(DISK):
        die("no %s; run install first" % DISK)
    if ARM:
        report_disk()       # makes it in a fresh OUT
        if PROBE_PS1 == os.path.join(SPIKE, "probe.ps1"):
            subprocess.run(["mcopy", "-o", "-i", REPORT_IMG, PROBE_PS1, "::probe.ps1"], check=True)
        else:
            # another script runs under stub.ps1, which logs a refusal
            subprocess.run(["mcopy", "-o", "-i", REPORT_IMG, os.path.join(SPIKE, "stub.ps1"), "::probe.ps1"], check=True)
            subprocess.run(["mcopy", "-o", "-i", REPORT_IMG, PROBE_PS1, "::main.ps1"], check=True)
    q, swtpm, qmp, serial, t0 = run_qemu([])
    shots = Shots(qmp, t0)
    timeout = 900 if ACCEL == "kvm" else 3 * 3600
    r = wait(q, serial, shots, "W11-DESKTOP", timeout)
    if r is None:
        shots.take("fail.png")
        log("FAIL: no desktop in %d s (shots/fail.png)" % timeout)
        power_off(q, qmp)
        return 1
    log("desktop at %.1f s (%s)" % (r[0], ACCEL))
    if REPORT:
        report(q, qmp, serial, shots)
    if PROBE:
        probe(q, qmp, serial, shots)
    end = time.monotonic() + SETTLE
    while time.monotonic() < end:
        shots.tick()
        time.sleep(1)
    shots.take("desktop-%s.png" % ACCEL)
    t = time.monotonic()
    clean = power_off(q, qmp)
    log("power off %s in %.1f s" % ("clean" if clean else "FORCED", time.monotonic() - t))
    if swtpm:
        swtpm.wait(10)
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("install", "boot"):
        print(__doc__)
        return 2
    for tool in (("swtpm",) if TPM == "swtpm" else ()) + ("xorriso",) + (() if os.environ.get("LANG_ISO") or sys.argv[1] != "install" else ("7z",)):
        if not shutil.which(tool):
            die("%s not found" % tool)
    if ARM:
        os.makedirs(OUT, exist_ok=True)
        arm_firmware()
    if not os.access(QEMU, os.X_OK):
        die("no %s (ninja -C build/qemu qemu-system-%s)" % (QEMU, ARCH))
    if sys.argv[1] == "install":
        if len(sys.argv) != 3:
            die("install wants the ISO")
        return install(os.path.abspath(sys.argv[2]))
    return boot()


if __name__ == "__main__":
    sys.exit(main())
