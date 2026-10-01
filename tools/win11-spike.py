#!/usr/bin/env python3
"""M20 step 1: Windows 11 on our x86_64 QEMU with EDK2 and a TPM 2.0.

  tools/win11-spike.py install <win11-x64.iso>   unattended install, then a report
  tools/win11-spike.py boot                      boot the installed disk, time the desktop

Stock Windows 11 setup with no bypass keys: its TPM, Secure Boot and CPU
checks run. The TPM is the distro's swtpm for now (step 2 replaces it with
libtpms inside QEMU). The disk, firmware variables and TPM state live in
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

Ends a run with the ACPI power button, never a kill, unless Windows does
not answer it.
"""
import os, queue, shutil, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qmpc  # noqa: E402

OUT = os.environ.get("OUT", "/mnt/data2/david/w11")
ACCEL = os.environ.get("ACCEL", "kvm")
SMP = os.environ.get("SMP", "4")
MEM = os.environ.get("MEM", "4096")
EDITION = os.environ.get("EDITION", "Windows 11 Pro")
QEMU = os.environ.get("QEMU", os.path.join(ROOT, "build/qemu/qemu-system-x86_64"))
FW = os.environ.get("FW", os.path.join(ROOT, "build/qemu/qemu-bundle/usr/local/share/qemu"))
VNC = os.environ.get("VNC", "")
SHOT = int(os.environ.get("SHOT", "60"))
SETTLE = int(os.environ.get("SETTLE", "60"))
SPIKE = os.path.join(ROOT, "tools/win11-spike")

DISK = os.path.join(OUT, "disk.qcow2")
VARS = os.path.join(OUT, "vars.fd")
TPMDIR = os.path.join(OUT, "tpm")


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


def unattend_iso(lang):
    d = os.path.join(OUT, "unattend")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    xml = open(os.path.join(SPIKE, "autounattend.xml")).read()
    xml = xml.replace("@LANG@", lang).replace("@EDITION@", EDITION)
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
    swtpm, tpmsock = start_swtpm()
    cpu = "host" if ACCEL == "kvm" else "max"
    args = [QEMU, "-L", FW,
            "-machine", "q35,smm=on,accel=" + ACCEL,
            "-cpu", cpu, "-smp", SMP, "-m", MEM,
            "-global", "driver=cfi.pflash01,property=secure,value=on",
            "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s/edk2-x86_64-secure-code.fd" % FW,
            "-drive", "if=pflash,format=raw,unit=1,file=" + VARS,
            "-chardev", "socket,id=chrtpm,path=" + tpmsock,
            "-tpmdev", "emulator,id=tpm0,chardev=chrtpm",
            "-device", "tpm-crb,tpmdev=tpm0",
            "-drive", "if=none,id=d0,format=qcow2,file=" + DISK,
            "-device", "nvme,drive=d0,serial=w11spike,bootindex=1",
            "-device", "qemu-xhci", "-device", "usb-tablet", "-device", "usb-kbd",
            "-nic", "none",
            "-vga", "std",
            "-display", "vnc=127.0.0.1:" + VNC if VNC else "none",
            "-chardev", "socket,id=com1,path=%s/com1.sock,server=on,wait=off" % OUT,
            "-serial", "chardev:com1",
            "-qmp", "unix:%s/qmp.sock,server=on,wait=off" % OUT,
            # a second monitor for hand clicks while this script holds the
            # first (tools/qmpc.py OUT/qmp2.sock click X Y 1280 800)
            "-qmp", "unix:%s/qmp2.sock,server=on,wait=off" % OUT]
    for i, iso in enumerate(cds):
        args += ["-drive", "if=none,id=cd%d,media=cdrom,readonly=on,file=%s" % (i, iso),
                 "-device", "ide-cd,drive=cd%d,bus=ide.%d%s" % (i, i, ",bootindex=0" if i == 0 else "")]
    with open(os.path.join(OUT, "qemu.cmd"), "w") as f:
        f.write(" ".join(args) + "\n")
    qlog = open(os.path.join(OUT, "qemu.log"), "a")
    t0 = time.monotonic()
    q = subprocess.Popen(args, stdout=qlog, stderr=subprocess.STDOUT)
    serial = Serial(os.path.join(OUT, "com1.sock"), t0)
    serial.start()
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
    log("QEMU up, %s, %s vCPUs, %s MB; the screen is OUT/shots/latest.png" % (ACCEL, SMP, MEM))
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
    shutil.copy(os.path.join(FW, "edk2-i386-vars.fd"), VARS)
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
    swtpm.wait(10)
    return 0


def boot():
    if not os.path.exists(DISK):
        die("no %s; run install first" % DISK)
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
    end = time.monotonic() + SETTLE
    while time.monotonic() < end:
        shots.tick()
        time.sleep(1)
    shots.take("desktop-%s.png" % ACCEL)
    t = time.monotonic()
    clean = power_off(q, qmp)
    log("power off %s in %.1f s" % ("clean" if clean else "FORCED", time.monotonic() - t))
    swtpm.wait(10)
    return 0


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("install", "boot"):
        print(__doc__)
        return 2
    for tool in ("swtpm", "xorriso", "7z"):
        if not shutil.which(tool):
            die("%s not found" % tool)
    if not os.access(QEMU, os.X_OK):
        die("no %s (ninja -C build/qemu qemu-system-x86_64)" % QEMU)
    if sys.argv[1] == "install":
        if len(sys.argv) != 3:
            die("install wants the ISO")
        return install(os.path.abspath(sys.argv[2]))
    return boot()


if __name__ == "__main__":
    sys.exit(main())
