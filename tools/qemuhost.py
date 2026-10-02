#!/usr/bin/env python3
"""This host's QEMU for the guest tests: which binary, and where it listens.

The binary is this checkout's build: build/qemu on Linux and macOS,
build/win/qemu on Windows (scripts/build-windows.sh), or $QEMU_BIN.

For QMP (and qtest), Linux and macOS use a Unix socket in the run's folder. Windows has AF_UNIX,
but Python on Windows does not expose it, so there QEMU listens on a free
loopback TCP port instead and the address is written `tcp:127.0.0.1:<port>`.
Every tool passes the address around as one string; nothing else needs to
know which kind it is (docs/testing.md "On Windows").

From Python (tools/ is on sys.path for `python3 tools/x.py`):

    import qemuhost
    qemu = qemuhost.qemu(ROOT)                 # or qemu(ROOT, "qemu-img")
    addr = qemuhost.addr(out_dir)              # or addr(out_dir, "qtest")
    args += ["-qmp", qemuhost.qemu_opt(addr)]
    s = qemuhost.connect(addr)                 # a connected socket

From a shell (tools/guestwait.sh wraps these):

    qemuhost.py addr <dir> [name]   the address
    qemuhost.py opt <addr>          the -qmp / -qtest value
    qemuhost.py ready <addr>        exit 0 once QEMU is listening there

SPDX-License-Identifier: GPL-2.0-or-later
"""
import os
import socket
import sys

WINDOWS = sys.platform == "win32"


def qemu(root, name="qemu-system-i386"):
    """this checkout's `name`; $QEMU_BIN overrides qemu-system-i386"""
    if name == "qemu-system-i386" and os.environ.get("QEMU_BIN"):
        return os.environ["QEMU_BIN"]
    if WINDOWS:
        return os.path.join(root, "build", "win", "qemu", name + ".exe")
    return os.path.join(root, "build", "qemu", name)


def rust_bin(root, name):
    """this checkout's release build of a Rust tool (discx, synthx, ...):
    the mingw target's directory on Windows (scripts/build-windows.sh)"""
    if WINDOWS:
        return os.path.join(root, "target", "x86_64-pc-windows-gnu", "release", name + ".exe")
    return os.path.join(root, "target", "release", name)


def addr(directory, name="qmp"):
    if not WINDOWS:
        return os.path.join(directory, name + ".sock")
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return "tcp:127.0.0.1:%d" % port


def qemu_opt(a):
    if a.startswith("tcp:"):
        return a + ",server=on,wait=off"
    return "unix:%s,server=on,wait=off" % a


def connect(a, timeout=None):
    if a.startswith("tcp:"):
        host, port = a[4:].rsplit(":", 1)
        return socket.create_connection((host, int(port)), timeout)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    if timeout is not None:
        s.settimeout(timeout)
    s.connect(a)
    return s


def ready(a):
    """QEMU listens there. A Unix socket is a file; a TCP port has to be
    knocked on, and QMP takes the next client once this one closes."""
    if not a.startswith("tcp:"):
        return os.path.exists(a)
    try:
        connect(a, 1).close()
        return True
    except OSError:
        return False


if __name__ == "__main__":
    verb, args = sys.argv[1], sys.argv[2:]
    if verb == "addr":
        print(addr(*args))
    elif verb == "opt":
        print(qemu_opt(args[0]))
    elif verb == "ready":
        sys.exit(0 if ready(args[0]) else 1)
    else:
        sys.exit("qemuhost.py: addr <dir> [name] | opt <addr> | ready <addr>")
