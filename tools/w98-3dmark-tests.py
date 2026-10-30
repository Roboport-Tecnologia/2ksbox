#!/usr/bin/env python3
"""Place a 3DMark 99 run's rate lines by test (tools/w98-3dmark.sh).

The executor's `ddi: N frames/s` and the device's `N page flips in X s`
lines cover windows on their own clocks. A line prints on the first frame
5 s after the previous one, so a window with no frames in it stretches
until the next frame, and its "frames/s" counts over mostly empty time.
That happens on a test's loading screen and in the "Synthetic CPU 3D
Speed" test, which presents about two frames a second. Such a 14 s
window was once misread as the first-person test, which had been at the
60 Hz cap all along (docs/tracks/m9-tcg-aarch64.md, the "Win98 3D"
correction).

So this script tells the tests apart from the screen. w98-3dmark.sh
screendumps every 5 s after the Benchmark click into tests/t<secs>.png,
and this classifies each shot by pixels as 3DMark's "Now testing" splash
(the orange logo block and the blue banner), the score dialog (the
project window's grey under the dialog's blue title bar), or a game frame
(anything else). The first-person test draws a green frame counter
top-left. The race is the run of game frames right before it, since the
two load too fast for a splash shot to separate them. A rate line counts
for a test only when its whole window lies inside that test's shots. A
run of shots survives one missing shot (a failed screendump), and the
longest run with the counter is the first-person test. The result goes
to tests.txt.

    tools/w98-3dmark-tests.py classify <shot.png.ppm>   -> fp | game | splash | score | other
    tools/w98-3dmark-tests.py report <run dir>          -> <run dir>/tests.txt, also printed
"""
import datetime
import glob
import os
import re
import sys


def load_ppm(path):
    d = open(path, "rb").read().split(b"\n", 3)
    w, h = (int(x) for x in d[1].split()[:2])
    return w, h, d[3]


def classify_ppm(path):
    w, h, p = load_ppm(path)
    if (w, h) != (800, 600):
        return "other"

    def px(x, y):
        o = (y * w + x) * 3
        return p[o], p[o + 1], p[o + 2]

    def orange(c):
        return c[0] > 200 and 90 <= c[1] <= 170 and c[2] < 80

    def blue(c):
        return c[2] > 100 and c[0] < 80 and c[1] < 120

    def grey(c):
        return 100 < c[0] < 215 and max(c) - min(c) < 20

    # the splash: the orange logo block (x 90..250, y 70..230) sampled at
    # three heights (a progress bar or the emblem's black bars cannot hide
    # all of them) and the blue banner right of it below its white title
    if (all(orange(px(100, y)) and orange(px(240, y)) for y in (110, 150, 190))
            and blue(px(500, 150)) and blue(px(500, 190))):
        return "splash"
    # 3DMark's project window: grey where the splash has its logo and where
    # a game frame is the scene, and the taskbar under it
    windowed = grey(px(100, 150)) and grey(px(500, 150)) and grey(px(400, 590))
    # the score dialog's blue title bar across it at y ~139
    if windowed and blue(px(400, 139)) and blue(px(300, 139)):
        return "score"
    if windowed and grey(px(700, 300)):
        return "other"          # the desktop / project window, no test running
    # the first-person test draws its own frame counter top-left in green,
    # blended over the scene (darker on a dark wall): count green-dominant
    # pixels in that box
    n = 0
    for x in range(5, 190, 2):
        for y in range(3, 38, 2):
            c = px(x, y)
            if c[1] > 60 and c[1] > c[0] + 40 and c[1] > c[2] + 40:
                n += 1
    return "fp" if n >= 30 else "game"


def report(run):
    click = float(open(os.path.join(run, "click.txt")).read().split()[0])
    shots = []
    for f in sorted(glob.glob(os.path.join(run, "tests", "t*.png.ppm"))):
        m = re.search(r"t(\d+)\.png\.ppm$", f)
        shots.append((int(m.group(1)), classify_ppm(f)))
    # the first-person test is the run of "fp" shots; the race is the run of
    # game shots right before it (the two load fast enough here that no
    # splash shot separates them); everything after is the synthetic tests
    runs = []                   # [kind, first shot s, last shot s]
    for s, c in shots:
        k = "fp" if c == "fp" else ("game" if c == "game" else None)
        if k is None:
            continue
        # shots are ~5 s apart; a screendump that failed (one once split
        # the first-person test into two runs, and the shorter was
        # reported) must not end a run, so a gap of one missing shot
        # still joins
        if runs and runs[-1][0] == k and s - runs[-1][2] <= 12:
            runs[-1][2] = s
        else:
            runs.append([k, s, s])
    tests = []
    # the longest run of "fp" shots is the test (a stray green frame
    # elsewhere would otherwise be it)
    fp = sorted((r for r in runs if r[0] == "fp"), key=lambda r: r[1] - r[2])
    if fp:
        i = runs.index(fp[0])
        if i > 0 and runs[i - 1][0] == "game":
            tests.append(("Game 1 (race)", runs[i - 1][1], runs[i - 1][2]))
        tests.append(("Game 2 (first person)", fp[0][1], fp[0][2]))
        later = [r for r in runs[i + 1:] if r[0] == "game"]
        if later:
            tests.append(("later tests (fill rate, texturing, ...)", later[0][1], later[-1][2]))
    lines = []                  # (end s, dt, kind, rate)
    for l in open(os.path.join(run, "qemu.log"), errors="replace"):
        m = re.search(r"ddi: ([0-9.]+) frames/s \(.* in ([0-9.]+) s\)", l)
        k = "ddi"
        if not m:
            m = re.search(r"(\d+) page flips in ([0-9.]+) s \(([0-9.]+)/s\)", l)
            k = "flips"
            if m:
                m = (m.group(3), m.group(2))
        else:
            m = (m.group(1), m.group(2))
        if not m:
            continue
        ts = datetime.datetime.fromisoformat(l.split()[0].replace("Z", "+00:00")).timestamp()
        lines.append((ts - click, float(m[1]), k, float(m[0])))
    out = ["seconds after the Benchmark click; shots every 5 s in tests/, "
           "classified by pixels (%s: f = first person by its green counter, "
           "g = other game frame, s = splash, o = desktop, S = score)"
           % os.path.basename(__file__),
           "shots: " + " ".join("%d:%s" % (s, {"fp": "f", "score": "S"}.get(c, c[0]))
                               for s, c in shots)]
    if not fp:
        out.append("the first-person test was never on screen (no shot with its green counter)")
    for name, a, b in tests:
        inside = {"ddi": [], "flips": []}
        edges = {"ddi": [], "flips": []}
        for end, dt, k, rate in lines:
            start = end - dt
            if start >= a - 0.5 and end <= b + 0.5:
                inside[k].append(rate)
            elif start >= a - 5.5 and end <= b + 5.5:
                edges[k].append(rate)
        def fmt(v):
            return " ".join("%.1f" % x for x in v) if v else "-"
        def mean(v):
            return "%.1f" % (sum(v) / len(v)) if v else "-"
        out.append("%-40s on screen %3d-%3d s: ddi %s | flips %s -> %s fps (ddi), %s (flips); "
                   "windows touching the edges: ddi %s | flips %s"
                   % (name, a, b, fmt(inside["ddi"]), fmt(inside["flips"]),
                      mean(inside["ddi"]), mean(inside["flips"]),
                      fmt(edges["ddi"]), fmt(edges["flips"])))
    if len([t for t in tests if t[0].startswith("Game")]) != 2:
        out.append("WARNING: the two game tests were not both seen; look at tests/")
    score = [s for s, c in shots if c == "score"]
    out.append("score dialog on screen from %d s (tests/t%03d.png)" % (score[0], score[0]) if score
               else "WARNING: no score dialog seen")
    text = "\n".join(out) + "\n"
    open(os.path.join(run, "tests.txt"), "w").write(text)
    sys.stdout.write(text)
    return 0 if len([t for t in tests if t[0].startswith("Game")]) == 2 and score else 1


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "classify":
        print(classify_ppm(sys.argv[2]))
    elif len(sys.argv) == 3 and sys.argv[1] == "report":
        sys.exit(report(sys.argv[2]))
    else:
        sys.exit(__doc__)
