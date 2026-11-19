/* Interrupt controller, scheduler, DMA, timers, I/O dispatch. */
#include <stdarg.h>
#include <stdlib.h>
#include "hw.h"

Cpu cpu;
uint8_t ram[0x800000];
uint8_t scratch[0x400];
int cache_isolated;
uint32_t clk_mul = 1;
uint32_t i_stat, i_mask;
FILE *rt_log;

void rt_logf(const char *fmt, ...) {
    if (!rt_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(rt_log, fmt, ap);
    va_end(ap);
    fflush(rt_log);
}

void rt_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_logf("FATAL: %s\n", buf);
    rt_logf("regs: pc? ra=%08x sp=%08x a0=%08x a1=%08x v0=%08x\n", cpu.r[31], cpu.r[29], cpu.r[4], cpu.r[5], cpu.r[2]);
    fprintf(stderr, "FATAL: %s\n", buf);
    exit(1);
}

/* ---- scheduler ------------------------------------------------------ */
static uint32_t ev_at[EV_COUNT];
static uint8_t ev_on[EV_COUNT];

void sched_update(void) {
    uint32_t best = cpu.cycles + 0x10000000u;
    for (int i = 0; i < EV_COUNT; i++)
        if (ev_on[i] && (int32_t)(ev_at[i] - best) < 0) best = ev_at[i];
    /* interrupts pending and enabled -> run event loop immediately */
    if ((i_stat & i_mask) && (cpu.sr & 0x401) == 0x401) best = cpu.cycles;
    cpu.next_event = best;
}

void sched(int ev, uint32_t delay) {
    ev_at[ev] = cpu.cycles + delay;
    ev_on[ev] = 1;
    sched_update();
}

/* periodic events (vblank, present): next due time = previous due time + period, so the lateness of
   the safe point that ran the event doesn't accumulate (it used to slow the game clock by up to ~2%
   and made the audio rate depend on the frame rate). If far behind, resync instead of bursting. */
void sched_periodic(int ev, uint32_t period) {
    uint32_t next = ev_at[ev] + period; /* ev_at still holds the due time that just fired */
    if ((int32_t)(cpu.cycles - next) > (int32_t)period || (int32_t)(next - cpu.cycles) > (int32_t)period)
        next = cpu.cycles + period;
    ev_at[ev] = next;
    ev_on[ev] = 1;
    sched_update();
}

void sched_cancel(int ev) {
    ev_on[ev] = 0;
    sched_update();
}

void irq_raise(int n) {
    i_stat |= 1u << n;
    sched_update();
}

static int in_exception;

void irq_check(void) {
    if (in_exception) return;
    if ((i_stat & i_mask) && (cpu.sr & 0x401) == 0x401) {
        in_exception = 1;
        bios_irq_exception();
        in_exception = 0;
    }
}

static void vblank(void) {
    static uint32_t fno;
    if (getenv("RUGRATS_TRACE") && (++fno % 30) == 0)
        rt_logf("frame %u: last loop %08x ra=%08x sr=%08x istat=%03x imask=%03x\n", fno, cpu.last_loop, cpu.r[31], cpu.sr, i_stat, i_mask);
    {   /* RUGRATS_PEEK=<hex addr>,<words>: log guest RAM words whenever they change (debug) */
        static int init; static uint32_t pa, pn, last[16];
        if (!init) {
            init = 1;
            const char *e = getenv("RUGRATS_PEEK");
            if (e) { sscanf(e, "%x,%u", &pa, &pn); if (pn > 16) pn = 16; }
        }
        if (pn) {
            uint32_t v[16]; int ch = 0;
            for (uint32_t i = 0; i < pn; i++) { memcpy(&v[i], ram + ((pa + i * 4) & 0x1FFFFC), 4); ch |= v[i] != last[i]; }
            if (ch) {
                char b[256]; int n = 0;
                for (uint32_t i = 0; i < pn; i++) n += snprintf(b + n, sizeof b - n, " %08x", v[i]);
                rt_logf("peek vbl %u:%s\n", fno, b);
                memcpy(last, v, sizeof v);
            }
        }
        if (!getenv("RUGRATS_TRACE")) fno++;
        /* RUGRATS_POKE=<vblank>,<hex addr>,<hex value>[;...] (up to 8): write guest RAM once at that vblank.
           A value of 1-2 hex digits writes a byte, more digits a 32-bit word (debug) */
        static int pinit, pn2; static uint32_t pf[8], paddr[8], pval[8]; static int pword[8];
        if (!pinit) {
            pinit = 1;
            const char *e = getenv("RUGRATS_POKE");
            while (e && *e && pn2 < 8) {
                char vs[16] = {0};
                if (sscanf(e, "%u,%x,%15[0-9a-fA-F]", &pf[pn2], &paddr[pn2], vs) != 3) break;
                pval[pn2] = (uint32_t)strtoul(vs, 0, 16);
                pword[pn2] = strlen(vs) > 2;
                pn2++;
                e = strchr(e, ';');
                if (e) e++;
            }
        }
        for (int i = 0; i < pn2; i++)
            if (pf[i] && fno >= pf[i]) {
                if (pword[i]) memcpy(ram + (paddr[i] & 0x1FFFFC), &pval[i], 4);
                else ram[paddr[i] & 0x1FFFFF] = (uint8_t)pval[i];
                rt_logf("poke vbl %u: %08x = %x\n", fno, paddr[i], pval[i]);
                pf[i] = 0;
            }
    }
    gpu_vblank();
    timer_vblank();
    spu_frame();
    irq_raise(IRQ_VBLANK);
    bios_vblank();
    hooks_vblank();
}

/* Presentation runs on its own clock (opt.fps per second) so the game can be shown faster than
   the 60 Hz vblank that drives its music, callbacks and counters. */
uint32_t present_count;
int present_fps;
static uint32_t present_rem;

static void present_event(void) {
    uint64_t span = (uint64_t)CYCLES_PER_FRAME * 60 + present_rem;
    uint32_t fps = (uint32_t)PRESENT_FPS;
    uint32_t period = (uint32_t)(span / fps);
    present_rem = (uint32_t)(span % fps);
    sched_periodic(EV_PRESENT, period);
    present_count++;
    host_present();
}

void present_start(void) {
    if (opt.fps < 30) opt.fps = 60;
    sched(EV_PRESENT, CYCLES_PER_FRAME * 60 / opt.fps);
}

void rt_event(void) {
    for (;;) {
        int fired = 0;
        for (int i = 0; i < EV_COUNT; i++) {
            if (ev_on[i] && (int32_t)(cpu.cycles - ev_at[i]) >= 0) {
                ev_on[i] = 0;
                fired = 1;
                switch (i) {
                case EV_VBLANK: sched_periodic(EV_VBLANK, CYCLES_PER_FRAME); vblank(); break;
                case EV_CDROM: cd_event(); break;
                case EV_DMA: dma_event(); break;
                case EV_SIO: sio_event(); break;
                case EV_PRESENT: present_event(); break;
                default: break;
                }
            }
        }
        if (!fired) break;
    }
    sched_update();
    irq_check();
    sched_update();
}

/* ---- DMA ------------------------------------------------------------ */
static uint32_t dma_madr[7], dma_bcr[7], dma_chcr[7];
static uint32_t dma_dpcr = 0x07654321, dma_dicr;
static uint32_t dma_done_pending;

void dma_init(void) {}

static void dicr_update(void) {
    uint32_t flags = (dma_dicr >> 24) & 0x7F, en = (dma_dicr >> 16) & 0x7F;
    int master = ((dma_dicr >> 15) & 1) || (((dma_dicr >> 23) & 1) && (flags & en));
    uint32_t old = dma_dicr >> 31;
    dma_dicr = (dma_dicr & 0x7FFFFFFF) | ((uint32_t)master << 31);
    if (master && !old) irq_raise(IRQ_DMA);
}

static void dma_finish(int ch) {
    dma_chcr[ch] &= ~0x01000000u;
    if (dma_dicr & (1u << (16 + ch))) {
        dma_dicr |= 1u << (24 + ch);
        dicr_update();
    }
}

extern uint32_t gpu_src_addr;
/* Draw layers (Stretch 2D option, gpu.c sq_*): the game sends one list per frame in which each of its 6 layers starts
   with its own DRAWENV packet (E3 drawing area ...), at fixed RAM addresses, one set per frame buffer. Seen
   (2026-11-15, RUGRATS_LAYERLOG): drawn in the order 4, 1, 2, 3, 5, 0; layer 4 = backdrop (sky), 1 = 3D world (on
   the title: the patterned background), 2 = HUD / menu text / full 2D screens (level select), 3 = menu button icons
   (3D models drawn with the menu camera, 8003f934). Each env packet goes out as its own DMA (PutDrawEnv), the
   layer's list in the next one, so gpu_layer = layer of the last env packet sent (-1 = none yet); after the last
   layer (0) it stays 0 until the next frame. RUGRATS_LAYERLOG=<first test frame>,<last> logs each list packet with its layer. */
int gpu_layer = -1;
/* Frames without the 3D world (title, menus, loading screens) draw their logo / speech bubble in layers 1 and 4 too,
   so there every layer is squeezed (plain 4:3 with black side bars). "World" = at least 64 GTE points of room
   geometry (gte_dcls DCLS_ROOM) since the previous frame's first layer (4). Objects are not counted: the title's logo
   bubble is a model (~560 points a frame); every level and in-engine cutscene has rooms. */
uint32_t gte_world_n;
int gpu_world_frame = 1;
uint32_t gpu_frame_tok;   /* +1 per frame (layer 4 env): the side bars are painted once per frame (gpu.c sq_set) */
static const uint32_t layer_env[2][6] = {
    { 0x0cdbf8, 0x0cdcd4, 0x0cdda4, 0x0cde74, 0x0cdf44, 0x0ce014 },
    { 0x0cdc68, 0x0cdd30, 0x0cde00, 0x0cded0, 0x0cdfa0, 0x0ce070 } };
static void layer_note(uint32_t addr, int n) {
    if (n < 2 || addr < 0x0cdbf8 || addr > 0x0ce070) return;
    uint32_t w0;
    memcpy(&w0, ram + ((addr + 4) & 0x7FFFFC), 4);
    if ((w0 >> 24) != 0xE3) return;
    for (int b = 0; b < 2; b++)
        for (int k = 0; k < 6; k++)
            if (layer_env[b][k] == addr) {
                gpu_layer = k;
                if (k == 4) {
                    static int lg = -1;
                    if (lg < 0) lg = getenv("RUGRATS_LAYERLOG") != NULL;
                    if (lg) rt_logf("layer frame: %u world points\n", gte_world_n);
                    gpu_world_frame = gte_world_n >= 64; gte_world_n = 0; gpu_frame_tok++;
                }
                return;
            }
}
static int layerlog_on(void) {
    static int a = -1, b;
    if (a < 0) { const char *e = getenv("RUGRATS_LAYERLOG"), *c = e ? strchr(e, ',') : NULL; a = e ? atoi(e) : 0; b = c ? atoi(c + 1) : a; }
    extern int frame_no;
    return a && frame_no >= a && frame_no <= b;
}
/* > 0: list sends are stale (state from an older build loaded, hw_state); counted down by the pacing hook */
int dma2_stale;
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *);   /* RUGRATS_DMATIME (no windows.h here) */
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *);
static long long dma_t_start;
unsigned fp_dma_n; double fp_dma_ms;   /* RUGRATS_FRAMEPROF: list sends and their ms this present */
uint32_t hooks_pkt_ring_bytes(void);
static void dma_run(int ch) {
    uint32_t chcr = dma_chcr[ch];
    uint32_t addr = dma_madr[ch] & 0x7FFFFC;
    int to_ram = !(chcr & 1);
    int step = (chcr & 2) ? -4 : 4;
    int mode = (chcr >> 9) & 3;
    if (mode == 2 && ch == 2 && dma2_stale) {
        static int lg;
        if (lg++ < 20) rt_logf("dma2: stale list from %06x skipped (state without the packet ring)\n", addr);
        dma_madr[ch] = 0xFFFFFF;
    } else if (mode == 2 && ch == 2) {
        /* linked list to GPU */
        int guard = 0;
        QueryPerformanceCounter(&dma_t_start);
        {   /* RUGRATS_SENDSTACK=<from>,<to> (debug): per list send in that present range, the likely guest return
               addresses on the stack */
            static int s0 = -2, s1; extern int frame_no;
            if (s0 == -2) { const char *e = getenv("RUGRATS_SENDSTACK"); s0 = -1; if (e) sscanf(e, "%d,%d", &s0, &s1); }
            if (s0 >= 0 && (int)frame_no >= s0 && (int)frame_no <= s1) {
                char b[400]; int n = 0;
                for (uint32_t a = cpu.r[29]; a < cpu.r[29] + 0x200 && n < 380; a += 4) {
                    uint32_t v = mem_r32(a);
                    if (v >= 0x80010000u && v < 0x800B6000u && (v & 3) == 0) n += snprintf(b + n, sizeof b - n, " %08x", v);
                }
                rt_logf("send f%u ot %06x:%s\n", frame_no, addr, b);
            }
        }
        { extern int gpu_cmdlog; if (gpu_cmdlog) rt_logf("dma2 list from %06x (ra %08x)\n", addr, cpu.r[31]); }
        {   /* debug: RUGRATS_PKTLOG also logs each list send with the packet ring's next-free pointer (hooks.c) */
            static int lg = -1;
            if (lg < 0) lg = getenv("RUGRATS_PKTLOG") != NULL;
            if (lg) { uint32_t cur; memcpy(&cur, ram + 0xDE3B0, 4); rt_logf("pkt send ot %06x ring %06x ra %08x\n", addr, cur & 0x7FFFFF, cpu.r[31]); }
        }
        for (;;) {
            uint32_t hdr;
            memcpy(&hdr, ram + addr, 4);
            int n = hdr >> 24;
            { extern int gpu_cmdlog; if (gpu_cmdlog && n) rt_logf("pkt %06x\n", addr); }
            layer_note(addr, n);
            if (n && layerlog_on()) { uint32_t w0, w1; memcpy(&w0, ram + ((addr + 4) & 0x7FFFFC), 4); memcpy(&w1, ram + ((addr + 8) & 0x7FFFFC), 4);
                rt_logf("layer %d pkt %06x n %d cmd %08x v %d,%d\n", gpu_layer, addr, n, w0, (int16_t)(w1 & 0xFFFF), (int16_t)(w1 >> 16)); }
            for (int i = 0; i < n; i++) {
                uint32_t w;
                memcpy(&w, ram + ((addr + 4 + i * 4) & 0x7FFFFC), 4);
                gpu_src_addr = (addr + 4 + i * 4) & 0x7FFFFC;
                gpu_gp0(w);
            }
            cpu.cycles += n + 1;
            if (hdr & 0x800000) break;
            addr = hdr & 0x7FFFFC;
            if (++guard > 0x40000) { rt_logf("DMA2 list loop\n"); break; }
        }
        gpu_glass_flush();   /* see-through object faces held back for the hi-res picture (gpu.c glass_q) */
        dma_madr[ch] = 0xFFFFFF;
        {   /* RUGRATS_DMATIME=1 (debug): ms spent sending lists (packet parse + polygon setup), every 240 sends */
            static int on = -1, nsend; static long long f; static double acc, mx;
            if (on < 0) { on = getenv("RUGRATS_DMATIME") != NULL; QueryPerformanceFrequency(&f); }
            long long t; QueryPerformanceCounter(&t);
            const double ms = (double)(t - dma_t_start) * 1000.0 / (double)f;
            fp_dma_n++; fp_dma_ms += ms;   /* RUGRATS_FRAMEPROF (host_win32.c) */
            if (on) {
                acc += ms; if (ms > mx) mx = ms;
                if (++nsend == 240) { rt_logf("dmatime: %.2f ms per send avg, max %.2f\n", acc / 240, mx); nsend = 0; acc = mx = 0; }
            }
        }
    } else {
        uint32_t words;
        if (mode == 0) words = dma_bcr[ch] & 0xFFFF ? dma_bcr[ch] & 0xFFFF : 0x10000;
        else words = (dma_bcr[ch] & 0xFFFF) * (dma_bcr[ch] >> 16);
        if (ch == 6) {
            /* OT clear: reverse linked list ending in 0xFFFFFF */
            for (uint32_t i = 0; i < words; i++) {
                uint32_t v = (i == words - 1) ? 0xFFFFFF : ((addr - 4) & 0x7FFFFC);
                memcpy(ram + addr, &v, 4);
                addr = (addr - 4) & 0x7FFFFC;
            }
        } else {
            for (uint32_t i = 0; i < words; i++) {
                uint32_t w = 0;
                if (to_ram) {
                    switch (ch) {
                    case 0: w = 0; break;
                    case 1: w = mdec_read(0); break;
                    case 2: w = gpu_read(); break;
                    case 3: w = cd_dma_read(); break;
                    case 4: w = spu_dma_read(); break;
                    default: break;
                    }
                    memcpy(ram + addr, &w, 4);
                } else {
                    memcpy(&w, ram + addr, 4);
                    switch (ch) {
                    case 0: mdec_write(0, w); break;
                    case 2: gpu_src_addr = addr; gpu_gp0(w); break;
                    case 4: spu_dma_write(w); break;
                    default: break;
                    }
                }
                addr = (addr + step) & 0x7FFFFC;
            }
            if (mode == 1) dma_madr[ch] = addr;
        }
        cpu.cycles += words;
    }
    /* completion is reported a little later so code that sets up callbacks after starting DMA works */
    dma_done_pending |= 1u << ch;
    sched(EV_DMA, 64);
}

void dma_event(void) {
    for (int ch = 0; ch < 7; ch++)
        if (dma_done_pending & (1u << ch)) dma_finish(ch);
    dma_done_pending = 0;
}

uint32_t dma_read(uint32_t off) {
    if (off == 0x70) return dma_dpcr;
    if (off == 0x74) return dma_dicr;
    int ch = off >> 4;
    if (ch > 6) return 0;
    switch (off & 0xF) {
    case 0: return dma_madr[ch];
    case 4: return dma_bcr[ch];
    case 8: return dma_chcr[ch];
    }
    return 0;
}

void dma_write(uint32_t off, uint32_t v) {
    if (off == 0x70) { dma_dpcr = v; return; }
    if (off == 0x74) {
        uint32_t ack = v & 0x7F000000;
        dma_dicr = (dma_dicr & 0xFF000000 & ~ack) | (v & 0x00FF803F);
        dicr_update();
        return;
    }
    int ch = off >> 4;
    if (ch > 6) return;
    switch (off & 0xF) {
    case 0: dma_madr[ch] = v & 0xFFFFFF; break;
    case 4: dma_bcr[ch] = v; break;
    case 8:
        dma_chcr[ch] = v;
        if ((v & 0x01000000) && (((v >> 9) & 3) != 0 || (v & 0x10000000) || ch == 6)) dma_run(ch);
        else if (v & 0x01000000 && ch == 3) dma_run(ch);
        break;
    }
}

/* ---- timers --------------------------------------------------------- */
typedef struct { uint32_t mode, target, base_cycles, value_at_base; } Timer;
static Timer tmr[3];

static uint32_t timer_div(int i) {
    int src = (tmr[i].mode >> 8) & 3;
    if (i == 1 && (src & 1)) return CYCLES_PER_LINE;
    if (i == 2 && (src & 2)) return 8 * clk_mul;
    if (i == 0 && (src & 1)) return 5 * clk_mul; /* dot clock approx */
    return clk_mul;
}

static uint32_t timer_value(int i) {
    uint32_t d = timer_div(i);
    uint32_t ticks = (cpu.cycles - tmr[i].base_cycles) / d + tmr[i].value_at_base;
    if (tmr[i].mode & 8) { /* reset at target */
        uint32_t t = tmr[i].target ? tmr[i].target : 0x10000;
        ticks %= (t + 1);
    }
    return ticks & 0xFFFF;
}

uint32_t timer_read(uint32_t off) {
    int i = (off >> 4) & 3;
    if (i > 2) return 0;
    switch (off & 0xF) {
    case 0: return timer_value(i);
    case 4: { uint32_t m = tmr[i].mode | 0x400; tmr[i].mode &= ~0x1800u; return m; }
    case 8: return tmr[i].target;
    }
    return 0;
}

void timer_write(uint32_t off, uint32_t v) {
    int i = (off >> 4) & 3;
    if (i > 2) return;
    switch (off & 0xF) {
    case 0: tmr[i].value_at_base = v & 0xFFFF; tmr[i].base_cycles = cpu.cycles; break;
    case 4: tmr[i].mode = v & 0x3FF; tmr[i].value_at_base = 0; tmr[i].base_cycles = cpu.cycles; break;
    case 8: tmr[i].target = v & 0xFFFF; break;
    }
}

void timer_vblank(void) {
    /* root counter IRQs are rarely used with irq-on-target by PsyQ games; approximate once per frame */
    for (int i = 0; i < 3; i++)
        if ((tmr[i].mode & 0x30) && (tmr[i].mode & 0x10)) irq_raise(IRQ_TMR0 + i);
}

/* ---- I/O dispatch -------------------------------------------------- */
static uint32_t io_misc[0x400];

static int io_trace3(void) {
    static int t = -1, n;
    if (t < 0) t = getenv("RUGRATS_TRACE") && getenv("RUGRATS_TRACE")[0] == '3';
    return t && n++ < 20000;
}

uint32_t io_read(uint32_t addr, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    cpu.cycles += 2;
    if (p >= 0x1F801000 && p < 0x1F803000) {
        uint32_t off = p - 0x1F801000;
        if (off == 0x070) {
            if (io_trace3()) rt_logf("istat r %03x (ra %08x)\n", i_stat, cpu.r[31]);
            return i_stat;
        }
        if (off == 0x074) return i_mask;
        if (off >= 0x080 && off < 0x100) return dma_read((off - 0x80) & ~3u) >> ((off & 3) * 8);
        if (off >= 0x100 && off < 0x130) return timer_read(off - 0x100);
        if (off >= 0x040 && off < 0x060) return sio_read(off - 0x40, size);
        if (off >= 0x800 && off < 0x804) {
            uint32_t v = cd_read(off - 0x800);
            if (size >= 2) v |= (uint32_t)cd_read(off - 0x800) << 8;
            if (size == 4) { v |= (uint32_t)cd_read(off - 0x800) << 16; v |= (uint32_t)cd_read(off - 0x800) << 24; }
            return v;
        }
        if (off == 0x810) return gpu_read();
        if (off == 0x814) return gpu_stat();
        if (off == 0x820 || off == 0x824) return mdec_read(off - 0x820);
        if (off >= 0xC00 && off < 0xE00) {
            uint32_t v = spu_read(off - 0xC00);
            if (size == 4) v |= (uint32_t)spu_read(off - 0xC00 + 2) << 16;
            return v;
        }
        return io_misc[(off >> 2) & 0x3FF];
    }
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0; /* BIOS ROM: not present (HLE) */
    if (p == 0x1FFE0130) return 0;
    rt_logf("io_read unmapped %08x size %d (ra=%08x)\n", addr, size, cpu.r[31]);
    return 0;
}

void io_write(uint32_t addr, uint32_t v, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    cpu.cycles += 2;
    if (p >= 0x1F801000 && p < 0x1F803000) {
        uint32_t off = p - 0x1F801000;
        if (off == 0x070) {
            if (io_trace3()) rt_logf("istat w %03x (ra %08x)\n", v, cpu.r[31]);
            i_stat &= v; sched_update(); return;
        }
        if (off == 0x074) { i_mask = v & 0x7FF; sched_update(); return; }
        if (off >= 0x080 && off < 0x100) {
            uint32_t r = (off - 0x80) & ~3u;
            if (size < 4) {
                /* byte/halfword access (libcd flips the DMA3 IRQ enable with a byte write to DICR+2):
                   merge into the 32-bit register; for DICR, bytes not written never ack flags */
                int sh = (off & 3) * 8;
                uint32_t mask = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
                uint32_t old = dma_read(r);
                if (r == 0x74) old &= 0x00FFFFFFu;
                v = (old & ~mask) | ((v << sh) & mask);
            }
            dma_write(r, v);
            return;
        }
        if (off >= 0x100 && off < 0x130) { timer_write(off - 0x100, v); return; }
        if (off >= 0x040 && off < 0x060) { sio_write(off - 0x40, v, size); return; }
        if (off >= 0x800 && off < 0x804) { cd_write(off - 0x800, (uint8_t)v); return; }
        if (off == 0x810) { gpu_src_addr = 0xFFFFFFFF; gpu_gp0(v); return; }
        if (off == 0x814) { gpu_gp1(v); return; }
        if (off == 0x820 || off == 0x824) { mdec_write(off - 0x820, v); return; }
        if (off >= 0xC00 && off < 0xE00) {
            spu_write(off - 0xC00, (uint16_t)v);
            if (size == 4) spu_write(off - 0xC00 + 2, (uint16_t)(v >> 16));
            return;
        }
        io_misc[(off >> 2) & 0x3FF] = v;
        return;
    }
    if (p == 0x1FFE0130) return; /* cache control */
    if ((p & 0xFFFFFF00) == 0x1F802000) return; /* expansion 2 / POST */
    rt_logf("io_write unmapped %08x=%08x size %d (ra=%08x)\n", addr, v, size, cpu.r[31]);
}

/* ---- COP0 ------------------------------------------------------------ */
uint32_t cop0_read(int reg) {
    switch (reg) {
    case 12: return cpu.sr;
    case 13: return cpu.cause | ((i_stat & i_mask) ? 0x400 : 0);
    case 14: return cpu.epc;
    case 15: return 0x00000002; /* PRId */
    }
    return 0;
}

void cop0_write(int reg, uint32_t v) {
    if (reg == 12) {
        cpu.sr = v;
        cache_isolated = (v >> 16) & 1;
        sched_update();
    } else if (reg == 13) {
        cpu.cause = (cpu.cause & ~0x300u) | (v & 0x300);
    }
}

void cop0_rfe(void) {
    cpu.sr = (cpu.sr & ~0xFu) | ((cpu.sr >> 2) & 0xF);
    sched_update();
}

void rt_div(uint32_t a, uint32_t b) {
    int32_t n = (int32_t)a, d = (int32_t)b;
    if (d == 0) { cpu.hi = a; cpu.lo = n >= 0 ? 0xFFFFFFFF : 1; }
    else if (n == INT32_MIN && d == -1) { cpu.lo = 0x80000000; cpu.hi = 0; }
    else { cpu.lo = (uint32_t)(n / d); cpu.hi = (uint32_t)(n % d); }
}

void rt_divu(uint32_t a, uint32_t b) {
    if (b == 0) { cpu.hi = a; cpu.lo = 0xFFFFFFFF; }
    else { cpu.lo = a / b; cpu.hi = a % b; }
}

/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
int hw_in_exception(void) { return in_exception; }
/* debug export (RUGRATS_EXPORT, savestate.c): interrupt, DMA and timer registers, one per out[] word:
   i_stat, i_mask, dpcr, dicr, done_pending, chcr0-6, madr2, bcr2, tmr0-2 mode, tmr0-2 target. Returns the count. */
int hw_export_regs(uint32_t *out) {
    int n = 0;
    out[n++] = i_stat; out[n++] = i_mask; out[n++] = dma_dpcr; out[n++] = dma_dicr; out[n++] = dma_done_pending;
    for (int i = 0; i < 7; i++) out[n++] = dma_chcr[i];
    out[n++] = dma_madr[2]; out[n++] = dma_bcr[2];
    for (int i = 0; i < 3; i++) out[n++] = tmr[i].mode;
    for (int i = 0; i < 3; i++) out[n++] = tmr[i].target;
    return n;
}
void hw_state(StateIO *s) {
    st_prefix(s, "cpu");
    ST(s, cpu.r); ST(s, cpu.hi); ST(s, cpu.lo); ST(s, cpu.cycles); ST(s, cpu.next_event);
    ST(s, cpu.sr); ST(s, cpu.cause); ST(s, cpu.epc);
    ST_N(s, "ram", ram, 0x200000);   /* the PS1 2 MB */
    /* The draw packet ring in the extension (hooks.c, 0x80200000..): right after a load the game sends the lists
       of the frame before the save again, and they link into ring packets, so the ring is saved too (user
       2026-11-18: screen turned to lines after loading; "DMA2 list loop" after every load). States from older
       builds have no ram_ext: then the first frame's list sends are skipped (dma2_stale), the restored VRAM
       already shows that frame. */
    {
        const uint32_t ext = hooks_pkt_ring_bytes();
        if (st_loading(s)) dma2_stale = ext && !st_has(s, "ram_ext") ? 2 : 0;   /* 2: see rt_hook_8001ceb4 */
        if (ext) ST_N(s, "ram_ext", ram + 0x200000, ext);
    }
    ST_N(s, "scratch", scratch, sizeof scratch);
    ST(s, cache_isolated);
    st_prefix(s, "hw");
    ST(s, ev_at); ST(s, ev_on); ST(s, i_stat); ST(s, i_mask); ST(s, present_rem);
    ST(s, dma_madr); ST(s, dma_bcr); ST(s, dma_chcr); ST(s, dma_dpcr); ST(s, dma_dicr); ST(s, dma_done_pending);
    for (int i = 0; i < 3; i++) {
        char n[32];
        snprintf(n, sizeof n, "tmr%d.mode", i); ST_N(s, n, &tmr[i].mode, 4);
        snprintf(n, sizeof n, "tmr%d.target", i); ST_N(s, n, &tmr[i].target, 4);
        snprintf(n, sizeof n, "tmr%d.base_cycles", i); ST_N(s, n, &tmr[i].base_cycles, 4);
        snprintf(n, sizeof n, "tmr%d.value_at_base", i); ST_N(s, n, &tmr[i].value_at_base, 4);
    }
    ST(s, io_misc);
    if (st_loading(s)) { in_exception = 0; sched_update(); }
}
