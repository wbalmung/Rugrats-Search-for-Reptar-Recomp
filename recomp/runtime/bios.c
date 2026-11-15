/* High-level replacement for the PS1 BIOS kernel functions this game calls. */
#include <setjmp.h>
#include <stdlib.h>
#include "hw.h"

#define R cpu.r

/* ---- events --------------------------------------------------------- */
typedef struct { uint32_t cls, spec, mode, func, status; } Event; /* status: 0 free 1 disabled 2 enabled 4 ready */
static Event ev[32];

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (int i = 0; i < 32; i++)
        if (ev[i].status == 0) {
            ev[i] = (Event){ cls, spec, mode, func, 1 };
            return 0xF1000000u | i;
        }
    return 0xFFFFFFFF;
}

static Event *get_event(uint32_t h) {
    uint32_t i = h & 0xFFFF;
    return ((h & 0xFFFF0000) == 0xF1000000 && i < 32) ? &ev[i] : 0;
}

void bios_deliver_event(uint32_t cls, uint32_t spec) {
    for (int i = 0; i < 32; i++) {
        Event *e = &ev[i];
        if (e->status == 2 && e->cls == cls && e->spec == spec) {
            if (e->mode == 0x2000) e->status = 4;
            else if (e->mode == 0x1000 && e->func) {
                Cpu save = cpu;
                R[31] = 0;
                rt_dispatch(e->func);
                uint32_t c = cpu.cycles;
                cpu = save;
                cpu.cycles = c;
            }
        }
    }
}

/* ---- pads ----------------------------------------------------------- */
static uint32_t pad_buf[2], pad_started;

static void pad_update(void) {
    if (!pad_started) return;
    for (int p = 0; p < 2; p++) {
        if (!pad_buf[p]) continue;
        uint32_t a = pad_buf[p];
        uint16_t b = host_pad_buttons(p);
        if (p == 0 || b != 0xFFFF) {
            mem_w8(a, 0x00);
            mem_w8(a + 1, 0x41);
            mem_w8(a + 2, b & 0xFF);
            mem_w8(a + 3, b >> 8);
        } else {
            mem_w8(a, 0xFF);
        }
    }
}

/* ---- interrupt handler chains (SysEnqIntRP) ------------------------- */
static uint32_t intr_head[4];
static uint32_t custom_exit;
static int clear_pad = 1, clear_rcnt[4] = { 1, 1, 1, 1 };
static jmp_buf *exc_jb;

static uint32_t call_guest(uint32_t fn, uint32_t a0) {
    R[4] = a0;
    R[31] = 0;
    rt_dispatch(fn);
    return R[2];
}

void bios_irq_exception(void) {
    Cpu saved = cpu;
    jmp_buf jb;
    jmp_buf *prev = exc_jb;
    static int storm;
    uint32_t pending_before = i_stat & i_mask;

    /* exception entry: push interrupt-enable stack, interrupts off */
    cpu.sr = (cpu.sr & ~0x3Fu) | ((cpu.sr << 2) & 0x3C);
    cpu.epc = 0;
    exc_jb = &jb;
    if (!setjmp(jb)) {
        for (int pr = 0; pr < 4; pr++) {
            uint32_t q = intr_head[pr];
            int guard = 0;
            while (q && guard++ < 16) {
                uint32_t next = mem_r32(q);
                uint32_t f2 = mem_r32(q + 4), f1 = mem_r32(q + 8);
                if (f1) {
                    uint32_t v = call_guest(f1, 0);
                    if (v && f2) call_guest(f2, v);
                }
                q = next;
            }
        }
        /* BIOS default acknowledges */
        if (clear_pad) i_stat &= ~(1u << IRQ_VBLANK);
        for (int t = 0; t < 3; t++)
            if (clear_rcnt[t]) i_stat &= ~(1u << (IRQ_TMR0 + t));
        if (custom_exit) {
            /* jmp_buf layout: ra, sp, fp, s0..s7, gp */
            R[31] = mem_r32(custom_exit);
            R[29] = mem_r32(custom_exit + 4);
            R[30] = mem_r32(custom_exit + 8);
            for (int i = 0; i < 8; i++) R[16 + i] = mem_r32(custom_exit + 12 + i * 4);
            R[28] = mem_r32(custom_exit + 44);
            R[2] = 1;
            uint32_t ra = R[31];
            R[31] = 0;
            rt_dispatch(ra);
        }
    }
    exc_jb = prev;
    uint32_t cyc = cpu.cycles;
    cpu = saved;
    cpu.cycles = cyc;
    if ((i_stat & i_mask) && (i_stat & i_mask) == pending_before) {
        if (++storm == 1000) rt_logf("warning: IRQ %03x not acknowledged by game\n", i_stat & i_mask);
        if (storm > 1000) i_stat &= ~i_mask; /* avoid an endless storm */
    } else storm = 0;
    sched_update();
}

void bios_vblank(void) { pad_update(); }

/* ---- memory card: files stored in the saves folder ----------------- */
#include "memcard.h"

/* ---- dispatcher ----------------------------------------------------- */
static void unimpl(uint32_t t, uint32_t f) {
    static uint8_t seen[3][256];
    int ti = t == 0xA0 ? 0 : t == 0xB0 ? 1 : 2;
    if (!seen[ti][f & 0xFF]++) rt_logf("BIOS %02X:%02X not implemented (ra=%08x)\n", t, f, R[31]);
    R[2] = 0;
}

static uint32_t heap_base, heap_size;

static void bios_call_impl(uint32_t t, uint32_t f);
/* RUGRATS_MCLOG=1: log every memory card call (file API + card events) with arguments and result */
void bios_call(uint32_t t, uint32_t f) {
    static int mclog = -1;
    if (mclog < 0) mclog = getenv("RUGRATS_MCLOG") != NULL;
    int mc = (t == 0xA0 && (f == 0xAB || f == 0xAC || f == 0xAD)) ||
             (t == 0xB0 && ((f >= 0x32 && f <= 0x36) || (f >= 0x41 && f <= 0x45) || (f >= 0x4A && f <= 0x50) || f == 0x5C || f == 0x5D || (f >= 0x07 && f <= 0x0D)));
    if (!mclog || !mc) { bios_call_impl(t, f); return; }
    uint32_t a0 = R[4], a1 = R[5], a2 = R[6], ra = R[31];
    char name[40] = "";
    if (t == 0xB0 && (f == 0x32 || f == 0x41 || f == 0x42 || f == 0x45))
        for (int i = 0; i < 39; i++) { name[i] = (char)mem_r8(a0 + (uint32_t)i); if (!name[i]) break; }
    if (t == 0xB0 && f >= 0x09 && f <= 0x0D) { /* event handle: show its class/spec/status */
        Event *e = get_event(a0);
        if (e) snprintf(name, sizeof name, "ev cls %08x spec %04x st %u", e->cls, e->spec, e->status);
    }
    if (t == 0xB0 && f == 0x08) snprintf(name, sizeof name, "open ev cls %08x spec %04x mode %04x", a0, a1, a2);
    if (t == 0xB0 && f == 0x07) snprintf(name, sizeof name, "deliver cls %08x spec %04x", a0, a1);
    bios_call_impl(t, f);
    rt_logf("mc: %02X:%02X(%08x, %08x, %08x) %s -> %08x (ra %08x)\n", t, f, a0, a1, a2, name, R[2], ra);
}

static void bios_call_impl(uint32_t t, uint32_t f) {
    uint32_t a0 = R[4], a1 = R[5], a2 = R[6], a3 = R[7];
    if (t == 0xA0) {
        switch (f) {
        case 0x39: heap_base = a0; heap_size = a1; R[2] = 0; return; /* InitHeap */
        case 0x44: R[2] = 0; return;                                /* FlushCache */
        case 0x49: gpu_gp1(0x01000000); gpu_gp0(a0); R[2] = 0; return; /* GPU_cw */
        case 0x71: case 0x72: R[2] = 1; return;                     /* _96_init/_96_remove */
        case 0xAB: R[2] = mc_card_info(a0); return;                 /* _card_info */
        case 0xAC: R[2] = mc_card_load(a0); return;                 /* _card_load */
        case 0xAD: R[2] = 0; return;                                /* set_card_auto_format */
        }
    } else if (t == 0xB0) {
        switch (f) {
        case 0x07: bios_deliver_event(a0, a1); R[2] = 1; return;
        case 0x08: R[2] = open_event(a0, a1, a2, a3); return;
        case 0x09: { Event *e = get_event(a0); if (e) e->status = 0; R[2] = 1; return; }
        case 0x0A: /* WaitEvent */ {
            Event *e = get_event(a0);
            if (!e) { R[2] = 0; return; }
            int guard = 0;
            while (e->status != 4 && e->status == 2 && guard++ < 600) { cpu.cycles += 2000; rt_event(); }
            if (e->status == 4) e->status = 2;
            R[2] = 1;
            return;
        }
        case 0x0B: { Event *e = get_event(a0); R[2] = 0; if (e && e->status == 4) { e->status = 2; R[2] = 1; } return; }
        case 0x0C: { Event *e = get_event(a0); if (e) e->status = 2; R[2] = 1; return; }
        case 0x0D: { Event *e = get_event(a0); if (e) e->status = 1; R[2] = 1; return; }
        case 0x12: pad_buf[0] = a0; pad_buf[1] = a2; R[2] = 1; return; /* InitPad */
        case 0x13: pad_started = 1; clear_pad = 1; R[2] = 1; return;   /* StartPad */
        case 0x14: pad_started = 0; R[2] = 1; return;
        case 0x15: pad_buf[0] = a0; pad_buf[1] = a0 + 0x22; pad_started = 1; R[2] = 1; return;
        case 0x17: /* ReturnFromException */
            if (exc_jb) longjmp(*exc_jb, 1);
            rt_logf("ReturnFromException outside exception\n");
            return;
        case 0x18: custom_exit = 0; R[2] = 0; return;
        case 0x19: custom_exit = a0; R[2] = 0; return;
        case 0x32: R[2] = mc_open(a0, a1); return;
        case 0x33: R[2] = mc_lseek(a0, a1, a2); return;
        case 0x34: R[2] = mc_read(a0, a1, a2); return;
        case 0x35:
            if (a0 == 1) { /* stdout: the game's debug printf. RUGRATS_GAMELOG=1 copies it to the log */
                static int gl = -1;
                if (gl < 0) gl = getenv("RUGRATS_GAMELOG") != NULL;
                if (gl) {
                    char b[256];
                    uint32_t n = a2 < 255 ? a2 : 255;
                    for (uint32_t i = 0; i < n; i++) b[i] = (char)mem_r8(a1 + i);
                    b[n] = 0;
                    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
                    if (n) rt_logf("game: %s\n", b);
                }
                R[2] = a2;
                return;
            }
            R[2] = mc_write(a0, a1, a2);
            return;
        case 0x36: R[2] = mc_close(a0); return;
        case 0x41: R[2] = mc_format(a0); return;
        case 0x42: R[2] = mc_firstfile(a0, a1); return;
        case 0x43: R[2] = mc_nextfile(a0); return;
        case 0x45: R[2] = mc_erase(a0); return;
        case 0x4A: case 0x4B: case 0x4C: R[2] = 1; return;           /* InitCard/StartCard/StopCard */
        case 0x4E: R[2] = mc_write_sector(a0, a1, a2); return;
        case 0x4F: R[2] = mc_read_sector(a0, a1, a2); return;
        case 0x50: R[2] = 1; return;                                 /* allow_new_card */
        case 0x5B: clear_pad = a0; R[2] = 1; return;                 /* ChangeClearPad */
        case 0x5C: case 0x5D: R[2] = 0x11; return;                   /* card status: ready */
        case 0x56: case 0x57: R[2] = 0; return;
        }
    } else if (t == 0xC0) {
        switch (f) {
        case 0x02: /* SysEnqIntRP(priority, struct) */
            if (a0 < 4) { mem_w32(a1, intr_head[a0]); intr_head[a0] = a1; }
            R[2] = 1;
            return;
        case 0x03: /* SysDeqIntRP */
            if (a0 < 4) {
                uint32_t prev = 0, q = intr_head[a0];
                int guard = 0;
                while (q && guard++ < 64) {
                    if (q == a1) {
                        if (prev) mem_w32(prev, mem_r32(q)); else intr_head[a0] = mem_r32(q);
                        break;
                    }
                    prev = q;
                    q = mem_r32(q);
                }
            }
            R[2] = 1;
            return;
        case 0x0A: /* ChangeClearRCnt(t, flag) -> old */
            if (a0 < 4) { R[2] = clear_rcnt[a0]; clear_rcnt[a0] = a1; } else R[2] = 0;
            return;
        }
    }
    unimpl(t, f);
}

/* The kernel's device table (DCB), as a real BIOS sets it up in low RAM: pointer at 0x150, size in bytes at
   0x154, 0x50 bytes per device, +0 = name pointer, +0x34 = firstfile handler. The game's PsyQ libapi firstfile
   (8009ac98) walks it to find "bu", patches that device's firstfile handler, calls BIOS firstfile (B0:42, done in
   memcard.h without the table) and restores the handler. With an empty table it never calls the BIOS and reports
   "no save file", so in-game saving failed ("Creating save game file failed") and Load Game found nothing. */
static void kernel_device_table(void) {
    static const char *const names[3] = { "tty", "cdrom", "bu" };
    const uint32_t tab = 0x80007000u, str = 0x80007200u; /* free kernel area (the game loads at 0x80010000) */
    for (int i = 0; i < 3; i++) {
        uint32_t e = tab + (uint32_t)i * 0x50, s = str + (uint32_t)i * 16;
        for (uint32_t k = 0; k < 0x50; k += 4) mem_w32(e + k, 0);
        for (int k = 0; k < 16; k++) mem_w8(s + (uint32_t)k, (uint8_t)(k < (int)strlen(names[i]) ? names[i][k] : 0));
        mem_w32(e + 0x00, s);
        mem_w32(e + 0x04, i == 2 ? 0x14 : 0x01); /* flags: block device for bu (informational) */
    }
    mem_w32(0x80000150u, tab);
    mem_w32(0x80000154u, 3 * 0x50);
}

void bios_init(void) {
    mc_init(opt.saves);
    kernel_device_table();
}

/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
void bios_state(StateIO *s) {
    st_prefix(s, "bios");
    ST(s, ev); ST(s, pad_buf); ST(s, pad_started); ST(s, intr_head); ST(s, custom_exit); ST(s, clear_pad);
    ST(s, clear_rcnt); ST(s, mc_fd);
    if (st_loading(s)) exc_jb = NULL;
}
