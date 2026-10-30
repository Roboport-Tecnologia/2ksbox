#!/usr/bin/env python3
"""The census log cut into a run's phases and summed.

    tools/hwmmu/phases.py <census.log> <shell_after_s> name:start:end [name:start:end ...]

start/end are seconds relative to the list start (run.sh's "keyed at" and the
RESULT ms); the plugin's clock starts at QEMU's launch, shell_after_s is
run.sh's "shell after N s". Prints per phase: instructions, loads, stores,
accesses per instruction, the store share, distinct pages per second (mean),
per 64K-access window (mean of means, max), windows over 3072 pages, and the
pages touched / written for the first time.
"""
import re, sys
log, shell = sys.argv[1], float(sys.argv[2])
rx = re.compile(r"census t=([\d.]+) insns=(\d+) ld=(\d+) st=(\d+) pages1s=(\d+) win64k avg=(\d+) max=(\d+) over3072=(\d+)/(\d+) newpages=(\d+) newwritten=(\d+)")
rows = [tuple(float(x) for x in m.groups()) for m in map(rx.match, open(log)) if m]
print(f"{'phase':10} {'s':>5} {'Ginsn':>7} {'Mld':>8} {'Mst':>8} {'acc/insn':>8} {'st%':>4} {'pages/s':>8} {'win avg':>7} {'win max':>7} {'over3072':>9} {'newpages':>8} {'newwritten':>10}")
for spec in sys.argv[3:]:
    name, a, b = spec.split(':'); a, b = shell + float(a), shell + float(b)
    sel = [r for r in rows if a <= r[0] < b]
    if not sel: print(name, "no rows"); continue
    ins = sum(r[1] for r in sel); ld = sum(r[2] for r in sel); st = sum(r[3] for r in sel)
    over = sum(r[7] for r in sel); win = sum(r[8] for r in sel)
    wavg = sum(r[5] * r[8] for r in sel) / max(win, 1); wmax = max(r[6] for r in sel)
    p1 = sum(r[4] for r in sel) / len(sel)
    print(f"{name:10} {len(sel):5d} {ins/1e9:7.2f} {ld/1e6:8.1f} {st/1e6:8.1f} {(ld+st)/ins:8.3f} {100*st/(ld+st):4.0f} {p1:8.0f} {wavg:7.0f} {wmax:7.0f} {int(over):5d}/{int(win):<7d} {int(sum(r[9] for r in sel)):8d} {int(sum(r[10] for r in sel)):10d}")
