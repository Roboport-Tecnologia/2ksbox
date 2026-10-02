#!/usr/bin/env python3
"""`<lines> | qmp-pipe.py <qemu> <args...> -qmp stdio|-monitor stdio ...`

What scripts/test.sh's `-qmp stdio` and `-monitor stdio` checks run on
Windows ($QSYS_PIPE). QEMU there cannot take its monitor from a pipe: its
stdin reader answers the first command and then hands the monitor garbage
("QMP input must be a JSON object", JSON parse errors at columns past the
input; with HMP the reads come out shifted), so a check's `quit` never
arrives and it hangs until its timeout. This starts the same QEMU with the
stdio monitor moved to a loopback TCP port (tools/qemuhost.py) and passes
the lines read from stdin to it one at a time, as they arrive: a caller's
`sleep 1` between two commands still lets the guest run for a second.
Everything the monitor says is printed as stdio would have printed it: the
QMP greeting and replies, or HMP's banner, echo and answers. QEMU's own
output passes through, and its exit status is this program's. A QEMU that
refuses its command line exits before it listens: its error is printed and
its status returned. Without a stdio monitor the command just runs.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemuhost

argv = sys.argv[1:]
# The program by its absolute path: with NoDefaultCurrentDirectoryInExePath
# set (as some shells set it) CreateProcess finds no relative one, and
# test.sh's is relative (build/win/qemu/...).
argv[0] = os.path.abspath(shutil.which(argv[0]) or argv[0])
kind = None
for i in range(len(argv) - 1):
    if argv[i] in ("-qmp", "-monitor") and argv[i + 1] == "stdio":
        kind = argv[i]
        break
if kind is None:
    sys.exit(subprocess.call(argv))

addr = qemuhost.addr(tempfile.gettempdir())
argv[i + 1] = qemuhost.qemu_opt(addr)
qemu = subprocess.Popen(argv, stdin=subprocess.DEVNULL)

sock = None
while sock is None:
    if qemu.poll() is not None:
        sys.exit(qemu.returncode)
    try:
        sock = qemuhost.connect(addr, 30)
    except OSError:
        time.sleep(0.05)
f = sock.makefile("rwb", buffering=0)


def out(data):
    sys.stdout.write(data.decode("utf-8", "replace"))
    sys.stdout.flush()


def read(fn, *args):
    """a read that takes a reset for the end it is: QEMU closing the monitor
    after `quit` is an EOF on Linux and ConnectionResetError on Windows"""
    try:
        return fn(*args)
    except (ConnectionResetError, ConnectionAbortedError):
        return b""


def qmp_reply():
    """QMP: the lines up to and including the command's answer (events too)"""
    while True:
        line = read(f.readline)
        if not line:
            return False
        out(line)
        if b'"event"' not in line:
            return True


PROMPT = b"(qemu) "


def hmp_reply():
    """HMP: everything up to the next prompt"""
    buf = b""
    while not buf.endswith(PROMPT):
        c = read(f.read, 1)
        if not c:
            out(buf)
            return False
        buf += c
    out(buf)
    return True


reply = qmp_reply if kind == "-qmp" else hmp_reply
reply()                                     # the greeting, or the banner and first prompt
for line in sys.stdin:
    line = line.rstrip("\r\n")
    if not line.strip():
        continue
    try:
        f.write((line + "\n").encode())
    except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
        break                               # QEMU has gone (a `quit` before this line)
    if not reply():
        break
sock.close()
sys.exit(qemu.wait())
