#!/usr/bin/env python3
"""Windows 11 on Arm's desktop frames (track M22): whole frames, at the
guest's pace.

  tools/win11-frames-test.py [machine-dir]    default the library's win11

An installed Windows 11 on Arm launcher machine runs in the player
(player-mitsuami, its window opens) on a copy: a qcow2 overlay of its disk
and copies of its firmware variables and TPM state in OUT, no disc, no
network, the launcher's own arguments (`launcherx --print-args`). The
tool logs in (W11_PASSWORD, empty for none), and a script from a FAT disk
on USB holds the desktop idle, then drags a borderless magenta window
sideways as fast as it can. Meanwhile it counts the frames the player
publishes (PLAYER_REFRESH_LOG) and dumps virtio-gpu's surface over QMP.

PASS: no dump of the moving window is torn (its left edge is one column
from its top row to its bottom), at least 10 dumps caught it, the player
published at least 50 frames a second while it moved (it shows each guest
flush, embed v12; the refresh tick alone gave ~44), and afterwards, with
the guest idle, the player's own frame (a PLAYER_SHOT_EVERY shot of the
texture it uploads into) matches the surface within 0.5 % of its pixels:
the player uploads only what changed, so a missed rectangle stays stale. The idle
rate and the player's CPU in each phase are printed, not judged: the
guest decides the rate. PLAYER_FLUSH=0 in the environment is the A/B.

Environment: W11_PASSWORD (required: skips when unset, empty for an
account with none), OUT=build/w11f (short: a socket lives there),
PLAYER (default player-mitsuami's aarch64 build). A Mac on
Apple Silicon; about 4 minutes. Ends with the ACPI power button.
"""
import os, platform, re, shutil, signal, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qmpc  # noqa: E402

OUT = os.path.abspath(os.environ.get("OUT", os.path.join(ROOT, "build/w11f")))
PASSWORD = os.environ.get("W11_PASSWORD")
PLAYER = os.environ.get("PLAYER", os.path.join(
    ROOT, "player-mitsuami/target/qemu-aarch64/release/player-mitsuami"))
LX = os.path.join(ROOT, "target/release/launcherx")
QEMU_IMG = os.path.join(ROOT, "build/qemu/qemu-img")
SOCK = "/tmp/w11f-%d.sock" % os.getpid()
DRAG_PS1 = r"""
$r = (Get-Volume -FileSystemLabel REPORT | Select-Object -First 1).DriveLetter
function say($s) { Add-Content -Path "${r}:\w11.log" -Value "W11-FRAMES $s" }
say "phase idle"
Start-Sleep 15
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$f = New-Object Windows.Forms.Form
$f.FormBorderStyle = 'None'; $f.StartPosition = 'Manual'; $f.TopMost = $true
$f.BackColor = [Drawing.Color]::FromArgb(255, 0, 255); $f.ShowInTaskbar = $false
$f.Location = New-Object Drawing.Point(100, 200); $f.Size = New-Object Drawing.Size(600, 400)
$f.Show(); [Windows.Forms.Application]::DoEvents(); Start-Sleep 1
say "phase drag"
$dx = 16; $n = 0; $sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt 20) {
  $x = $f.Location.X + $dx; if ($x -gt 1000 -or $x -lt 50) { $dx = -$dx }
  $f.Location = New-Object Drawing.Point($x, 200); [Windows.Forms.Application]::DoEvents(); $n++
}
say ("moves {0} in {1:N1} s" -f $n, $sw.Elapsed.TotalSeconds)
$f.Close()
say "done"
"""


def log(msg):
    print(msg, flush=True)


def skip(why):
    log("win11-frames-test: " + why)
    sys.exit(77)


def bundle(machine):
    """The machine's copy in OUT: overlay, variables, TPM; no disc, no net."""
    src = open(os.path.join(machine, "machine.toml")).read()
    vals = dict(re.findall(r'^(\w+) = "(.*)"$', src, re.M))
    disk = os.path.join(OUT, "disk.qcow2")
    subprocess.run([QEMU_IMG, "create", "-q", "-f", "qcow2", "-b", vals["disk"], "-F", "qcow2", disk],
                   check=True)
    lines = []
    for line in src.splitlines():
        key = line.split(" = ")[0]
        if key in ("efi_vars", "tpm_state"):
            dst = os.path.join(OUT, os.path.basename(vals[key]))
            shutil.copy(vals[key], dst)
            line = '%s = "%s"' % (key, dst)
        elif key == "disk":
            line = 'disk = "%s"' % disk
        elif key == "disc":
            line = 'disc = ""'
        elif key == "network":
            line = "network = false"
        elif key == "shared_folder":
            continue
        lines.append(line)
    path = os.path.join(OUT, "machine.toml")
    open(path, "w").write("\n".join(lines) + "\n")
    return path


def report_disk():
    img = os.path.join(OUT, "report.img")
    with open(img, "wb") as f:
        f.truncate(32 << 20)
    subprocess.run(["mformat", "-i", img, "-v", "REPORT", "-F", "::"], check=True)
    ps1 = os.path.join(OUT, "drag.ps1")
    open(ps1, "w").write(DRAG_PS1)
    subprocess.run(["mcopy", "-i", img, ps1, "::drag.ps1"], check=True)
    return img


def guest_log(img):
    r = subprocess.run(["mtype", "-i", img, "::w11.log"], capture_output=True, text=True)
    return [l for l in r.stdout.splitlines() if l.startswith("W11-FRAMES")]


def refreshes(path):
    n = 0
    for l in open(path, errors="replace"):
        m = re.match(r"\[display\] refresh #(\d+)", l)
        if m:
            n = int(m.group(1))
    return n


def cpu_secs(pid):
    """The process's CPU time so far, in seconds (ps's [[dd-]hh:]mm:ss.cc)."""
    t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    secs = 0.0
    for part in t.replace("-", ":").split(":"):
        secs = secs * 60 + float(part)
    return secs


def dump(qmp, name):
    """virtio-gpu's surface as (width, height, rgb bytes), or None."""
    ppm = os.path.join(OUT, name + ".ppm")
    r = qmpc.cmd(qmp, "screendump", {"filename": ppm, "device": "vgpu"})
    if "error" in r:
        return None
    data = open(ppm, "rb").read()
    os.unlink(ppm)
    head, dims, _, raw = data.split(b"\n", 3)
    w, h = map(int, dims.split())
    return w, h, raw


MAGENTA = b"\xff\x00\xff"


def left_edges(w, h, raw):
    """Per row holding the magenta window, its leftmost pixel's x."""
    edges = []
    for y in range(h):
        row = raw[y * w * 3:(y + 1) * w * 3]
        i = row.find(MAGENTA)
        while i >= 0 and i % 3:
            i = row.find(MAGENTA, i + 1)
        if i >= 0 and row[i:i + 300 * 3] == MAGENTA * 300:
            edges.append((y, i // 3))
    return edges


def main():
    if platform.system() != "Darwin" or platform.machine() != "arm64":   # os.uname() is POSIX only
        skip("Windows 11 on Arm under HVF only, for now")
    machine = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.expanduser("~/Library/Application Support/2ksbox"), "machines", "win11")
    for f in (LX, PLAYER, QEMU_IMG, os.path.join(machine, "machine.toml")):
        if not os.path.exists(f):
            skip("no " + f)
    if not shutil.which("mformat"):
        skip("no mtools")
    if PASSWORD is None:
        skip("W11_PASSWORD unset (the machine's account password; empty for none)")
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    toml = bundle(machine)
    subprocess.run([LX, "--prepare", toml], check=True, capture_output=True)
    argv = subprocess.run([LX, "--print-args", toml], check=True, capture_output=True,
                          text=True).stdout.split()
    gpu = [i for i, a in enumerate(argv) if a.startswith("virtio-gpu-pci")]
    if not gpu:
        skip("the machine has no virtio-gpu (not Windows 11 on Arm)")
    argv[gpu[0]] += ",id=vgpu"
    img = report_disk()
    argv += ["-drive", "if=none,id=rep,format=raw,file=" + img,
             "-device", "usb-storage,drive=rep,removable=on",
             "-qmp", "unix:%s,server,nowait" % SOCK]
    open(os.path.join(OUT, "qemu.args"), "w").write(" ".join(argv) + "\n")
    plog = os.path.join(OUT, "player.log")
    # a shot is a whole frame PNG-encoded on the player's main thread, which
    # stalls the picture: rare, for the comparison at the end only
    env = dict(os.environ, PLAYER_REFRESH_LOG="1", PLAYER_SHOT_EVERY="600",
               PLAYER_SHOT_DIR=os.path.join(OUT, "shots"))
    p = subprocess.Popen([PLAYER, "--"] + argv, stdout=open(plog, "w"), stderr=subprocess.STDOUT, env=env)
    log("player %d, log %s" % (p.pid, plog))
    ok = False
    try:
        ok = run(p, img, plog)
    finally:
        power_off(p)
        if os.path.exists(SOCK):
            os.unlink(SOCK)
    log("PASS" if ok else "FAIL")
    return 0 if ok else 1


def type_text(qmp, text):
    for ch in text:
        k = qmpc.KEYMAP.get(ch)
        if k is None:
            k = ("shift", ch.lower()) if ch.isupper() else ch
        qmpc.send(qmp, list(k) if isinstance(k, tuple) else [k])


def connect():
    for _ in range(60):
        try:
            q = qmpc.connect(SOCK)
            qmpc.cmd(q, "qmp_capabilities")
            return q
        except OSError:
            time.sleep(1)
    raise SystemExit("FAIL: no QMP socket")


def run(p, img, plog):
    # QMP serves one client at a time: this connection must be closed
    # whatever happens, or power_off's waits for a greeting forever
    qmp = connect()
    try:
        return measure(p, img, plog, qmp)
    finally:
        qmp.close()


def measure(p, img, plog, qmp):
    # viogpudo takes the screen once Windows runs: virtio-gpu's surface
    # turns from its placeholder to the lock screen's photograph (the
    # firmware's logo on black has few distinct bytes)
    t0 = time.time()
    while time.time() - t0 < 300:
        if p.poll() is not None:
            log("FAIL: the player exited (%s)" % plog)
            return False
        d = dump(qmp, "boot")
        if d and d[0] > 800 and len(set(d[2][::4099])) > 64:
            break
        time.sleep(5)
    else:
        log("FAIL: no Windows screen on virtio-gpu in 300 s")
        return False
    log("lock screen at %.0f s" % (time.time() - t0))
    time.sleep(15)
    qmpc.send(qmp, ["spc"])
    time.sleep(4)
    type_text(qmp, PASSWORD)
    qmpc.send(qmp, ["ret"])
    time.sleep(60)  # the desktop, and the work Windows does after a logon
    qmpc.send(qmp, ["meta_l", "r"])
    time.sleep(3)
    cmd = "powershell -ep bypass -w hidden -c \"& ((Get-Volume -FileSystemLabel REPORT).DriveLetter+':\\drag.ps1')\""
    type_text(qmp, cmd)
    qmpc.send(qmp, ["ret"])
    marks, dumps, t0 = {}, [], time.time()
    while time.time() - t0 < 120:
        lines = guest_log(img)
        for l in lines:
            m = re.match(r"W11-FRAMES phase (\w+)", l)
            if m and m.group(1) not in marks:
                marks[m.group(1)] = (time.time(), refreshes(plog), cpu_secs(p.pid))
        if any(l.startswith("W11-FRAMES done") for l in lines):
            break
        if "drag" in marks and len(dumps) < 40:
            d = dump(qmp, "drag")
            if d:
                dumps.append(d)
            continue
        time.sleep(1)
    else:
        log("FAIL: the guest script did not finish (no W11-FRAMES done; %s)" % img)
        return False
    end = (time.time(), refreshes(plog), cpu_secs(p.pid))
    for l in guest_log(img):
        log("  guest: " + l)
    if "idle" not in marks or "drag" not in marks:
        log("FAIL: no phase marks")
        return False
    idle_fps = (marks["drag"][1] - marks["idle"][1]) / (marks["drag"][0] - marks["idle"][0])
    drag_fps = (end[1] - marks["drag"][1]) / (end[0] - marks["drag"][0])
    log("published: idle %.1f fps, drag %.1f fps (100-frame counter, +-%.0f)"
        % (idle_fps, drag_fps, 100 / (end[0] - marks["drag"][0])))
    span = lambda a, b: 100 * (b[2] - a[2]) / (b[0] - a[0])
    log("player CPU: idle %.0f%%, drag %.0f%% (of one core; the drag includes the dumps)"
        % (span(marks["idle"], marks["drag"]), span(marks["drag"], end)))
    caught = torn = 0
    for i, (w, h, raw) in enumerate(dumps):
        edges = left_edges(w, h, raw)[8:-8]   # the corners may be rounded
        if len(edges) < 100:
            continue
        caught += 1
        xs = sorted(set(x for _, x in edges))
        if xs[-1] - xs[0] > 1:
            torn += 1
            if torn <= 3:
                log("  dump %d torn: left edge at x %s" % (i, xs[:6]))
                open(os.path.join(OUT, "torn-%d.ppm" % i), "wb").write(
                    b"P6\n%d %d\n255\n" % (w, h) + raw)
    log("dumps of the moving window: %d, torn: %d" % (caught, torn))
    ok = True
    if caught < 10:
        log("FAIL: fewer than 10 dumps caught the window")
        ok = False
    if torn:
        log("FAIL: %d torn dump(s) (OUT/torn-*.ppm)" % torn)
        ok = False
    if drag_fps < 50:
        log("FAIL: %.1f fps published while the window moved (want 50)" % drag_fps)
        ok = False
    # what the player shows is QEMU's surface: the player uploads only what
    # changed (M22), so a rectangle it missed stays stale on screen
    shown = shown_vs_surface(qmp)
    if shown is None:
        log("FAIL: no player shot to compare with the surface (OUT/shots)")
        ok = False
    else:
        diff, total = shown
        log("player's frame vs virtio-gpu's surface, idle: %d of %d pixels differ" % (diff, total))
        if diff > total // 200:
            log("FAIL: the player's frame is stale (OUT/shown.bmp, OUT/surface.ppm)")
            ok = False
    return ok


def bmp_rgb(path):
    """A BMP as (width, height, RGB bytes top-down)."""
    b = open(path, "rb").read()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    bpp = int.from_bytes(b[28:30], "little")
    n = bpp // 8
    stride = (w * n + 3) & ~3
    rows = []
    for y in range(abs(h)):
        row = b[off + y * stride:off + y * stride + w * n]
        rows.append(bytes(c for i in range(0, w * n, n) for c in (row[i + 2], row[i + 1], row[i])))
    if h > 0:
        rows.reverse()
    return w, abs(h), b"".join(rows)


def shown_vs_surface(qmp):
    """With the guest idle: the player's next guest-frame shot (the texture
    it uploads into, PLAYER_SHOT_EVERY) against virtio-gpu's surface just
    after it. (differing pixels, all pixels), or None without a shot."""
    shots = os.path.join(OUT, "shots")
    time.sleep(3)
    before = set(os.listdir(shots)) if os.path.isdir(shots) else set()
    for _ in range(240):
        new = sorted(set(os.listdir(shots)) - before) if os.path.isdir(shots) else []
        if new:
            break
        time.sleep(0.25)
    else:
        return None
    d = dump(qmp, "surface")
    if not d:
        return None
    sw, sh, sraw = d
    open(os.path.join(OUT, "surface.ppm"), "wb").write(b"P6\n%d %d\n255\n" % (sw, sh) + sraw)
    # the player may still be writing the PNG (a whole frame takes a while)
    bmp = os.path.join(OUT, "shown.bmp")
    for _ in range(10):
        time.sleep(1)
        r = subprocess.run(["sips", "-s", "format", "bmp", os.path.join(shots, new[-1]), "--out", bmp],
                           capture_output=True, text=True)
        if r.returncode == 0:
            break
    else:
        log("  sips could not read %s: %s" % (new[-1], r.stderr.strip()[-200:]))
        return None
    w, h, raw = bmp_rgb(bmp)
    if (w, h) != (sw, sh):
        log("  the shot is %dx%d, the surface %dx%d" % (w, h, sw, sh))
        return (w * h, w * h)
    diff = 0
    row = w * 3
    for y in range(h):
        a, b = raw[y * row:(y + 1) * row], sraw[y * row:(y + 1) * row]
        if a != b:
            diff += sum(1 for x in range(0, row, 3) if a[x:x + 3] != b[x:x + 3])
    return diff, w * h


def power_off(p):
    if p.poll() is not None:
        return
    # never wait on the monitor for long: a client still holding it would
    # keep the greeting from ever coming
    signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError()))
    signal.alarm(10)
    try:
        q = qmpc.connect(SOCK)
        qmpc.cmd(q, "qmp_capabilities")
        qmpc.cmd(q, "system_powerdown")
        q.close()
    except (OSError, TimeoutError):
        log("no answer on QMP; the power button not pressed")
    finally:
        signal.alarm(0)
    for _ in range(180):
        if p.poll() is not None:
            return
        time.sleep(1)
    log("no answer to the power button in 180 s; stopping the player")
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(15)
    except subprocess.TimeoutExpired:
        p.send_signal(signal.SIGKILL)
        p.wait()


if __name__ == "__main__":
    sys.exit(main())
