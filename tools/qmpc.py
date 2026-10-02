#!/usr/bin/env python3
"""Tiny QMP client: qmpc.py <socket> <cmd> [args...]
  screendump <out.png>         -> PPM via QMP, converted to PNG (pure python)
  keys <k1> <k2> ...           -> send-key one at a time (QKeyCode names; 'a+b' = chord)
  type <text>                  -> types ASCII text (letters, digits, \\ : . / space _ - & ( ) , ; = ' \" * % + ! > < | ~ ` ^ @ # $ [ ] { } ?; US layout)
  click <x> <y> [w h]          -> absolute pointer (usb-tablet) to x,y of a w×h screen (default 640×480), left click
  relclick <x> <y>             -> PS/2 mouse: walk the pointer to x,y in paced 3-pixel steps, reading it
                                  back from d3dpt-vga's cursor registers (blind from a corner on cirrus), left click
  json <json>                  -> raw request
"""
import json, os, socket, struct, sys, time, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemuhost

def connect(path):
    # a Unix socket's path, or tcp:127.0.0.1:<port> on Windows (qemuhost.py)
    s = qemuhost.connect(path)
    f = s.makefile("rwb", buffering=0)
    greet = json.loads(f.readline())
    assert "QMP" in greet
    return f

def cmd(f, execute, arguments=None):
    req = {"execute": execute}
    if arguments:
        req["arguments"] = arguments
    f.write((json.dumps(req) + "\n").encode())
    while True:
        line = json.loads(f.readline())
        if "event" in line:
            continue
        return line

def ppm_to_png(ppm, png):
    data = open(ppm, "rb").read()
    # P6\nW H\n255\n
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split())
    raw = parts[3]
    rows = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    open(png, "wb").write(out)
    return w, h

KEYMAP = {" ": "spc", "\\": "backslash", ":": ("shift", "semicolon"), ".": "dot",
          "_": ("shift", "minus"), "-": "minus", "/": "slash", "\n": "ret",
          "&": ("shift", "7"), "(": ("shift", "9"), ")": ("shift", "0"), ",": "comma",
          ";": "semicolon", "=": "equal", "'": "apostrophe", '"': ("shift", "apostrophe"),
          "*": ("shift", "8"), "%": ("shift", "5"), "+": ("shift", "equal"), "!": ("shift", "1"),
          ">": ("shift", "dot"), "<": ("shift", "comma"), "|": ("shift", "backslash"),
          "~": ("shift", "grave_accent"), "`": "grave_accent", "^": ("shift", "6"), "@": ("shift", "2"),
          "#": ("shift", "3"), "$": ("shift", "4"), "[": "bracket_left", "]": "bracket_right",
          "{": ("shift", "bracket_left"), "}": ("shift", "bracket_right"), "?": ("shift", "slash")}
# US-International guests (the Brazilian XP images): " ' ^ ~ ` are dead keys
# there; a following space yields the character itself, so type '~ 1' for
# '~1' only on a US layout. Prefer 8.3 names with ~ (dead key + digit = both).

def send(f, names, hold=None):
    # QMPC_HOLD=<ms> lengthens the press: a DOS game polling the keyboard
    # once a frame under TCG misses a 60 ms tap (Blood's demo ignored Esc
    # at 60 ms and took it at 300); Windows' queue does not care
    if hold is None:
        hold = int(os.environ.get("QMPC_HOLD", "60"))
    keys = [{"type": "qcode", "data": n} for n in names]
    r = cmd(f, "send-key", {"keys": keys, "hold-time": hold})
    if "error" in r:
        print("send-key", names, r["error"])
    time.sleep(0.12)

def main():
    path, what = sys.argv[1], sys.argv[2]
    f = connect(path)
    cmd(f, "qmp_capabilities")
    if what == "screendump":
        ppm = sys.argv[3] + ".ppm"
        r = cmd(f, "screendump", {"filename": ppm})
        if "error" in r:
            print(r); return 1
        time.sleep(0.3)
        print("screendump %dx%d -> %s" % (*ppm_to_png(ppm, sys.argv[3]), sys.argv[3]))
    elif what == "keys":
        for k in sys.argv[3:]:
            send(f, k.split("+"))
    elif what == "type":
        for ch in sys.argv[3]:
            k = KEYMAP.get(ch)
            if k is None:
                k = ("shift", ch.lower()) if ch.isupper() else ch
            send(f, list(k) if isinstance(k, tuple) else [k])
    elif what == "click":
        x, y = int(sys.argv[3]), int(sys.argv[4])
        w, h = (int(sys.argv[5]), int(sys.argv[6])) if len(sys.argv) > 6 else (640, 480)
        move = [{"type": "abs", "data": {"axis": "x", "value": x * 32767 // w}},
                {"type": "abs", "data": {"axis": "y", "value": y * 32767 // h}}]
        cmd(f, "input-send-event", {"events": move})
        time.sleep(0.15)
        for down in (True, False):
            r = cmd(f, "input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})
            if "error" in r:
                print("click", r["error"])
            time.sleep(0.1)
    elif what == "relclick":
        # A machine with no tablet has only the relative PS/2 mouse. Two
        # things make a blind walk land elsewhere: Windows 9x doubles any
        # step past its acceleration threshold (6 px by default), and QEMU's
        # PS/2 mouse *accumulates* deltas the guest has not read yet into one
        # packet, so a burst of small steps reaches a slow TCG guest as a few
        # big ones, and a burst long enough overflows the queue and the
        # guest's mouse stream desyncs for good. So: 3-px steps, paced, and
        # on our own adapter the pointer is read back from the CURSOR_X / Y
        # registers after every step (the sprite is the driver's, so what
        # the registers say is where Windows put it) and the walk corrects
        # itself. On another adapter it is blind but still paced.
        x, y = int(sys.argv[3]), int(sys.argv[4])
        def rel(dx, dy):
            cmd(f, "input-send-event", {"events": [{"type": "rel", "data": {"axis": "x", "value": dx}},
                                                   {"type": "rel", "data": {"axis": "y", "value": dy}}]})
        def hmp(line):
            return cmd(f, "human-monitor-command", {"command-line": line}).get("return", "")
        regs = None
        pci = hmp("info pci").split("\n")
        for i, l in enumerate(pci):
            if "1234:3d00" in l:
                for m in pci[i:i + 8]:
                    if "BAR1:" in m:
                        regs = int(m.split(" at ")[1].split()[0], 16)
                break
        def cur():
            if regs is None:
                return None
            r = hmp("xp /2wx 0x%x" % (regs + 0xa8)).split(":")[1].split()
            return int(r[0], 16), int(r[1], 16)
        def blind():
            # into the top-left corner (Windows clamps), then out by steps
            for _ in range(30):
                rel(-100, -100); time.sleep(0.15)
            cx, cy = 0, 0
            while cx < x or cy < y:
                dx, dy = min(3, x - cx), min(3, y - cy)
                rel(dx, dy); cx += dx; cy += dy
                time.sleep(0.15)
        if regs is None:
            blind()
        else:
            # The registers say where the *sprite* is, and a game that hides
            # the Windows pointer and draws its own (Total Annihilation's
            # menu) stops them moving: after a few steps with no change the
            # walk goes blind rather than correcting against a stale value
            # for 400 steps.
            rel(1, 1); time.sleep(0.3)         # the sprite follows the first move
            first = cur(); moved = False
            for i in range(400):
                cx, cy = cur()
                if (cx, cy) != first:
                    moved = True
                if (cx, cy) == (x, y):
                    break
                if i >= 8 and not moved:
                    print("pointer registers not following (the pointer is hidden?): walking blind")
                    blind()
                    break
                rel(max(-3, min(3, x - cx)), max(-3, min(3, y - cy)))
                time.sleep(0.15)
            print("pointer at", cur())
        time.sleep(0.3)
        for down in (True, False):
            r = cmd(f, "input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})
            if "error" in r:
                print("relclick", r["error"])
            time.sleep(0.12)
    elif what == "json":
        r = cmd(f, **json.loads(sys.argv[3]))
        print(json.dumps(r))
    return 0

if __name__ == "__main__":
    sys.exit(main())
