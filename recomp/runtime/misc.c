/* SIO0: controller port (digital pad on port 1, nothing on port 2, no raw memory card access).
   MDEC lives in mdec.c. */
#include <stdlib.h>
#include "hw.h"

static struct {
    uint16_t ctrl, mode, baud;
    uint8_t rx; int rx_full;
    int step;       /* byte index within the current transfer */
    int device;     /* 0 none, 1 pad, 2 card */
    int ack;        /* /ACK low after last byte */
    int irq;
    uint16_t buttons;
} s;

/* exchange log: one line per selection, only when it is not the plain 01 42 00 00 00 poll */
static char xlog[256];
static int xlen, xport;
static void xlog_flush(void) {
    static int on = -1, n;
    if (on < 0) on = getenv("RUGRATS_PADLOG") != 0;
    int normal = xlen == 25 && strncmp(xlog, " 01>ff 42>41 00>5a 00>", 22) == 0 && s.step == 5;
    int empty_port2 = xport == 1 && xlen == 6;
    if (on && xlen && !normal && !empty_port2 && n++ < 300) {
        extern uint32_t hooks_frame(void);
        rt_logf("pad%d @%u:%s\n", xport, hooks_frame(), xlog);
    }
    xlen = 0; xlog[0] = 0;
}

static void deselect(void) { xlog_flush(); s.step = 0; s.device = 0; }

static int sio_trace(void);

static void transfer(uint8_t tx) {
    int port = (s.ctrl >> 13) & 1;
    uint8_t rx = 0xFF;
    int ack = 0;
    if (!(s.ctrl & 2)) { s.rx = 0xFF; s.rx_full = 1; return; } /* not selected */
    if (s.step == 0) {
        if (tx == 0x01 && port == 0) {
            s.device = 1; ack = 1; s.buttons = host_pad_buttons(0);
            static int nlog;
            if (s.buttons != 0xFFFF && getenv("RUGRATS_PADLOG") && (nlog++ % 5) == 0) rt_logf("pad read %04x\n", s.buttons);
        }
        else s.device = 0;        /* 0x81 = memory card: none attached at the hardware level */
        rx = 0xFF;
    } else if (s.device == 1) {
        switch (s.step) {
        case 1:                                                  /* digital pad ID */
            rx = 0x41;
            if (tx == 0x42) ack = 1;
            else s.device = 0;   /* digital pads only know "read buttons"; DualShock probes get no ACK */
            break;
        case 2: rx = 0x5A; ack = 1; break;
        case 3: rx = s.buttons & 0xFF; ack = 1; break;
        case 4: rx = s.buttons >> 8; ack = 0; s.device = 0; break; /* last byte: no ACK */
        default: rx = 0xFF; break;
        }
    }
    if (xlen < 200 && s.step < 4) xlen += snprintf(xlog + xlen, sizeof xlog - xlen, " %02x>%02x", tx, rx);
    else if (xlen < 200 && s.step == 4) xlen += snprintf(xlog + xlen, sizeof xlog - xlen, "%s", ""); /* buttons hidden */
    xport = port;
    s.step++;
    s.rx = rx;
    s.rx_full = 1;
    s.ack = 0;
    /* the device pulls /ACK low a little after the byte completes (~8 bits + ack delay) */
    if (ack) sched(EV_SIO, 400);
    cpu.cycles += 100;
}

void sio_event(void) {
    s.ack = 1;
    if (s.ctrl & 0x1000) {
        s.irq = 1;
        irq_raise(IRQ_PAD);
    }
}

static int sio_trace(void) { static int t = -1; if (t < 0) t = getenv("RUGRATS_TRACE") && getenv("RUGRATS_TRACE")[0] == '3'; return t; }
static int ntr;

uint32_t sio_read(uint32_t off, int size) {
    (void)size;
    if (sio_trace() && ntr++ < 400) rt_logf("sio r %x (ctrl %04x step %d rxf %d) loop %08x ra %08x\n", off, s.ctrl, s.step, s.rx_full, cpu.last_loop, cpu.r[31]);
    switch (off) {
    case 0x0: {
        uint8_t v = s.rx_full ? s.rx : 0xFF;
        s.rx_full = 0;
        return v | (v << 8) | (v << 16) | ((uint32_t)v << 24);
    }
    case 0x4: {
        /* /ACK is a short pulse: visible on one status read, then released */
        uint32_t v = 0x5 | (s.rx_full ? 2 : 0) | (s.ack ? 0x80 : 0) | (s.irq ? 0x200 : 0);
        s.ack = 0;
        return v;
    }
    case 0x8: return s.mode;
    case 0xA: return s.ctrl;
    case 0xE: return s.baud;
    }
    return 0;
}

void sio_write(uint32_t off, uint32_t v, int size) {
    (void)size;
    if (sio_trace() && ntr++ < 400) rt_logf("sio w %x = %04x\n", off, v);
    switch (off) {
    case 0x0: transfer((uint8_t)v); break;
    case 0x8: s.mode = (uint16_t)v; break;
    case 0xA:
        if (v & 0x40) { memset(&s, 0, sizeof s); return; }  /* reset */
        if (v & 0x10) { s.irq = 0; }                         /* acknowledge */
        if (!(v & 2)) deselect();
        if ((v ^ s.ctrl) & 0x2000) deselect();               /* port switched */
        s.ctrl = (uint16_t)(v & ~0x50u);
        break;
    case 0xE: s.baud = (uint16_t)v; break;
    }
}


/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
void sio_state(StateIO *st) {
    st_prefix(st, "sio");
    ST(st, s.ctrl); ST(st, s.mode); ST(st, s.baud); ST(st, s.rx); ST(st, s.rx_full); ST(st, s.step); ST(st, s.device);
    ST(st, s.ack); ST(st, s.irq); ST(st, s.buttons);
}
