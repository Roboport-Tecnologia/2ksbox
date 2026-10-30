#!/usr/bin/env python3
"""Compare two 24-bit BMPs (the d3dgame -dump format) pixel by pixel.

    tools/bmpdiff.py golden.bmp candidate.bmp [-o diff.bmp] [--mask X,Y,W,H]...
                     [--tolerance N] [--max-over N]

Prints the number of differing pixels, the largest channel difference and
the bounding box of the differences. -o writes an amplified difference
image. --mask ignores a rectangle (top-left origin, like the screen). The
d3dgame frame-time HUD sits at 0,368,270,112 in a 640x480 frame; it is
deterministic in -frames mode, so only the first rig captures need it
masked. Exits 1 when pixels differ by more than --tolerance (default 0),
unless --max-over allows that many. The native DXVK frame sits ~1090
pixels from the GeForce golden at tolerance 8, so --max-over is a
regression budget, not equality. Pure Python, no Pillow.
"""
import argparse
import struct
import sys


def read_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM":
        sys.exit(f"{path}: not a BMP")
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp != 24:
        sys.exit(f"{path}: {bpp}-bit BMP, only 24-bit is supported")
    stride = (w * 3 + 3) & ~3
    rows = []
    flip = h > 0
    h = abs(h)
    for y in range(h):
        src = off + y * stride
        rows.append(d[src:src + w * 3])
    if flip:
        rows.reverse()  # rows[0] is now the top of the image
    return w, h, rows, d[:off]


def write_bmp(path, w, h, rows):
    stride = (w * 3 + 3) & ~3
    pad = b"\0" * (stride - w * 3)
    size = 54 + stride * h
    hdr = struct.pack("<2sIHHI", b"BM", size, 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr)
        for y in range(h - 1, -1, -1):
            f.write(rows[y] + pad)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("golden")
    ap.add_argument("candidate")
    ap.add_argument("-o", "--out", help="write an amplified difference image (BMP)")
    ap.add_argument("--mask", action="append", default=[], help="X,Y,W,H rectangle to ignore (top-left origin); repeatable")
    ap.add_argument("--tolerance", type=int, default=0, help="per-channel difference that still counts as equal")
    ap.add_argument("--max-over", type=int, default=0, help="exit 0 while at most this many pixels are beyond the tolerance")
    a = ap.parse_args()
    w, h, ga, _ = read_bmp(a.golden)
    w2, h2, gb, _ = read_bmp(a.candidate)
    if (w, h) != (w2, h2):
        sys.exit(f"size mismatch: {w}x{h} vs {w2}x{h2}")
    masks = []
    for m in a.mask:
        x, y, mw, mh = (int(v) for v in m.split(","))
        masks.append((x, y, x + mw, y + mh))
    differing = 0
    over = 0
    maxd = 0
    bbox = None
    out = []
    for y in range(h):
        ra, rb = ga[y], gb[y]
        orow = bytearray(w * 3)
        for x in range(w):
            i = x * 3
            if any(mx0 <= x < mx1 and my0 <= y < my1 for mx0, my0, mx1, my1 in masks):
                continue
            d = max(abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2]))
            if d:
                differing += 1
                maxd = max(maxd, d)
                if d > a.tolerance:
                    over += 1
                    bbox = (x, y, x, y) if bbox is None else (min(bbox[0], x), min(bbox[1], y), max(bbox[2], x), max(bbox[3], y))
                v = min(255, d * 4)
                orow[i] = orow[i + 1] = orow[i + 2] = v
        out.append(bytes(orow))
    total = w * h
    print(f"{differing} of {total} pixels differ ({100.0 * differing / total:.2f} %), max channel difference {maxd}")
    if bbox:
        print(f"{over} beyond tolerance {a.tolerance}; bounding box x {bbox[0]}..{bbox[2]}, y {bbox[1]}..{bbox[3]} (top-left origin)")
    if a.out:
        write_bmp(a.out, w, h, out)
        print(f"difference image: {a.out}")
    sys.exit(1 if over > a.max_over else 0)


if __name__ == "__main__":
    main()
