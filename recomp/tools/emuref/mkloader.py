"""Builds inject/loader.bin (MIPS code run by the emulator at 0x800F0000) and inject/blob.bin
(the loader + GPU setting words at 0xF0F00 + our VRAM at 0xF1000), from export/regs.txt + vram.bin.
The loader: waits for GPU DMA idle, masks interrupts, resets the GP0 FIFO, uploads all of VRAM with GP0 A0,
sends our GP0 E1-E6 / GP1 03,05-08 settings, restores interrupts, then spins at DONE (Lua breakpoint)."""
import struct, sys, os
S = sys.argv[1]
regs = [int(l, 16) for l in open(os.path.join(S, 'export', 'regs.txt')).read().split()]
gpu = regs[1 + 32 + 2 + 64:][:11]  # GPU words; hw register words (hw_export_regs) follow
assert len(gpu) == 11, len(gpu)
vram = open(os.path.join(S, 'export', 'vram.bin'), 'rb').read()
assert len(vram) == 1 << 20

def I(op, rs, rt, imm): return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def R(rs, rt, rd, sh, fn): return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | fn
t0, t1, t3, t4, a0, a1 = 8, 9, 11, 12, 4, 5
code = []
def e(w): code.append(w); return len(code) - 1
NOP = 0
e(I(0x0F, 0, t0, 0x1F80))                 # lui t0,0x1f80
W = e(I(0x23, t0, t1, 0x10A8))            # lw t1,0x10a8(t0)   D2_CHCR
e(NOP)
e(R(0, t1, t1, 24, 0x02))                 # srl t1,t1,24
e(I(0x0C, t1, t1, 1))                     # andi t1,t1,1
b = e(0); code[b] = I(0x05, t1, 0, W - (b + 1))   # bne t1,zero,W
e(NOP)
e((0x10 << 26) | (0 << 21) | (t3 << 16) | (12 << 11))   # mfc0 t3,$12
e(I(0x09, 0, t4, 0xFFFE))                 # addiu t4,zero,-2
e(R(t3, t4, t4, 0, 0x24))                 # and t4,t3,t4
e((0x10 << 26) | (4 << 21) | (t4 << 16) | (12 << 11))   # mtc0 t4,$12
e(NOP); e(NOP)
for hi, port in ((0x0100, 0x1814), (0x0400, 0x1814), (0xA000, 0x1810)):
    e(I(0x0F, 0, t1, hi)); e(I(0x2B, t0, t1, port))     # lui t1,hi ; sw t1,port(t0)
e(I(0x2B, t0, 0, 0x1810))                 # sw zero (dest 0,0)
e(I(0x0F, 0, t1, 0x0200)); e(I(0x0D, t1, t1, 0x0400)); e(I(0x2B, t0, t1, 0x1810))  # 1024x512
e(I(0x0F, 0, a0, 0x800F)); e(I(0x0D, a0, a0, 0x1000))
e(I(0x0F, 0, a1, 0x0004))                 # 0x40000 words
L = e(I(0x23, a0, t1, 0))                 # lw t1,0(a0)
e(I(0x09, a0, a0, 4))
e(I(0x2B, t0, t1, 0x1810))
e(I(0x09, a1, a1, 0xFFFF))
b = e(0); code[b] = I(0x05, a1, 0, L - (b + 1))
e(NOP)
e(I(0x0F, 0, a0, 0x800F)); e(I(0x0D, a0, a0, 0x0F00))
for k in range(11):
    e(I(0x23, a0, t1, 4 * k)); e(NOP); e(I(0x2B, t0, t1, 0x1810 if k < 6 else 0x1814))

# Optional SPU part (export/spu_ram.bin + spu_regs.bin, written by the port since 2026-11-10): inject.lua copies the
# sound RAM to 0x80070000 (overwritten by our RAM in phase B anyway); the loader keys off every voice, DMAs it to SPU
# address 0 (DMA4), acks the DMA4 interrupt flag, then writes our SPU registers from the table at 0x800F0400
# (halfword pairs offset, value; 0xFFFF ends). Not written: key on/off, ENDX, data FIFO, status, voice current volume.
spu_regs = None
p = os.path.join(S, 'export', 'spu_regs.bin')
if os.path.exists(p) and os.path.exists(os.path.join(S, 'export', 'spu_ram.bin')):
    spu_regs = struct.unpack('<512H', open(p, 'rb').read())
if spu_regs:
    def delay(n):
        e(I(0x0D, 0, a1, n)); d = e(I(0x09, a1, a1, 0xFFFF)); b = e(0); code[b] = I(0x05, a1, 0, d - (b + 1)); e(NOP)
    def sh_const(off, val):
        e(I(0x0D, 0, t1, val)); e(I(0x29, t0, t1, 0x1C00 + off))   # ori t1,zero,val ; sh t1,0x1c00+off(t0)
    sh_const(0x18C, 0xFFFF); sh_const(0x18E, 0x00FF)                # key off all voices
    sh_const(0x1AA, 0xC000); delay(2000)                            # SPUCNT: on, unmuted, transfer stop
    sh_const(0x1A6, 0); sh_const(0x1AC, 0x0004)                     # transfer address 0, normal transfer
    sh_const(0x1AA, 0xC020); delay(2000)                            # DMA write mode
    e(I(0x23, t0, t1, 0x10F0)); e(I(0x0F, 0, t4, 0x000B)); e(R(t1, t4, t1, 0, 0x25)); e(I(0x2B, t0, t1, 0x10F0))  # DPCR |= ch4
    e(I(0x0F, 0, t1, 0x0007)); e(I(0x2B, t0, t1, 0x10C0))          # D4_MADR = 0x70000
    e(I(0x0F, 0, t1, 0x2000)); e(I(0x0D, t1, t1, 0x0010)); e(I(0x2B, t0, t1, 0x10C4))  # D4_BCR = 8192 x 16 words
    e(I(0x0F, 0, t1, 0x0100)); e(I(0x0D, t1, t1, 0x0201)); e(I(0x2B, t0, t1, 0x10C8))  # D4_CHCR: start, RAM->SPU
    W4 = e(I(0x23, t0, t1, 0x10C8)); e(NOP); e(R(0, t1, t1, 24, 0x02)); e(I(0x0C, t1, t1, 1))
    b = e(0); code[b] = I(0x05, t1, 0, W4 - (b + 1)); e(NOP)
    delay(2000)
    sh_const(0x1AA, 0xC000); delay(2000)
    e(I(0x23, t0, t1, 0x10F4)); e(I(0x0F, 0, t4, 0x00FF)); e(I(0x0D, t4, t4, 0x803F)); e(R(t1, t4, t1, 0, 0x24))
    e(I(0x0F, 0, t4, 0x1000)); e(R(t1, t4, t1, 0, 0x25)); e(I(0x2B, t0, t1, 0x10F4))   # DICR: ack ch4 flag only
    e(I(0x0F, 0, a0, 0x800F)); e(I(0x0D, a0, a0, 0x0400))
    LT = e(I(0x25, a0, t1, 0)); e(I(0x25, a0, a1, 2)); e(I(0x0D, 0, t4, 0xFFFF))      # lhu off ; lhu val
    bq = e(0); e(NOP)
    e(I(0x0F, 0, t4, 0x1F80)); e(R(t4, t1, t4, 0, 0x21)); e(I(0x29, t4, a1, 0x1C00))   # sh val,0x1f801c00+off
    e(I(0x09, a0, a0, 4))
    b = e(0); code[b] = I(0x04, 0, 0, LT - (b + 1)); e(NOP)                            # b LT
    code[bq] = I(0x04, t1, t4, len(code) - (bq + 1))                                   # beq off,0xffff,end
    table = []
    for v in range(24):
        for k in (0, 2, 4, 6, 8, 0xA, 0xE):
            table.append(v * 16 + k)
    table += [0x180, 0x182, 0x184, 0x186, 0x190, 0x192, 0x194, 0x196, 0x198, 0x19A, 0x1A2, 0x1A4, 0x1AC]
    table += list(range(0x1B0, 0x1C0, 2)) + list(range(0x1C0, 0x200, 2))
    pairs = [(o, spu_regs[o >> 1]) for o in table] + [(0x1AA, spu_regs[0x1AA >> 1] & ~0x30)]
    spu_table = b''.join(struct.pack('<HH', o, v) for o, v in pairs) + struct.pack('<HH', 0xFFFF, 0)
    assert len(spu_table) <= 0xB00
e((0x10 << 26) | (4 << 21) | (t3 << 16) | (12 << 11))   # mtc0 t3,$12
e(NOP)
D = e(0); code[D] = (0x02 << 26) | (((0x800F0000 + 4 * D) >> 2) & 0x3FFFFFF)  # j D
e(NOP)
blob = bytearray(0x1000 + len(vram))
assert 4 * len(code) <= 0x400, len(code)
os.makedirs(os.path.join(S, 'inject'), exist_ok=True)
blob[0:4 * len(code)] = struct.pack('<%dI' % len(code), *code)
if spu_regs:
    blob[0x400:0x400 + len(spu_table)] = spu_table
    open(os.path.join(S, 'inject', 'spu_ram.bin'), 'wb').write(open(os.path.join(S, 'export', 'spu_ram.bin'), 'rb').read())
elif os.path.exists(os.path.join(S, 'inject', 'spu_ram.bin')):
    os.remove(os.path.join(S, 'inject', 'spu_ram.bin'))
open(os.path.join(S, 'inject', 'done.txt'), 'w').write('%08x\n' % (0x800F0000 + 4 * D))
blob[0xF00:0xF00 + 44] = struct.pack('<11I', *gpu)
blob[0x1000:] = vram
os.makedirs(os.path.join(S, 'inject'), exist_ok=True)
open(os.path.join(S, 'inject', 'blob.bin'), 'wb').write(blob)
print('code words', len(code), 'DONE at %08x' % (0x800F0000 + 4 * D), 'blob', len(blob))
