#!/usr/bin/env python3
"""Tables out of tools/specbench/run.sh's runs.

    tools/specbench/report.py build/specbench/runs [--md]

For every configuration directory it reads results.txt (SPECRUN's RESULT
lines: wall ms, job CPU ms, CRC of stdout, exit code) and the programs'
own outputs pulled off the floppy (nbench.txt, 7zip.txt, ssebench.txt,
superpi.txt), and prints the tables docs/22-tcg-evaluation.md carries:

  - the headline table: one number per benchmark per configuration, each
    in the benchmark's own unit, with the ratio to `stock` (>1 = faster):
      superpi   CPU seconds of the 1M run (lower is better; best of the reps)
      7zip      7-Zip's total rating in MIPS (`Tot:` line; higher is better)
      ssebench  SSEBENCH's SSE score, ns per SSE op (lower is better)
      nbench    the geometric mean of its ten kernels' iterations/s ratios
  - nbench per kernel, 7-Zip compress / decompress, SSEBENCH per kernel
  - the CRC check: a benchmark whose stdout differs between configurations
    computed a different answer (nbench, 7-Zip and SSEBENCH print their own
    timings, so only superpi's digits, and the SPEC-lineage programs when
    SPEC=1 built them, are checked)
"""
import os, re, sys, statistics

RES = re.compile(r"^RESULT (\S+) rep=(\d+) ms=(\d+)(?: cpu=(\d+))? crc=([0-9a-f]{8}) exit=(\d+)")
CRC_CHECKED = {"superpi", "bzip2-c", "bzip2-d", "shor", "hmmer", "gnugo", "sjeng"}
LOWER_IS_BETTER = {"superpi", "ssebench", "bzip2-c", "bzip2-d", "shor", "hmmer", "gnugo", "sjeng"}
NBENCH_TESTS = ["NUMERIC SORT", "STRING SORT", "BITFIELD", "FP EMULATION", "FOURIER",
                "ASSIGNMENT", "IDEA", "HUFFMAN", "NEURAL NET", "LU DECOMPOSITION"]
HEADLINE = ["superpi", "7zip", "ssebench", "nbench"]
SPEC = ["bzip2-c", "bzip2-d", "shor", "hmmer", "gnugo", "sjeng"]


def load(d):
    p = os.path.join(d, "results.txt")
    if not os.path.exists(p):
        return None
    r = {}
    for line in open(p, errors="replace"):
        m = RES.match(line.strip())
        if m:
            r.setdefault(m.group(1), []).append(dict(ms=int(m.group(3)), cpu=int(m.group(4) or 0),
                                                     crc=m.group(5), exit=int(m.group(6))))
    d_ = dict(res=r, nbench={}, zip={}, sse={}, sse_kernels={})
    p = os.path.join(d, "nbench.txt")
    if os.path.exists(p):
        for line in open(p, errors="replace"):
            for t in NBENCH_TESTS:
                if line.startswith(t):
                    nums = re.findall(r"\d+\.\d+|\d+", line[len(t):])
                    if nums:
                        d_["nbench"][t] = float(nums[0])
    p = os.path.join(d, "7zip.txt")
    if os.path.exists(p):
        # 7-Zip: "Avr:  <usage> <R/U> <rating> | <usage> <R/U> <rating>", "Tot:  <usage> <R/U> <rating>"
        for line in open(p, errors="replace"):
            if line.startswith("Avr:") or line.startswith("Avg:"):
                nums = [int(x) for x in re.findall(r"\d+", line)]
                if len(nums) >= 8:
                    d_["zip"]["compress"] = nums[3]; d_["zip"]["decompress"] = nums[7]
            elif line.startswith("Tot:"):
                nums = [int(x) for x in re.findall(r"\d+", line)]
                if nums:
                    d_["zip"]["total"] = nums[-1]
    p = os.path.join(d, "ssebench.txt")
    if os.path.exists(p):
        for line in open(p, errors="replace"):
            m = re.match(r"SSE score: ([\d.]+) ns", line)
            if m:
                d_["sse"]["score"] = float(m.group(1))
            m = re.match(r"(.{32}) +([\d.]+) +([\d.]+) +([\d.]+) +\S+$", line.rstrip())
            if m and m.group(1).strip() != "kernel":
                d_["sse_kernels"][m.group(1).strip()] = float(m.group(4))
    return d_


def fmt_table(rows, md):
    if md:
        out = ["| " + " | ".join(rows[0]) + " |", "|" + "---|" * len(rows[0])]
        out += ["| " + " | ".join(r) + " |" for r in rows[1:]]
    else:
        w = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
        out = ["  ".join(c.ljust(w[i]) for i, c in enumerate(r)) for r in rows]
    return "\n".join(out)


def main():
    root = sys.argv[1]
    md = "--md" in sys.argv
    data = {}
    for c in sorted(os.listdir(root)):
        got = load(os.path.join(root, c)) if os.path.isdir(os.path.join(root, c)) else None
        if got:
            data[c] = got
    if not data:
        print("no results under", root); return
    order = [c for c in ["stock", "stock-pic", "off", "default"] if c in data] + \
            [c for c in data if c not in ("stock", "stock-pic", "off", "default")]
    base = "stock" if "stock" in data else order[0]

    def value(c, n):  # the benchmark's own number for a configuration, or None
        d = data[c]
        if n == "superpi":
            v = [x["cpu"] or x["ms"] for x in d["res"].get("superpi", []) if x["exit"] == 0]
            return min(v) / 1000.0 if v else None
        if n == "7zip":
            return d["zip"].get("total")
        if n == "ssebench":
            return d["sse"].get("score")
        if n == "nbench":
            if not d["nbench"] or base not in data or not data[base]["nbench"]:
                return None
            rs = [d["nbench"][t] / data[base]["nbench"][t] for t in NBENCH_TESTS
                  if t in d["nbench"] and t in data[base]["nbench"]]
            return statistics.geometric_mean(rs) if rs else None
        v = [x["ms"] for x in d["res"].get(n, [])]
        return min(v) / 1000.0 if v else None

    def ratio(c, n):
        v, s = value(c, n), value(base, n)
        if v is None or s is None or v == 0 or s == 0:
            return None
        if n == "nbench":
            return v
        return s / v if n in LOWER_IS_BETTER else v / s

    names = [n for n in HEADLINE if any(value(c, n) is not None for c in data)]
    names += [n for n in SPEC if any(n in data[c]["res"] for c in data)]
    unit = {"superpi": "s CPU", "7zip": "MIPS", "ssebench": "ns/op", "nbench": "geomean"}
    rows = [["config"] + ["%s (%s)" % (n, unit.get(n, "s")) for n in names] + ["geomean vs " + base]]
    for c in order:
        row = [c]; rs = []
        for n in names:
            v = value(c, n)
            if v is None:
                row.append("-"); continue
            cell = ("%.2f" % v) if n in ("ssebench", "nbench") else ("%d" % v if n == "7zip" else "%.1f" % v)
            if c != base:
                r = ratio(c, n)
                if r is not None:
                    cell += " (%.2fx)" % r; rs.append(r)
            row.append(cell)
        row.append("1.00x" if c == base else ("%.2fx" % statistics.geometric_mean(rs) if rs else "-"))
        rows.append(row)
    print(fmt_table(rows, md))

    bad = {}
    for n in sorted(CRC_CHECKED):
        crcs = {c: {x["crc"] for x in data[c]["res"].get(n, [])} for c in order if n in data[c]["res"]}
        allc = set().union(*crcs.values()) if crcs else set()
        if len(allc) > 1:
            bad[n] = crcs
    if bad:
        print("\nCRC MISMATCH (a configuration computed a different answer):")
        for n, crcs in bad.items():
            print(" ", n, {c: sorted(v) for c, v in crcs.items()})
    else:
        print("\nCRC: identical output for every checked benchmark in every configuration")

    def block(title, cols, keys, get, higher, fmt):
        cs = [c for c in order if cols(c)]
        if not cs:
            return
        print("\n" + title)
        rows = [[""] + cs]
        for k in keys(cs[0]):
            row = [k]
            for c in cs:
                v = get(c, k); s = get(base, k) if base in data else None
                cell = fmt % v if v is not None else "-"
                if v is not None and s and c != base and v:
                    cell += " (%.2fx)" % ((v / s) if higher else (s / v))
                row.append(cell)
            rows.append(row)
        print(fmt_table(rows, md))

    block("nbench, iterations/s (higher is better)", lambda c: data[c]["nbench"], lambda c: NBENCH_TESTS,
          lambda c, k: data[c]["nbench"].get(k), True, "%.2f")
    block("7-Zip `b 2 -mmt1 -md=22`, MIPS (higher is better)", lambda c: data[c]["zip"],
          lambda c: ["compress", "decompress", "total"], lambda c, k: data[c]["zip"].get(k), True, "%d")
    block("SSEBENCH, ns per op (lower is better)", lambda c: data[c]["sse_kernels"],
          lambda c: list(data[c]["sse_kernels"]), lambda c, k: data[c]["sse_kernels"].get(k), False, "%.2f")


if __name__ == "__main__":
    main()
