#!/usr/bin/env python3
"""rfb-shot.py <port> <out.png>: one frame of a local VNC server, as a PNG.

For a whole window the launcher's own `LAUNCHER_SHOT` cannot see: it
captures a window's content, and a mitsuami `Sidebar` (Kirigami's first
column, libadwaita's split view) is the window's. Run the KDE build on
Qt's VNC platform (`QT_QPA_PLATFORM=vnc:port=5917:size=1200x800`), then
this. No authentication, raw encoding, stdlib only; `gvnccapture` gives up
on Qt's server before its first frame.
"""
import socket
import struct
import sys
import time
import zlib


def recv(s, n):
    buf = b''
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


port, out = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(('127.0.0.1', port))
version = recv(s, 12)
s.sendall(version)  # echo the server's version
if version == b'RFB 003.003\n':
    # 3.3: the server names the one security type; 1 is none.
    assert struct.unpack('>I', recv(s, 4))[0] == 1
else:
    types = recv(s, recv(s, 1)[0])
    assert 1 in types, types
    s.sendall(b'\x01')
    assert struct.unpack('>I', recv(s, 4))[0] == 0
s.sendall(b'\x01')  # shared
w, h = struct.unpack('>HH', recv(s, 4))
recv(s, 16)
recv(s, struct.unpack('>I', recv(s, 4))[0])
# 32 bpp, depth 24, little endian, true colour, R at 16, G at 8, B at 0.
s.sendall(struct.pack('>B3xBBBBHHHBBB3x', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
s.sendall(struct.pack('>BxHi', 2, 1, 0))  # SetEncodings: raw only
time.sleep(0.2)
s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, w, h))
fb = bytearray(w * h * 4)
got = 0
while got < w * h:
    kind = recv(s, 1)[0]
    if kind != 0:
        continue
    recv(s, 1)
    for _ in range(struct.unpack('>H', recv(s, 2))[0]):
        x, y, rw, rh, enc = struct.unpack('>HHHHi', recv(s, 12))
        assert enc == 0, enc
        data = recv(s, rw * rh * 4)
        for row in range(rh):
            o = ((y + row) * w + x) * 4
            fb[o:o + rw * 4] = data[row * rw * 4:(row + 1) * rw * 4]
        got += rw * rh
raw = bytearray()
for y in range(h):
    raw.append(0)
    for x in range(w):
        b, g, r, _ = fb[(y * w + x) * 4:(y * w + x) * 4 + 4]
        raw += bytes((r, g, b))


def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print(f'{w}x{h} -> {out}')
