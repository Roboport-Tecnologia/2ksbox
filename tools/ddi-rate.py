#!/usr/bin/env python3
"""ddi-rate.py <qemu.log> [min draws a frame]: the executor's 5 s rate lines, summed.

The executor logs `ddi: N frames/s (... draws ...; readbacks R ms, W of them
waiting for the frame)` every 5 s while frames are read back, and the device
logs `batches in 5.0 s, T ms of them in the executor`. This keeps the lines
of the scene being measured (at least `min` draws a frame, 150 by default:
Max Payne 2's corridor has 207, its menu 12) and prints their mean and
spread.

With D3DPT_DDI_FLUSH_AB=n the executor alternates two flush thresholds, one
line each, tagged `[flush N]`. Then the two are compared by adjacent pairs:
a TCG run's speed moves by about 5 % between launches (doc 22 §5.0), so an
A/B read from two runs measures the launches, and one read inside a run
does not (track M17).
"""
import re
import statistics as st
import sys

RATE = re.compile(r'ddi: ([\d.]+) frames/s \((\d+) readbacks, (\d+) dp2 calls, (\d+) draws'
                  r'.*?readbacks (\d+) ms, (\d+) of them waiting for the frame\)(?: \[flush (\d+)\])?')
EXEC = re.compile(r'batches in [\d.]+ s, ([\d.]+) ms of them in the executor')


def main():
    path = sys.argv[1]
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 150
    rows, exec_ms = [], []
    last_level = False
    for line in open(path, errors='replace'):
        m = RATE.search(line)
        if m:
            fps, rb, draws = float(m.group(1)), int(m.group(2)), int(m.group(4))
            last_level = rb > 0 and draws / rb >= lo
            if last_level:
                rows.append((m.group(7), fps, int(m.group(5)), int(m.group(6))))
            continue
        m = EXEC.search(line)
        if m and last_level:
            exec_ms.append(float(m.group(1)))
    if not rows:
        print(f'no rate line with {lo}+ draws a frame in {path}')
        return
    fps = [r[1] for r in rows]
    print(f'{len(rows)} periods of 5 s: {st.mean(fps):.2f} frames/s (sd {st.pstdev(fps):.2f}), '
          f'readbacks {st.mean(r[2] for r in rows):.0f} ms, {st.mean(r[3] for r in rows):.0f} of them waiting'
          + (f'; the executor {st.mean(exec_ms):.0f} ms' if exec_ms else '') + ' in each 5 s')
    keys = sorted({r[0] for r in rows if r[0] is not None}, key=int)
    for k in keys:
        v = [r for r in rows if r[0] == k]
        print(f'  flush {k:>3}: {len(v)} periods, {st.mean(r[1] for r in v):.2f} frames/s, '
              f'waiting {st.mean(r[3] for r in v):.0f} ms')
    if len(keys) == 2:
        a, b = keys
        pairs = [(y[1] - x[1]) if y[0] == b else (x[1] - y[1])
                 for x, y in zip(rows, rows[1:]) if x[0] != y[0]]
        se = st.pstdev(pairs) / len(pairs) ** 0.5
        print(f'  flush {b} against {a}, adjacent periods: {st.mean(pairs):+.2f} frames/s '
              f'(n {len(pairs)}, standard error {se:.2f})')


if __name__ == '__main__':
    main()
