#!/usr/bin/env python3
"""The libtpms TPM backend (patch 75, track M20), with no guest.

  tools/tpm-qtest.py [qemu-system-x86_64]

The i386 QEMU works as well (its q35 has the same tpm-crb), which is what
scripts/test.sh runs on a Mac, where there is no x86_64 target.

QEMU runs under qtest (`-accel qtest`): this script is the CPU. It drives
`tpm-crb`'s registers and data buffer the way QEMU's own
tests/qtest/tpm-util.c does, and sends raw TPM 2.0 commands to
`-tpmdev libtpms`. Checks:

  1. A fresh state file: Startup, GetRandom, a PCR extend whose value
     matches SHA-256 computed here, an NV index defined and written. The
     state file exists afterwards.
  2. A second QEMU on the same file: the NV index reads back, PCR 10 is
     zero again (Startup(CLEAR)). The TPM survived the restart.
  3. Snapshots: extend and `savevm`, then change the TPM (another extend,
     a new NV value), `loadvm`: both are back to the snapshot's. A third
     QEMU then reads the snapshot's NV value: the file followed loadvm.
  4. `info tpm` names the backend and its state file.
  5. A second `-tpmdev libtpms` in one process is refused.

Environment: OUT (default build/test/tpm-qtest), QEMU (or the argument).
Exit 0 when every check passes.
"""
import hashlib, os, shutil, socket, struct, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qmpc  # noqa: E402
import qemuhost  # noqa: E402  (Unix sockets, or loopback TCP on Windows)

QEMU = sys.argv[1] if len(sys.argv) > 1 else os.environ.get(
    "QEMU", os.path.join(ROOT, "build/qemu/qemu-system-x86_64"))
# absolute: Windows starts no program from a relative path with forward
# slashes (test.sh's build/win/qemu-msvc/...)
QEMU = os.path.abspath(QEMU)
OUT = os.path.abspath(os.environ.get("OUT", os.path.join(ROOT, "build/test/tpm-qtest")))
STATE = os.path.join(OUT, "tpm.permall")

CRB = 0xFED40000            # hw/acpi/tpm.h TPM_CRB_ADDR_BASE
LOC_CTRL = 0x08
CTRL_STS = 0x44
CTRL_START = 0x4C
CTRL_CMD_LADDR = 0x5C
CTRL_RSP_ADDR = 0x68

NV_INDEX = 0x01500020
RH_OWNER = 0x40000001
RS_PW = 0x40000009
PCR = 10

failures = []


def check(ok, what):
    print("%s  %s" % ("PASS" if ok else "FAIL", what), flush=True)
    if not ok:
        failures.append(what)
    return ok


class Qtest:
    """qtest's text protocol: one request line, one OK line."""

    def __init__(self, addr):
        for _ in range(100):
            try:
                self.s = qemuhost.connect(addr)
                break
            except OSError:
                time.sleep(0.05)
        self.f = self.s.makefile("rwb", buffering=0)

    def req(self, line):
        self.f.write((line + "\n").encode())
        while True:
            r = self.f.readline().decode().strip()
            if r.startswith("IRQ"):
                continue
            if not r.startswith("OK"):
                raise RuntimeError("qtest: %s -> %s" % (line, r))
            return r[3:]

    def readl(self, a):
        return int(self.req("readl 0x%x" % a), 16)

    def readq(self, a):
        return int(self.req("readq 0x%x" % a), 16)

    def writel(self, a, v):
        self.req("writel 0x%x 0x%x" % (a, v))

    def writeb(self, a, v):
        self.req("writeb 0x%x 0x%x" % (a, v))

    def memwrite(self, a, data):
        self.req("write 0x%x 0x%x 0x%s" % (a, len(data), data.hex()))

    def memread(self, a, n):
        return bytes.fromhex(self.req("read 0x%x 0x%x" % (a, n))[2:])


class Tpm:
    """TPM 2.0 commands through tpm-crb, as tpm_util_crb_transfer."""

    def __init__(self, qt):
        self.qt = qt

    def transfer(self, cmd):
        qt = self.qt
        caddr = qt.readq(CRB + CTRL_CMD_LADDR)
        raddr = qt.readq(CRB + CTRL_RSP_ADDR)
        qt.writeb(CRB + LOC_CTRL, 1)               # request locality 0
        qt.memwrite(caddr, cmd)
        qt.writel(CRB + CTRL_START, 1)
        end = time.monotonic() + 30                # a primary key takes seconds
        while qt.readl(CRB + CTRL_START) & 1:
            if time.monotonic() > end:
                raise RuntimeError("the TPM never finished a command")
            time.sleep(0.005)
        if qt.readl(CRB + CTRL_STS) & 1:
            raise RuntimeError("CRB reports a fatal TPM error")
        hdr = qt.memread(raddr, 10)
        tag, size, rc = struct.unpack(">HII", hdr)
        return rc, qt.memread(raddr, size)

    @staticmethod
    def command(tag, cc, body):
        return struct.pack(">HII", tag, 10 + len(body), cc) + body

    @staticmethod
    def pw_session():
        # authorization area: TPM_RS_PW, empty nonce, attributes 0, empty hmac
        s = struct.pack(">IHBH", RS_PW, 0, 0, 0)
        return struct.pack(">I", len(s)) + s

    def startup(self):
        return self.transfer(self.command(0x8001, 0x144, struct.pack(">H", 0)))[0]

    def get_random(self, n):
        rc, r = self.transfer(self.command(0x8001, 0x17B, struct.pack(">H", n)))
        return rc, r[12:12 + struct.unpack(">H", r[10:12])[0]] if rc == 0 else b""

    def pcr_extend(self, pcr, digest):
        body = struct.pack(">I", pcr) + self.pw_session()
        body += struct.pack(">IH", 1, 0x000B) + digest       # one SHA-256 digest
        return self.transfer(self.command(0x8002, 0x182, body))[0]

    def pcr_read(self, pcr):
        sel = bytearray(3)
        sel[pcr // 8] |= 1 << (pcr % 8)
        body = struct.pack(">IHB", 1, 0x000B, 3) + bytes(sel)
        rc, r = self.transfer(self.command(0x8001, 0x17E, body))
        if rc:
            return rc, b""
        # updateCounter, the selection echoed (4+2+1+3), digests count, size
        off = 10 + 4 + 10
        count = struct.unpack(">I", r[off:off + 4])[0]
        size = struct.unpack(">H", r[off + 4:off + 6])[0]
        return rc, r[off + 6:off + 6 + size] if count else b""

    def nv_define(self, index, size):
        attrs = (1 << 1) | (1 << 17)                         # OWNERWRITE | OWNERREAD
        pub = struct.pack(">IHIHH", index, 0x000B, attrs, 0, size)
        body = struct.pack(">I", RH_OWNER) + self.pw_session()
        body += struct.pack(">H", 0) + struct.pack(">H", len(pub)) + pub
        return self.transfer(self.command(0x8002, 0x12A, body))[0]

    def nv_write(self, index, data):
        body = struct.pack(">II", RH_OWNER, index) + self.pw_session()
        body += struct.pack(">H", len(data)) + data + struct.pack(">H", 0)
        return self.transfer(self.command(0x8002, 0x137, body))[0]

    def nv_read(self, index, size):
        body = struct.pack(">II", RH_OWNER, index) + self.pw_session()
        body += struct.pack(">HH", size, 0)
        rc, r = self.transfer(self.command(0x8002, 0x14E, body))
        if rc:
            return rc, b""
        n = struct.unpack(">H", r[14:16])[0]                  # after parameterSize
        return rc, r[16:16 + n]


def extended(old, digest):
    return hashlib.sha256(old + digest).digest()


LIVE = []    # every QEMU started, killed on the way out whatever happened


class Machine:
    def __init__(self, tpmdevs=1, name="run"):
        self.qtest_addr = qemuhost.addr(OUT, "qtest")
        self.qmp_addr = qemuhost.addr(OUT, "qmp")
        for a in (self.qtest_addr, self.qmp_addr):
            if os.path.exists(a):
                os.unlink(a)
        args = [QEMU, "-machine", "q35", "-accel", "qtest", "-nodefaults",
                "-display", "none",
                "-qtest", qemuhost.qemu_opt(self.qtest_addr),
                "-qmp", qemuhost.qemu_opt(self.qmp_addr),
                "-drive", "if=none,id=snap,format=qcow2,file=%s/snap.qcow2" % OUT]
        for i in range(tpmdevs):
            args += ["-tpmdev", "libtpms,id=tpm%d,state=%s" % (i, STATE)]
        args += ["-device", "tpm-crb,tpmdev=tpm0"]
        self.log = open(os.path.join(OUT, name + ".log"), "w")
        self.p = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        LIVE.append(self.p)
        self.qt = self.qmp = None

    def connect(self):
        self.qt = Qtest(self.qtest_addr)
        for _ in range(100):
            try:
                self.qmp = qmpc.connect(self.qmp_addr)
                qmpc.cmd(self.qmp, "qmp_capabilities")
                break
            except OSError:
                time.sleep(0.05)
        return Tpm(self.qt)

    def hmp(self, line):
        r = qmpc.cmd(self.qmp, "human-monitor-command", {"command-line": line})
        return r.get("return", str(r.get("error")))

    def quit(self):
        try:
            qmpc.cmd(self.qmp, "quit")
        except OSError:
            pass
        self.p.wait(30)
        self.log.close()


def main():
    if not os.access(QEMU, os.X_OK):
        print("no %s (ninja -C build/qemu qemu-system-x86_64)" % QEMU)
        return 2
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    # Windows resolves no qemu-img without its .exe here (the MSVC QEMU's)
    img = "qemu-img.exe" if os.name == "nt" else "qemu-img"
    subprocess.run([os.path.join(os.path.dirname(QEMU), img), "create", "-q",
                    "-f", "qcow2", os.path.join(OUT, "snap.qcow2"), "16M"], check=True)
    test_digest = b"test".ljust(32, b"\0")
    x = b"2ksbox-1"
    y = b"2ksbox-2"
    zero = bytes(32)

    # 1. a TPM that was never made
    m = Machine(name="run1")
    tpm = m.connect()
    check(tpm.startup() == 0, "fresh: Startup(CLEAR)")
    rc, rnd = tpm.get_random(16)
    check(rc == 0 and len(rnd) == 16, "fresh: GetRandom(16)")
    check(tpm.pcr_extend(PCR, test_digest) == 0, "fresh: PCR_Extend")
    rc, v = tpm.pcr_read(PCR)
    check(rc == 0 and v == extended(zero, test_digest), "fresh: PCR 10 = SHA-256(0 || digest)")
    check(tpm.nv_define(NV_INDEX, len(x)) == 0, "fresh: NV_DefineSpace")
    check(tpm.nv_write(NV_INDEX, x) == 0, "fresh: NV_Write")
    rc, v = tpm.nv_read(NV_INDEX, len(x))
    check(rc == 0 and v == x, "fresh: NV_Read")
    info = m.hmp("info tpm")
    check("type=libtpms" in info and ",state=%s" % STATE in info,
          "info tpm names libtpms and its state file")
    m.quit()
    check(os.path.getsize(STATE) > 0 if os.path.exists(STATE) else False,
          "the state file was written (%s)" % STATE)

    # 2. the same TPM in a new process
    m = Machine(name="run2")
    tpm = m.connect()
    check(tpm.startup() == 0, "restart: Startup(CLEAR)")
    rc, v = tpm.nv_read(NV_INDEX, len(x))
    check(rc == 0 and v == x, "restart: the NV index kept its value")
    rc, v = tpm.pcr_read(PCR)
    check(rc == 0 and v == zero, "restart: PCR 10 is zero after Startup(CLEAR)")

    # 3. snapshots
    tpm.pcr_extend(PCR, test_digest)
    a = extended(zero, test_digest)
    r = m.hmp("savevm s1")
    check(r.strip() == "", "savevm with the TPM (%s)" % (r.strip() or "ok"))
    tpm.pcr_extend(PCR, test_digest)
    tpm.nv_write(NV_INDEX, y)
    rc, v = tpm.nv_read(NV_INDEX, len(y))
    check(rc == 0 and v == y, "snapshot: the TPM changed after savevm")
    r = m.hmp("loadvm s1")
    check(r.strip() == "", "loadvm (%s)" % (r.strip() or "ok"))
    rc, v = tpm.pcr_read(PCR)
    check(rc == 0 and v == a, "loadvm: PCR 10 is the snapshot's")
    rc, v = tpm.nv_read(NV_INDEX, len(x))
    check(rc == 0 and v == x, "loadvm: the NV index is the snapshot's")
    m.quit()

    m = Machine(name="run3")
    tpm = m.connect()
    tpm.startup()
    rc, v = tpm.nv_read(NV_INDEX, len(x))
    check(rc == 0 and v == x, "after loadvm the state file holds the snapshot's TPM")
    m.quit()

    # 5. one per process
    m = Machine(tpmdevs=2, name="run4")
    try:
        m.p.wait(30)
    except subprocess.TimeoutExpired:
        m.p.kill()
    m.log.close()
    log = open(os.path.join(OUT, "run4.log")).read()
    # QEMU allows one -tpmdev of any type (system/tpm.c) before the
    # backend's own one-per-process check is reached
    check(m.p.returncode != 0 and ("Only one TPM is allowed" in log
                                   or "one TPM per process" in log),
          "a second -tpmdev libtpms is refused")

    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        rc = main()
    finally:
        for p in LIVE:
            if p.poll() is None:
                p.kill()
    sys.exit(rc)
