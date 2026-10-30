#!/usr/bin/env python3
"""Read a guest run of Wine's Direct3D tests (track M16) and compare it.

    tools/winetest-summary.py <dir>                      a table per test file
    tools/winetest-summary.py <dir> --save <baseline>    write the run as a baseline
    tools/winetest-summary.py <dir> --baseline <file>    exit 1 on anything worse
    ... --baseline <file> --by-function <src>             the worse keys counted per test
                                                         function of the sources in <src>
    ... --baseline <file> --split <other> [--by-function <src>]
                                                         the worse keys in two halves: the
                                                         ones <other> fails too, the rest

<dir> holds what WTRUN.EXE wrote (guest-tools/src/winetest/wtrun.c):
<exe>_<test>.txt per test file and WTRUN.LOG. A baseline
(reference/winetest/*.txt) is one line per failing check, keyed by source
line, which is stable for one pinned Wine tag:

    d3d9_test_visual visual.c:1234 2      a check that failed twice
    d3d9_test_device crash                the test file died
    d3d9_test_visual timeout              WTRUN killed it

"Worse" is a key the baseline lacks, or a key failing more often than
there. The rig's baseline is the oracle: nothing may fail on the driver
that passes on real hardware. reference/winetest/dxvk-wine.txt is DXVK's
own (tools/winetest-dxvk.sh): a key worse than there is the driver's or
the executor's, the rest DXVK's. With --by-function <src> (the Wine tag's
checkout, build/winetest/wine-<tag>) the worse keys are also counted per
test function, which is where one starts reading. The triage against the
rig is --baseline rig-xp.txt --split dxvk-wine.txt: a key worse than the
rig that DXVK's run fails as often is DXVK's behaviour (a patch in
patches/dxvk/, under the rule in docs/tracks/m16-dx9-ddi.md), the rest is
the driver's or the executor's.
"""
import argparse
import bisect
import collections
import os
import re
import sys

# the prefix is the process id: 4 hex digits on NT, 8 on Win98 (fffd333d)
SUMMARY = re.compile(r"^[0-9a-f]{4,8}:(\w+):.*?(\d+) tests executed \((\d+) marked as todo, "
                     r"(\d+) as flaky, (\d+) failures?\), (\d+) skipped")
# "Test marked todo" is a check that failed where Wine expects it to
# (todo_wine, active only on Wine): a failure all the same for a run on the
# host's Wine (tools/winetest-dxvk.sh), and never seen on Windows
FAIL = re.compile(r"^(\w+\.c):(\d+): .*?(Test failed|Test succeeded inside todo block|Test marked todo|"
                  r"Test marked flaky|Test succeeded inside flaky todo block)")
CRASH = re.compile(r"unhandled exception ([0-9a-f]{8})")
STOPPED = re.compile(r"wt: no device can be made after (\S+): stopping")
NOTRUN = re.compile(r"wtrun: (\S+) (\S+): not run, no device can be made after (\S+)")
WTRUN = re.compile(r"^wtrun: (\w+) (\w+): (exit \d+|timeout|cannot start)")


def read_run(d):
    files = {}      # stem -> dict(executed, failures, skipped, state)
    keys = collections.Counter()
    for name in sorted(os.listdir(d)):
        if not name.lower().endswith(".txt") or "_test_" not in name.lower():
            continue
        stem = name[:-4].lower()
        info = {"executed": 0, "failures": 0, "skipped": 0, "state": "no summary"}
        seen = 0    # failure lines, the count when a crash left no summary line
        with open(os.path.join(d, name), errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                m = SUMMARY.match(line)
                if m:
                    info.update(executed=int(m[2]), failures=int(m[5]), skipped=int(m[6]),
                                state="ran")
                    continue
                m = FAIL.match(line)
                if m:
                    keys[(stem, f"{m[1]}:{m[2]}")] += 1
                    seen += 1
                    continue
                m = CRASH.search(line)
                if m:
                    info["state"] = f"crash {m[1]}"
                    continue
                m = STOPPED.match(line)
                if m:
                    # winetest patch 09: no device could be made after a test
                    info["state"] = f"stopped after {m[1]}"
        if info["state"] != "ran":
            keys[(stem, "crash")] += 1
            info["failures"] = seen
        files[stem] = info
    log = os.path.join(d, "WTRUN.LOG")
    if os.path.exists(log):
        with open(log, errors="replace") as f:
            for line in f:
                m = NOTRUN.match(line.strip())
                if m:
                    # WTRUN's STOP.TXT: the file never ran (patch 09)
                    stem = f"{m[1]}_{m[2]}".lower()
                    files[stem] = {"executed": 0, "failures": 0, "skipped": 0, "state": f"not run (after {m[3]})"}
                    keys[(stem, "not run")] += 1
                    continue
                m = WTRUN.match(line.strip())
                if m and m[3] == "timeout":
                    stem = f"{m[1]}_{m[2]}".lower()
                    files.setdefault(stem, {"executed": 0, "failures": 0, "skipped": 0})
                    files[stem]["state"] = "timeout"
                    keys.pop((stem, "crash"), None)
                    keys[(stem, "timeout")] += 1
    return files, keys


def read_baseline(path):
    keys = collections.Counter()
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if not line:
                continue
            keys[(line[0], line[1])] = int(line[2]) if len(line) > 2 else 1
    return keys


def functions(src, stem):
    """source line -> the test function it is in, for d3d9_test_visual's visual.c"""
    dll, test = stem.split("_test_", 1)
    path = os.path.join(src, "dlls", dll, "tests", test + ".c")
    starts, names = [], []
    if os.path.exists(path):
        with open(path, errors="replace") as f:
            for n, line in enumerate(f, 1):
                m = re.match(r"^(?:static )?\w[\w \*]*?\b(\w+)\s*\([^;]*$", line)
                if m and m[1] not in ("if", "while", "for", "switch"):
                    starts.append(n)
                    names.append(m[1])
    return lambda ln: names[bisect.bisect_right(starts, ln) - 1] if starts and ln >= starts[0] else "?"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--save", metavar="BASELINE")
    ap.add_argument("--baseline", metavar="FILE")
    ap.add_argument("--by-function", metavar="SRC")
    ap.add_argument("--split", metavar="OTHER")
    a = ap.parse_args()

    files, keys = read_run(a.dir)
    if not files:
        print(f"no test output in {a.dir}")
        return 1
    print(f"{'test file':28} {'executed':>9} {'failures':>9} {'skipped':>8}  state")
    for stem, i in sorted(files.items()):
        print(f"{stem:28} {i['executed']:>9} {i['failures']:>9} {i['skipped']:>8}  {i['state']}")

    if a.save:
        with open(a.save, "w") as f:
            f.write(f"# tools/winetest-summary.py {os.path.basename(os.path.normpath(a.dir))} "
                    "--save; one failing check per line\n")
            for (stem, key), n in sorted(keys.items()):
                f.write(f"{stem} {key} {n}\n")
        print(f"saved {len(keys)} failing keys to {a.save}")

    if a.baseline:
        base = read_baseline(a.baseline)
        worse = [(s, k, n, base.get((s, k), 0)) for (s, k), n in sorted(keys.items())
                 if n > base.get((s, k), 0)]
        better = sum(1 for sk in base if sk not in keys)
        other = read_baseline(a.split) if a.split else collections.Counter()
        oname = os.path.basename(a.split) if a.split else ""
        # of a key's failures beyond the baseline, how many --split's run has too
        shared = lambda s, k, n, b: max(0, min(n, other.get((s, k), 0)) - b)
        for s, k, n, b in worse:
            o = shared(s, k, n, b)
            print(f"WORSE {s} {k}: {n} here, {b} in the baseline" +
                  (f" ({o} in {oname})" if o else ""))
        if a.by_function:
            # per test function: the checks worse than the baseline, and of
            # those the ones --split's run fails too
            per = collections.OrderedDict()
            fn = {}
            for s, k, n, b in worse:
                if ":" not in k:
                    continue
                if s not in fn:
                    fn[s] = functions(a.by_function, s)
                name = (s, fn[s](int(k.split(":")[1])))
                t = per.setdefault(name, [0, 0])
                t[0] += n - b
                t[1] += shared(s, k, n, b)
            for (s, name), (n, o) in per.items():
                print(f"{n:6} {s} {name}" + (f"  ({o} of them {oname}'s)"
                                             if a.split and o else ""))
        print(f"{len(worse)} keys worse than {os.path.basename(a.baseline)}, "
              f"{better} of its keys pass here")
        return 1 if worse else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
