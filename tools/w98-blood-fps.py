#!/usr/bin/env python3
"""Blood's frame rate out of QEMU's vga_vbe_write trace (tools/w98-blood.sh).

Every index-9 write (VBE display start: Build's page flip) is a frame.
Buckets them per second of the run, t+0 being the harness's run clock
("t+0 = HH:MM:SS UTC" in the run log), so a row lines up with the
screendump shots/t<secs>.png of the same second.

    tools/w98-blood-fps.py <run dir> [from to]   -> rows; with a window, its mean
"""
import datetime
import os
import re
import sys

run = sys.argv[1]
log = open(os.path.join(run, "..", os.path.basename(run) + ".run.log"), errors="replace").read()
m = re.search(r"t\+0 = (\d\d):(\d\d):(\d\d) UTC", log)
writes = []
for l in open(os.path.join(run, "qemu.log"), errors="replace"):
    # QEMU 9.2's log backend: "pid@epoch:event"; 11.1 drops that prefix
    # and -msg timestamp=on puts the ISO time in front instead
    t = re.match(r"\d+@(\d+\.\d+):vga_vbe_write index 0x9,", l)
    if t:
        writes.append(float(t.group(1)))
        continue
    t = re.match(r"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)Z vga_vbe_write index 0x9,", l)
    if t:
        writes.append(datetime.datetime.fromisoformat(t.group(1))
                      .replace(tzinfo=datetime.timezone.utc).timestamp())
if not writes:
    sys.exit("no vga_vbe_write index 9 in qemu.log (was -trace vga_vbe_write on?)")
day = datetime.datetime.fromtimestamp(writes[0], datetime.timezone.utc).date()
t0 = datetime.datetime.combine(day, datetime.time(int(m.group(1)), int(m.group(2)), int(m.group(3))),
                               datetime.timezone.utc).timestamp()
if writes[0] < t0 - 3600:       # the run clock crossed midnight
    t0 -= 86400
per = {}
for w in writes:
    s = int(w - t0)
    per[s] = per.get(s, 0) + 1
shots = sorted(int(x[1:4]) for x in os.listdir(os.path.join(run, "shots")) if re.match(r"t\d{3}\.png$", x))
print("sec  frames  shot")
for s in range(min(per), max(per) + 1):
    near = min(shots, key=lambda x: abs(x - s)) if shots else -1
    print("%3d  %6d  t%03d" % (s, per.get(s, 0), near))
if len(sys.argv) == 4:
    a, b = int(sys.argv[2]), int(sys.argv[3])
    n = sum(per.get(s, 0) for s in range(a, b))
    print("window %d-%d s: %d frames, %.1f fps" % (a, b, n, n / (b - a)))
