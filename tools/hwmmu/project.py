#!/usr/bin/env python3
"""The hardware-MMU design's projected gain on a workload.

    tools/hwmmu/project.py <probe results> name:Ginsn:Mld:Mst:wall_s [...]

Per workload: the guest's instruction count and its loads and stores (from
census.c through phases.py) and the wall seconds the same work takes at
full speed (the run without the plugin), so the access rates are at full
speed. Each load is charged the difference between the probe's softmmu
and direct kernels (`mix4`: an independent load with four ALU ops behind
it, the throughput case translated code is closest to), each store the
`copy` pair's difference less the load's (`indep`), at the 64 KiB, 4 MiB
and 8 MiB working sets: the optimistic, middle and pessimistic rows for
a workload whose 64K-access windows hold under 2000 distinct pages. The
projection is the fraction of each second those differences add up to,
and the speed-up that removing it would be. It counts nothing else: not
the TLB refills and CR3 flushes the mirror would also change, not the
helpers' own accesses.
"""
import re, sys
probe = open(sys.argv[1]).read()
def row(kind, setname):
    m = re.search(rf"load: +{re.escape(setname)} {kind} +direct ([\d.]+) ns +softmmu ([\d.]+) ns", probe)
    return float(m.group(1)), float(m.group(2))
sets = ["64 KiB", "4 MiB", "8 MiB"]
delta = {}
for s in sets:
    ld = row("mix4", s); ind = row("indep", s); cp = row("copy", s)
    delta[s] = (ld[1] - ld[0], (cp[1] - cp[0]) - (ind[1] - ind[0]))
print("per access, ns (softmmu - direct):", ", ".join(f"{s}: load {d[0]:.2f} store {d[1]:.2f}" for s, d in delta.items()))
print(f"{'workload':10} {'Ginsn/s':>8} {'Macc/s':>7} " + " ".join(f"{s:>14}" for s in sets))
for spec in ([] if "--reuse" in sys.argv else sys.argv[2:]):
    name, gi, ld, st, wall = spec.split(':'); gi, ld, st, wall = map(float, (gi, ld, st, wall))
    ldr, str_ = ld * 1e6 / wall, st * 1e6 / wall
    cols = []
    for s in sets:
        frac = (ldr * delta[s][0] + str_ * delta[s][1]) * 1e-9
        cols.append(f"{100*frac:4.0f}% -> {1/(1-frac):4.2f}x")
    print(f"{name:10} {gi/wall:8.2f} {(ldr+str_)/1e6:7.0f} " + " ".join(f"{c:>14}" for c in cols))

# ---- the mixture: each access charged the row its own reuse distance puts it on ----
# tools/hwmmu/project.py <probe> --reuse <census.log> <shell_after_s> name:Ginsn:Mld:Mst:wall_s:start:end ...
if len(sys.argv) > 2 and sys.argv[2] == "--reuse":
    log, shell = sys.argv[3], float(sys.argv[4])
    rrx = re.compile(r"reuse t=([\d.]+)((?: \d+)+)")
    crx = re.compile(r"census t=([\d.]+) insns=(\d+) ld=(\d+) st=(\d+) pages1s=(\d+) win64k avg=(\d+) max=(\d+)")
    reuse_rows, dens_rows = [], []
    for line in open(log):
        m = rrx.match(line)
        if m:
            reuse_rows.append((float(m.group(1)), [int(x) for x in m.group(2).split()]))
        m = crx.match(line)
        if m:
            dens_rows.append((float(m.group(1)), int(m.group(6)) / 65536.0))
    allsets = ["64 KiB", "4 MiB", "8 MiB", "16 MiB", "32 MiB"]
    for s_ in allsets:
        if s_ not in delta:
            ld = row("mix4", s_); ind = row("indep", s_); cp = row("copy", s_)
            delta[s_] = (ld[1] - ld[0], (cp[1] - cp[0]) - (ind[1] - ind[0]))
    def row_for_pages(pages):  # distinct pages between reuses -> the kernel row of that range
        for lim, s_ in [(100, "64 KiB"), (1536, "4 MiB"), (3072, "8 MiB"), (6144, "16 MiB")]:
            if pages < lim:
                return s_
        return "32 MiB"
    print("\nmixture: each access on the row of its own reuse distance")
    print(f"{'workload':10} {'first':>6} " + " ".join(f"{s_:>7}" for s_ in allsets) + f" {'by pages':>16} {'by accesses':>16}")
    for spec in sys.argv[5:]:
        name, gi, ld, st, wall, a, b = spec.split(':'); gi, ld, st, wall = map(float, (gi, ld, st, wall)); a, b = shell + float(a), shell + float(b)
        sel = [r for t, r in reuse_rows if a <= t < b]
        dens = [d for t, d in dens_rows if a <= t < b]
        density = sum(dens) / len(dens)          # distinct pages per access in a window
        tot = [sum(r[i] for r in sel) for i in range(31)]
        n = sum(tot)
        share = {s_: 0.0 for s_ in allsets}; share_acc = {s_: 0.0 for s_ in allsets}
        for bkt in range(30):
            d = 2 ** bkt * 1.5                    # the bucket's middle, in accesses
            share[row_for_pages(d * density)] += tot[bkt] / n
            share_acc[row_for_pages(d)] += tot[bkt] / n
        ldr, str_ = ld * 1e6 / wall, st * 1e6 / wall
        def proj(sh):
            frac = sum(sh[s_] * (ldr * delta[s_][0] + str_ * delta[s_][1]) for s_ in allsets) * 1e-9
            return f"{100*frac:4.0f}% -> {1/(1-frac):4.2f}x"
        print(f"{name:10} {100*tot[30]/n:5.2f}% " + " ".join(f"{100*share[s_]:6.1f}%" for s_ in allsets) + f" {proj(share):>16} {proj(share_acc):>16}")
        print(f"{'':10} accesses-distance shares: " + " ".join(f"{s_} {100*share_acc[s_]:.1f}%" for s_ in allsets) + f"; density {density:.4f} pages/access")
