#!/usr/bin/env python3
"""tcg-form-weights.py <run dir> <pages dir>: the per-instruction sample
counts (pcs.txt, from tools/tcg-perf-cut.py) joined with a memsave of the hot
guest pages (w98-3dmark.sh PAGES=, taken while the guest is in the test, since
a user page is only there in its own process) and disassembled with capstone
(build/venv-capstone/bin/python): samples by instruction form (unit: x87,
sse, mmx, int, branch; by operand kind: memory, register, immediate), the
samples per instruction of each form, which is where an uneven form shows
(a memory-operand addps at 10 against 2.7 for the register form was patch
36), the mnemonics, and the hottest instructions with their text."""
import os, sys, glob
from collections import Counter, defaultdict
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_OP_IMM

run, pdir = sys.argv[1], sys.argv[2]
pcs = {}
for l in open(os.path.join(run, 'pcs.txt')):
    a, n = l.split(); pcs[int(a, 16)] = int(n)
pages = {}
for f in glob.glob(os.path.join(pdir, '*.bin')):
    pages[int(os.path.basename(f)[:-4], 16)] = open(f, 'rb').read()
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
insns = {}
for base, data in pages.items():
    # decode from the page start; a page that begins mid-instruction is
    # resynchronised by the sampled pcs themselves: decode from each
    # sampled pc that the sequential pass did not land on
    seen = set()
    def dec(start):
        for i in md.disasm(data[start - base:], start):
            if i.address in insns: break
            insns[i.address] = i
    dec(base)
    for pc in pcs:
        if base <= pc < base + 4096 and pc not in insns:
            dec(pc)
X87 = ('f',); SSE = ('ps', 'ss', 'pd', 'sd')
def unit(i):
    m = i.mnemonic
    if m.startswith('f') and not m.startswith('fs:'): return 'x87'
    if m in ('cvttps2pi', 'cvtpi2ps', 'cvtps2pi'): return 'sse'
    if m.endswith(SSE) or m in ('movaps', 'movups', 'movlps', 'movhps', 'movlhps', 'movhlps', 'movmskps', 'ldmxcsr', 'stmxcsr', 'shufps', 'unpcklps', 'unpckhps', 'cvtsi2ss', 'cvttss2si', 'cvtss2si', 'sqrtss', 'rsqrtss', 'rcpss', 'rsqrtps', 'rcpps', 'sqrtps'): return 'sse'
    if m.startswith('p') and m not in ('push', 'pop', 'pushfd', 'popfd', 'pushad', 'popad', 'pause') or m in ('emms', 'movq', 'movd'): return 'mmx'
    if m in ('call', 'ret', 'jmp') or m.startswith('j'): return 'branch'
    return 'int'
def kind(i):
    if any(o.type == X86_OP_MEM for o in i.operands): return 'mem'
    if i.operands and all(o.type == X86_OP_IMM for o in i.operands): return 'imm'
    return 'reg'
form = Counter(); nins = Counter(); tot = 0; miss = 0; top = []
mnem = Counter()
for pc, n in pcs.items():
    i = insns.get(pc)
    if i is None:
        miss += n; continue
    tot += n
    k = (unit(i), kind(i))
    form[k] += n; nins[k] += 1
    mnem[(unit(i), i.mnemonic, kind(i))] += n
    top.append((n, pc, f'{i.mnemonic} {i.op_str}'))
print(f'{tot} samples on decoded instructions, {miss} on pages not dumped')
print('\nby form (samples, share of decoded, instructions, samples/instruction):')
for k, v in form.most_common():
    print(f'  {k[0]:6s} {k[1]:3s}  {v:5d} {100*v/tot:5.1f}%  {nins[k]:4d} insns  {v/nins[k]:5.1f} per insn')
print('\nby mnemonic:')
for k, v in mnem.most_common(40):
    print(f'  {v:5d} {100*v/tot:5.1f}%  {k[0]:6s} {k[1]:12s} {k[2]}')
print('\nhottest instructions:')
for n, pc, t in sorted(top, reverse=True)[:40]:
    print(f'  {n:5d}  {pc:08x}  {t}')
