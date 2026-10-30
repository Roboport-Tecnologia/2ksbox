#!/usr/bin/env python3
"""moto-watch.py <qmp socket> <seconds> <outdir>: a second-by-second view of
what QEMU is doing while a guest plays, with the picture that goes with it
(M9 track).

A game whose cost is one *effect* (Moto Racer's tyre smoke, which nearly stops
it) is invisible in an average over a lap: the sample and the fps probe both
report the mean.  This walks the second axis instead.  Every 250 ms it reads
`info jit` (TB invalidations, bytes of code generated) over QMP, every second it
takes a screendump, and it prints one row per second plus the seconds with the
most TB invalidations, so the frame that costs can be looked at:

    tools/moto-watch.py /tmp/tcgprof-1234.sock 30 build/tcg-profile/x/watch [A:B]

A fourth argument cycles the bike's own controls over the same connection
(`6:4` is six seconds of throttle then four of brake, repeated; QEMU serves one
QMP client at a time, so the thing that measures has to be the thing that
drives), and each row is marked with the phase it was in.

Writes `watch.csv` and `w<NN>.png` (kept only for the rows named `hot`, unless
KEEP_ALL=1) into <outdir>.

With WATCH_TRACE_LOG=<the run's qemu.log> it turns QEMU's `translate_block`
trace on for one whole throttle phase and one whole brake phase (`log
trace:translate_block` / `log none` over QMP, the log sliced by the byte offsets
it noted) and prints the guest pages retranslated in each, which names the
routine a game patches when an effect appears.

With WATCH_MEMSAVE=<addr:size,...> it saves those guest-virtual ranges twice, a
second apart, *inside* the sampled phase (`mem-<addr>-a.bin` / `-b.bin`), which
is what tools/smc-diff.py wants: the bytes an effect's rasterizer patches.

With WATCH_SAMPLE_PID=<qemu pid> it also takes a macOS `sample` of exactly one
phase (WATCH_SAMPLE_PHASE, default `brake`, the second time it comes round) for
WATCH_SAMPLE_SECS seconds, with the perf map and `info jit` on both sides of it
in <outdir>/sample, which is the only way to profile an effect that lasts two
seconds a lap.  Report it with `tools/tcg-profile.py <outdir>/sample`.
"""
import collections
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile('rwb', buffering=0)
        self.f.readline()
        self.cmd({'execute': 'qmp_capabilities'})

    def cmd(self, obj):
        self.f.write(json.dumps(obj).encode() + b'\n')
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def hmp(self, line):
        r = self.cmd({'execute': 'human-monitor-command',
                      'arguments': {'command-line': line}})
        return r.get('return', '')

    def screendump(self, path):
        self.cmd({'execute': 'screendump', 'arguments': {'filename': path}})


def jit(q):
    t = q.hmp('info jit')
    inv = re.search(r'TB invalidate count\s+(\d+)', t)
    gen = re.search(r'gen code size\s+(\d+)', t)
    return (int(inv.group(1)) if inv else 0, int(gen.group(1)) if gen else 0)


def save_mem(q, ranges, out, tag):
    for r in ranges.split(','):
        addr, size = r.split(':')
        q.cmd({'execute': 'memsave',
               'arguments': {'val': int(addr, 0), 'size': int(size, 0),
                             'filename': os.path.join(out, 'mem-%s-%s.bin' % (addr, tag))}})


def key(q, code, down):
    q.cmd({'execute': 'input-send-event',
           'arguments': {'events': [{'type': 'key', 'data': {
               'down': down, 'key': {'type': 'qcode', 'data': code}}}]}})


def main():
    sock, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    cycle = None
    if len(sys.argv) > 4:
        a, b = sys.argv[4].split(':')
        cycle = (float(a), float(b))
    os.makedirs(out, exist_ok=True)
    q = Qmp(sock)
    rows, t0 = [], time.monotonic()
    phase, phase_end = 'throttle', 0.0
    sample_pid = os.environ.get('WATCH_SAMPLE_PID')
    sample_phase = os.environ.get('WATCH_SAMPLE_PHASE', 'brake')
    sample_secs = os.environ.get('WATCH_SAMPLE_SECS', '3')
    sample_dir, sample_proc, seen_phase, sampled = os.path.join(out, 'sample'), None, 0, False
    memsave = os.environ.get('WATCH_MEMSAVE')
    pending_b = None
    trace_log = os.environ.get('WATCH_TRACE_LOG')
    windows, tracing = {}, None       # phase -> (start, end) byte offsets in the log
    inv0, gen0 = jit(q)
    prev_img = None
    while time.monotonic() - t0 < secs:
        sec = int(time.monotonic() - t0)
        if cycle and sec >= phase_end:      # the bike's controls, on the one connection
            if phase == 'throttle':
                phase, phase_end = 'brake', sec + cycle[1]
                key(q, 'up', False); key(q, 'down', True)
            else:
                phase, phase_end = 'throttle', sec + cycle[0]
                key(q, 'down', False); key(q, 'up', True)
            if trace_log and cycle:   # one whole phase of each kind, traced
                if tracing is not None:
                    q.hmp('log none')
                    windows[tracing] = (windows[tracing][0], os.path.getsize(trace_log))
                    tracing = None
                if phase not in windows and sec > cycle[0]:
                    windows[phase] = (os.path.getsize(trace_log), None)
                    q.hmp('log trace:translate_block')
                    tracing = phase
            if phase == sample_phase and (sample_pid or memsave) and not sampled and sample_proc is None:
                seen_phase += 1
                if seen_phase == 2:     # the first one of a race is the standing start
                    os.makedirs(sample_dir, exist_ok=True)
                    if memsave:
                        save_mem(q, memsave, sample_dir, 'a')
                        pending_b = sec + 1
                    open(os.path.join(sample_dir, 'info-jit-before.txt'), 'w').write(q.hmp('info jit'))
                    if sample_pid:
                        sample_proc = subprocess.Popen(
                            ['sample', sample_pid, sample_secs, '1', '-mayDie',
                             '-file', os.path.join(sample_dir, 'sample.txt')],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    else:
                        sampled = True
        png = os.path.join(out, 'w%02d.png' % sec)
        q.screendump(png)
        try:
            img = hashlib.md5(open(png, 'rb').read()).digest()
        except OSError:
            img = None
        changed = int(img is not None and img != prev_img)
        prev_img = img
        # four jit reads over the second: the counter is cumulative, the row is a rate
        for _ in range(4):
            time.sleep(0.25)
        inv1, gen1 = jit(q)
        dt = 1.0
        rows.append((sec, (inv1 - inv0) / dt, (gen1 - gen0) / dt, changed, png, phase))
        inv0, gen0 = inv1, gen1
        if pending_b is not None and sec >= pending_b:
            save_mem(q, memsave, sample_dir, 'b')
            pending_b = None
        if sample_proc is not None and sample_proc.poll() is not None:
            sampled = True
            open(os.path.join(sample_dir, 'info-jit-after.txt'), 'w').write(q.hmp('info jit'))
            try:
                shutil.copy('/tmp/perf-%s.map' % sample_pid, os.path.join(sample_dir, 'perf.map'))
            except OSError:
                pass
            sample_proc = None

    with open(os.path.join(out, 'watch.csv'), 'w') as fh:
        fh.write('second,phase,invalidations_per_s,gen_bytes_per_s,frame_changed\n')
        for sec, inv, gen, ch, _, ph in rows:
            fh.write('%d,%s,%.0f,%.0f,%d\n' % (sec, ph, inv, gen, ch))
    hot = sorted(rows, key=lambda r: -r[1])[:5]
    hot_secs = {r[0] for r in hot}
    if not os.environ.get('KEEP_ALL'):
        for sec, _, _, _, png, _ in rows:
            if sec not in hot_secs:
                try:
                    os.unlink(png)
                    os.unlink(png + '.ppm')
                except OSError:
                    pass
    if tracing is not None:
        q.hmp('log none')
        windows[tracing] = (windows[tracing][0], os.path.getsize(trace_log))
    for ph, (a, b) in windows.items():
        if b is None or b <= a:
            continue
        with open(trace_log, 'rb') as fh:
            fh.seek(a)
            blob = fh.read(b - a).decode('latin1')
        pcs = collections.Counter(int(m, 16) for m in
                                  re.findall(r'translate_block .*?pc:0x([0-9a-f]+)', blob))
        total = sum(pcs.values())
        pages = collections.Counter()
        for pc, n in pcs.items():
            pages[pc & ~0xfff] += n
        secs_traced = (cycle[0] if ph == 'throttle' else cycle[1]) if cycle else 1
        print('\n%s: %d translations in %.0f s (%.0f/s), top pages:'
              % (ph, total, secs_traced, total / secs_traced))
        for pg, n in pages.most_common(8):
            top = sorted(((c, pc) for pc, c in pcs.items() if pc & ~0xfff == pg), reverse=True)[:4]
            print('  0x%08x  %7d  (%s)' % (pg, n, ' '.join('0x%x:%d' % (pc, c) for c, pc in top)))

    med = sorted(r[1] for r in rows)[len(rows) // 2] if rows else 0
    print('second  phase     invalidations/s   generated KiB/s')
    for sec, inv, gen, ch, _, ph in rows:
        print('%6d  %-8s  %15.0f   %15.0f %s' % (sec, ph, inv, gen / 1024, '*' if sec in hot_secs else ''))
    if cycle:
        for ph in ('throttle', 'brake'):
            r = [x[1] for x in rows if x[5] == ph]
            if r:
                print('%-8s mean invalidations/s %.0f over %d s' % (ph, sum(r) / len(r), len(r)))
    print('median invalidations/s %.0f, peak %.0f (x%.1f) at second%s %s' %
          (med, hot[0][1] if hot else 0, (hot[0][1] / med) if hot and med else 0,
           's' if len(hot_secs) > 1 else '', ' '.join(str(s) for s in sorted(hot_secs))))


if __name__ == '__main__':
    main()
