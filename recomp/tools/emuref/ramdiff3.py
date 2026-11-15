import sys, struct, os, glob
# ramdiff3.py emu_dir ours_dir : match frame-start dumps (ram_fK.bin) by the game's frame index (u16 gp+154), diff.
ED, OD = sys.argv[1], sys.argv[2]
GP = 0xC3E80
SKIP = [(0xBB000, 0xBD000)]   # PsyQ library globals (libgpu/libetc/libcd bookkeeping, VSync counters)
def w(d, a): return struct.unpack_from('<I', d, a)[0]
def h(d, a): return struct.unpack_from('<H', d, a)[0]
def load(dr):
    out = {}
    for p in glob.glob(os.path.join(dr, 'ram_f*.bin')):
        d = open(p, 'rb').read(); out.setdefault(h(d, GP + 154), (os.path.basename(p), d))
    return out
E, O = load(ED), load(OD)
NR = int(os.environ.get('NRUNS', '12'))
for idx in sorted(set(E) & set(O)):
    (en, e), (on, o) = E[idx], O[idx]
    diffs = [a for a in range(0x10000, 0x1F0000, 4) if e[a:a+4] != o[a:a+4] and not any(s <= a < t for s, t in SKIP)]
    runs = []
    for a in diffs:
        if runs and a - runs[-1][1] <= 16: runs[-1][1] = a
        else: runs.append([a, a])
    print('frame idx %d (emu %s, ours %s) ticks %d/%d: %d words differ outside libs, %d runs' % (idx, en, on, w(e, GP+156), w(o, GP+156), len(diffs), len(runs)))
    for r in runs[:NR]:
        k = (r[1] - r[0]) // 4 + 1
        sample = ' '.join('%08x/%08x' % (w(e, a), w(o, a)) for a in range(r[0], min(r[1] + 4, r[0] + 16), 4))
        print('   %06x +%d  emu/ours: %s' % (r[0], k, sample))
