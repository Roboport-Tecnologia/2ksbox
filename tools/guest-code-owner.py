#!/usr/bin/env python3
"""guest-code-owner.py <run dir> <vcpu tid> <qmp socket> [pe dir]: whose code the vCPU ran.

Reads <run dir>/perf.data (a `perf record -g` of QEMU) and <run dir>/perf.map
(QEMU's -perfmap) and prints the vCPU thread's samples by host library, then
the generated-code samples by guest module (track M17, tools/w98-mp2.sh).

Naming a Win98 guest's modules is the hard part. A game's DLLs share one
preferred base (Max Payne 2's twenty-odd at 0x10000000; Win98's own D3D8.DLL
and D3D9.DLL claim 0x400000), so nearly every one is relocated, and a header
page is usually not present, which QMP's memsave cannot read. The code that
ran is present, though. So for each hot 64 KiB region this reads 24 bytes at
its hottest guest addresses over QMP (in the running guest's current address
space, which is the game's while it runs flat out) and looks for them in the
PE files of [pe dir] (default <run dir>/pe): the first address whose bytes
occur in exactly one file names the region. Run it while the guest still
runs. A region no file matches is printed by address.
"""
import collections
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmpc  # noqa: E402
from tcg_profile_lib import load_map, map_lookup  # noqa: E402


def main():
    run, tid, sock = sys.argv[1], sys.argv[2], sys.argv[3]
    pedir = sys.argv[4] if len(sys.argv) > 4 else os.path.join(run, 'pe')
    out = subprocess.run(['perf', 'report', '-i', os.path.join(run, 'perf.data'), '--tid', tid, '--stdio',
                          '--no-children', '--sort', 'dso', '-g', 'none'], capture_output=True, text=True).stdout
    print('the vCPU thread by host library:')
    for line in out.splitlines():
        if '%' in line and not line.startswith('#'):
            print('  ' + line.strip())

    mp = load_map(os.path.join(run, 'perf.map'))
    ips = subprocess.run(['perf', 'script', '-i', os.path.join(run, 'perf.data'), '--tid', tid, '-F', 'ip,dso', '-G'],
                         capture_output=True, text=True).stdout
    total, pcs = 0, collections.Counter()
    for line in ips.splitlines():
        p = line.split()
        if len(p) < 2:
            continue
        total += 1
        if 'perf-' in p[1]:
            name = map_lookup(mp, int(p[0], 16))
            if name and name.startswith('guest-0x'):
                pcs[int(name[8:], 16)] += 1
    if not total:
        print('no samples')
        return
    with open(os.path.join(run, 'jit-pcs.txt'), 'w') as f:
        f.writelines(f'{pc:08x} {n}\n' for pc, n in pcs.most_common())

    files = {}
    if os.path.isdir(pedir):
        for fn in os.listdir(pedir):
            files[fn] = open(os.path.join(pedir, fn), 'rb').read()
    f = qmpc.connect(sock)
    qmpc.cmd(f, 'qmp_capabilities')
    tmp = os.path.join(run, 'memsave.bin')

    def read(addr, n):
        r = qmpc.cmd(f, 'memsave', {'val': addr, 'size': n, 'filename': tmp})
        return open(tmp, 'rb').read() if 'return' in r else None

    regions, hottest = collections.Counter(), collections.defaultdict(list)
    for pc, n in pcs.most_common():
        regions[pc & ~0xffff] += n
        hottest[pc & ~0xffff].append(pc)
    owners = collections.Counter()
    for r, n in regions.most_common():
        who = None
        if r >= 0xc0000000:
            who = 'ring 0 (VxDs)'
        elif n * 1000 >= total:                 # regions under 0.1 % are not worth the reads
            for pc in hottest[r][:25]:
                b = read(pc, 24)
                hits = [fn for fn, data in files.items() if b and b in data]
                if len(hits) == 1:
                    who = hits[0]
                    break
        if not who:
            who = f'unnamed {r:08x}' if n * 1000 >= total else 'regions under 0.1 %'
        owners[who] += n
    jit = sum(pcs.values())
    print(f'generated code: {100 * jit / total:.1f} % of the vCPU thread, by guest module:')
    for who, n in owners.most_common():
        print(f'  {100 * n / total:5.1f} %  {who}')


if __name__ == '__main__':
    main()
