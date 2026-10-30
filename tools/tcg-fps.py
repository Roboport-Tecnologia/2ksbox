#!/usr/bin/env python3
"""tcg-fps.py <qmp socket> <seconds> [rate]: frames per second of a guest
drawing on the VGA surface, measured from outside (M9 track).

Takes a QMP screendump `rate` times a second (default 25) over one
persistent connection for `seconds`, hashes the images and counts how many
consecutive dumps differ: a game that blits its frame to the primary surface
changes the VGA surface once per frame, so distinct dumps per second is its
frame rate (capped at `rate`; a frame that repeats a previous image counts as
none).  Prints `fps <distinct/s> dumps <n> distinct <m>` and, under it, the
per-second counts, which is where a game that stalls on one effect shows it.  A screendump shows
the VGA surface only, nothing for a guest presenting through the 3D device.
"""
import hashlib
import json
import os
import socket
import sys
import tempfile
import time


def main():
    sock, secs = sys.argv[1], float(sys.argv[2])
    rate = float(sys.argv[3]) if len(sys.argv) > 3 else 25.0
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    f = s.makefile('rwb', buffering=0)
    f.readline()                                   # greeting
    f.write(b'{"execute":"qmp_capabilities"}\n'); f.readline()
    tmp = tempfile.mkdtemp(prefix='tcgfps-')
    path = os.path.join(tmp, 'f.ppm')
    last, distinct, dumps = None, 0, 0
    t_start = time.monotonic()
    per_sec = [0] * (int(secs) + 1)
    t_end = t_start + secs
    while time.monotonic() < t_end:
        t0 = time.monotonic()
        f.write(json.dumps({'execute': 'screendump',
                            'arguments': {'filename': path}}).encode() + b'\n')
        while True:                                # skip events
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r:
                break
        with open(path, 'rb') as fh:
            h = hashlib.md5(fh.read()).digest()
        dumps += 1
        if h != last:
            distinct += 1
            last = h
            i = int(time.monotonic() - t_start)
            if i < len(per_sec):
                per_sec[i] += 1
        dt = 1.0 / rate - (time.monotonic() - t0)
        if dt > 0:
            time.sleep(dt)
    os.unlink(path); os.rmdir(tmp)
    print(f'fps {distinct / secs:.1f} dumps {dumps} distinct {distinct}')
    print('per second ' + ' '.join(str(n) for n in per_sec[:int(secs)]))


if __name__ == '__main__':
    main()
