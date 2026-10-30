#!/usr/bin/env python3
"""Write one file of a UDF disc image to stdout: udfcat.py <image.iso> <path>

Windows' install discs (Vista on) keep their files in UDF, with only a
README.TXT in the ISO 9660 tree that xorriso and bsdtar read. This walks
the UDF side (ECMA-167 / UDF 1.02 as Windows writes it: one partition,
short or long allocation descriptors), enough for tools/win7-aero-test.sh
to read sources\\ei.cfg. Path components match case-insensitively.
"""
import struct
import sys

SECTOR = 2048


class Udf:
    def __init__(self, f):
        self.f = f
        avdp = self.sector(256)
        assert self.tag(avdp) == 2, "no UDF anchor at sector 256"
        length, loc = struct.unpack_from("<II", avdp, 16)
        self.part = None
        fsd = None
        for i in range(length // SECTOR):
            d = self.sector(loc + i)
            t = self.tag(d)
            if t == 5:                                   # partition descriptor
                self.part = struct.unpack_from("<I", d, 188)[0]
            elif t == 6:                                 # logical volume descriptor
                fsd = struct.unpack_from("<I", d, 252)[0]
            elif t == 8:                                 # terminating descriptor
                break
        assert self.part is not None and fsd is not None, "no partition / logical volume"
        d = self.block(fsd)
        assert self.tag(d) == 256, "no file set descriptor"
        self.root = struct.unpack_from("<I", d, 404)[0]

    def sector(self, n):
        self.f.seek(n * SECTOR)
        return self.f.read(SECTOR)

    def block(self, lbn):
        return self.sector(self.part + lbn)

    @staticmethod
    def tag(d):
        return struct.unpack_from("<H", d, 0)[0]

    def data(self, lbn):
        """The contents of the file whose (extended) file entry is at lbn."""
        fe = self.block(lbn)
        t = self.tag(fe)
        if t == 261:
            size, = struct.unpack_from("<Q", fe, 56)
            l_ea, l_ad = struct.unpack_from("<II", fe, 168)
            ads = 176 + l_ea
        elif t == 266:
            size, = struct.unpack_from("<Q", fe, 56)
            l_ea, l_ad = struct.unpack_from("<II", fe, 208)
            ads = 216 + l_ea
        else:
            raise SystemExit(f"udfcat: no file entry at block {lbn} (tag {t})")
        kind = struct.unpack_from("<H", fe, 34)[0] & 7
        if kind == 3:                                    # the data embedded in the entry
            return fe[ads:ads + l_ad][:size]
        step = 8 if kind == 0 else 16
        out = bytearray()
        for p in range(ads, ads + l_ad, step):
            elen, epos = struct.unpack_from("<II", fe, p)
            elen &= 0x3FFFFFFF
            if elen == 0:
                break
            for s in range((elen + SECTOR - 1) // SECTOR):
                out += self.block(epos + s)
        return bytes(out[:size])

    def lookup(self, lbn, name):
        d = self.data(lbn)
        p = 0
        while p + 38 <= len(d):
            assert self.tag(d[p:]) == 257, "bad file identifier descriptor"
            chars, l_fi = d[p + 18], d[p + 19]
            icb, = struct.unpack_from("<I", d, p + 24)
            l_iu, = struct.unpack_from("<H", d, p + 36)
            raw = d[p + 38 + l_iu:p + 38 + l_iu + l_fi]
            p += (38 + l_iu + l_fi + 3) & ~3
            if chars & 8 or not raw:                     # the parent entry
                continue
            fid = raw[1:].decode("latin-1") if raw[0] == 8 else raw[1:].decode("utf-16-be")
            if fid.lower() == name.lower():
                return icb
        raise SystemExit(f"udfcat: no {name}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.splitlines()[0])
    with open(sys.argv[1], "rb") as f:
        udf = Udf(f)
        lbn = udf.root
        for part in sys.argv[2].replace("\\", "/").strip("/").split("/"):
            lbn = udf.lookup(lbn, part)
        sys.stdout.buffer.write(udf.data(lbn))


main()
