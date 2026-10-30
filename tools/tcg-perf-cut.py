#!/usr/bin/env python3
"""tcg-perf-cut.py <run dir> [from to]: a whole-run Linux perf profile
(tools/w98-3dmark.sh <n> whole, EXTRA=-perfmap) cut to one window, given in
seconds after the Benchmark click, and split by thread and by bucket on the
vCPU thread: generated code, the softmmu slow path, translation + lookup,
dispatch, helpers, the executor, DXVK, libc. With no window it prints the
`ddi:` frame-rate lines with their offsets so a window can be chosen (the
first-person test is the ~15 s one). Writes <run dir>/pcs.txt (samples per
guest instruction) for tools/tcg-form-weights.py. Keep the run's
perf-<pid>.map beside perf.data (w98-3dmark.sh copies it) or the generated
code is one [unknown]. The Linux counterpart of tools/tcg-profile.py (macOS
`sample`); docs/tracks/m9-tcg-aarch64.md, "Win98 3D"."""
import os, re, subprocess, sys, datetime
from collections import Counter, defaultdict

d = sys.argv[1]
click = float(open(os.path.join(d, 'click.txt')).read().strip())
lines = []
for l in open(os.path.join(d, 'qemu.log'), errors='replace'):
    m = re.match(r'(\S+)Z .*ddi: ([0-9.]+) frames/s \((\d+) readbacks, (\d+) dp2 calls, (\d+) draws.* in ([0-9.]+) s', l)
    if m:
        t = datetime.datetime.fromisoformat(m.group(1)).replace(tzinfo=datetime.timezone.utc).timestamp()
        lines.append((t - click, float(m.group(2)), int(m.group(5)), float(m.group(6))))
if len(sys.argv) < 4:
    for off, fps, draws, dt in lines:
        print(f'{off - dt:7.1f} .. {off:7.1f}  {fps:6.1f} fps  {draws:7d} draws')
    sys.exit(0)
w0, w1 = float(sys.argv[2]), float(sys.argv[3])

# perf script: time is perf_clock; the first sample is ~ at the click
out = subprocess.run(['perf', 'script', '-i', os.path.join(d, 'perf.data'), '-F', 'comm,tid,time,ip,sym,dso', '--no-demangle'],
                     capture_output=True, text=True, errors='replace').stdout
first = None
threads = Counter(); bucket = Counter(); sym = Counter(); guest = Counter(); dsos = Counter(); gpc = Counter()
n = 0
rows = []
BUCKETS = [
    ('softmmu slow path', r'^(helper_(le|be|ret)?_?(ld|st)\w*_mmu|(load|store)_helper|do_(ld|st)\w*|mmu_lookup\w*|cpu_ld\w*|cpu_st\w*|tlb_\w*|x86_cpu_tlb_fill|mmu_translate|get_physical_address|address_space_\w*|memory_region_\w*|iotlb_to_section|probe_access\w*|page_collection\w*|tb_invalidate\w*|soft_imm\w*|notdirty\w*|cpu_physical_memory\w*|flatview\w*)'),
    ('translation + lookup', r'^(tb_gen_code|tb_lookup|helper_lookup_tb_ptr|tb_htable_lookup|tcg_gen_code|tcg_optimize|liveness_pass\w*|reachable_code\w*|tcg_\w*|gen_\w*|translator_\w*|i386_tr_\w*|disas_insn\w*|x86_\w*decode\w*|decode_\w*|tb_link_page|qht_\w*|setjmp_gen_code|tb_add_jump|tb_reset_jump|cpu_restore_state\w*|x86_restore_state_to_opc)'),
    ('dispatch', r'^(cpu_exec\w*|cpu_tb_exec|cpu_loop_exec_tb|tcg_qemu_tb_exec|cpu_handle_interrupt|cpu_handle_exception|x86_cpu_exec_interrupt|x86_cpu_do_interrupt|do_interrupt\w*|raise_\w*|cpu_loop_exit\w*|tcg_cpu_exec|tcg_cpus_exec|mttcg_cpu_thread_fn|rr_cpu_thread_fn|qemu_wait_io_event\w*|cpu_exec_interrupt\w*|cpu_has_work|cpu_exit|apic_\w*|pic_\w*|hpet_\w*|i8254\w*|pit_\w*|timer\w*|qemu_clock\w*|icount\w*)'),
    ('helpers: x87', r'^helper_(f\w*|x87\w*|shadow\w*)$'),
    ('helpers: sse/simd', r'^helper_(\w*(xmm|mmx)\w*|cvt\w*|comis\w*|ucomis\w*|movmsk\w*|sse\w*|simd\w*|enter_mmx|emms|ldmxcsr|update_mxcsr\w*|p\w*|sh\w*|un\w*|add\w*|sub\w*|mul\w*|div\w*|min\w*|max\w*|sqrt\w*|rcp\w*|rsqrt\w*|and\w*|or\w*|xor\w*|cmp\w*|mov\w*)$'),
    ('helpers: other', r'^helper_\w+$'),
    ('softfloat', r'^(float\w*|int\w*_to_float\w*|round\w*|propagateFloat\w*|packFloat\w*|normalize\w*|soft_\w*|parts\w*|frac\w*)'),
]
def bucket_of(s, dso):
    if 'perf-' in dso and '.map' in dso:
        return 'generated code'
    if 'd3dpt_exec' in dso:
        return 'executor'
    if 'dxvk' in dso:
        return 'dxvk'
    if 'libc' in dso or 'ld-linux' in dso:
        return 'libc'
    if 'libvulkan' in dso or 'radeon' in dso or 'radv' in dso or 'nvidia' in dso or 'mesa' in dso or 'libLLVM' in dso or 'amdvlk' in dso:
        return 'vulkan driver'
    if 'kernel' in dso or dso == '[kernel.kallsyms]':
        return 'kernel'
    if 'qemu' not in dso:
        return 'other lib: ' + os.path.basename(dso)
    for b, r in BUCKETS:
        if re.match(r, s):
            return b
    return 'qemu other'

for l in out.splitlines():
    p = l.split()
    if len(p) < 5:
        continue
    # comm tid time ip sym dso  (comm may have spaces: take from the right)
    dso = p[-1]; ip = None
    # find the time field: ends with ':'
    ti = next((i for i, x in enumerate(p) if re.match(r'^\d+\.\d+:$', x)), None)
    if ti is None:
        continue
    t = float(p[ti][:-1]); tid = p[ti - 1]; comm = ' '.join(p[:ti - 1])
    ip = p[ti + 1]; s = ' '.join(p[ti + 2:-1])
    if first is None:
        first = t
    off = t - first
    if off < w0 or off >= w1:
        continue
    n += 1
    threads[tid] += 1
    rows.append((tid, s, dso))
vcpu = Counter(t for t, s_, dso in rows if 'perf-' in dso).most_common(1)[0][0]
for tid, s, dso in rows:
    if tid == vcpu:
        b = bucket_of(s, dso)
        bucket[b] += 1
        if b == 'generated code':
            m = re.match(r'guest-0x([0-9a-f]+)', s)
            if m:
                guest[int(m.group(1), 16) >> 12] += 1
                gpc[int(m.group(1), 16)] += 1
        else:
            sym[(b, s)] += 1
            dsos[dso] += 1
print(f'{n} samples in [{w0}, {w1}) s after the click')
print('\nthreads:')
for k, v in threads.most_common(12):
    print(f'  {v:6d} {100*v/n:5.1f}%  {k}')
vc = sum(bucket.values())
print(f'\nvCPU thread tid {vcpu} ({vc} samples, {100*vc/n:.1f}% of all) by bucket:')
for k, v in bucket.most_common():
    print(f'  {v:6d} {100*v/vc:5.1f}%  {k}')
print('\nvCPU thread, hottest non-generated symbols:')
for (b, s), v in sym.most_common(45):
    print(f'  {v:6d} {100*v/vc:5.1f}%  [{b}] {s}')
print('\nhottest guest pages:')
cum = 0
for k, v in guest.most_common(40):
    cum += v
    print(f"  {v:6d} {100*v/vc:5.1f}% cum {100*cum/vc:5.1f}%  0x{k << 12:08x}")

with open(os.path.join(d, 'pcs.txt'), 'w') as f:
    for k, v in sorted(gpc.items()):
        f.write(f'{k:08x} {v}\n')
print(f'{len(gpc)} distinct guest instructions sampled -> pcs.txt')
