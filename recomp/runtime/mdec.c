/* MDEC: PS1 motion decoder (RLE -> IDCT -> YUV->RGB), per psx-spx.
   Input words arrive through 1F801820 or DMA0; a command is decoded as soon as all its
   parameter words are in, into an output FIFO that DMA1 / 1F801820 reads drain. */
#include <stdlib.h>
#include <string.h>
#include "hw.h"

#define IN_MAX  (0x10000 + 8)
#define OUT_MAX (1 << 20)

static struct {
    uint32_t cmd;           /* current command word */
    uint32_t need;          /* parameter words still expected */
    uint32_t in[IN_MAX];
    uint32_t nin;
    uint32_t out[OUT_MAX];
    uint32_t out_rd, out_wr;
    uint32_t ctrl;
    uint8_t  iq_y[64], iq_uv[64];
    int16_t  scale[64];
    int      block;         /* status bits 18-16 */
} m;

static int logon = -1;

static const uint8_t zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};
static uint8_t zagzig[64];

/* ---- RLE reader over the input halfwords ---- */
static uint32_t rd_pos, rd_end;     /* in halfwords */
static int rd_more(void) { return rd_pos < rd_end; }
static uint16_t rd_next(void) {
    if (rd_pos >= rd_end) return 0xFE00;
    uint32_t w = m.in[rd_pos >> 1];
    uint16_t h = (rd_pos & 1) ? (uint16_t)(w >> 16) : (uint16_t)w;
    rd_pos++;
    return h;
}
static int sx10(uint32_t v) { v &= 0x3FF; return (v & 0x200) ? (int)v - 0x400 : (int)v; }

/* returns 0 when no data is left (no block started) */
static int decode_block(int16_t blk[64], const uint8_t *qt) {
    int32_t tmp[64];
    memset(tmp, 0, sizeof tmp);
    uint16_t n;
    do {
        if (!rd_more()) return 0;
        n = rd_next();
    } while (n == 0xFE00);
    int q = (n >> 10) & 0x3F;
    int k = 0;
    int32_t val = sx10(n) * qt[0];
    for (;;) {
        if (q == 0) val = sx10(n) * 2;
        if (val < -0x400) val = -0x400;
        if (val > 0x3FF) val = 0x3FF;
        if (q > 0) tmp[zagzig[k]] = val;
        else tmp[k] = val;
        n = rd_next();
        if (n == 0xFE00) break;
        k += (n >> 10) + 1;
        if (k > 63) break;
        val = (sx10(n) * qt[k] * q + 4) / 8;
    }
    /* two-pass IDCT; each pass transposes (psx-spx real_idct_core) */
    int32_t t2[64];
    int32_t *src = tmp, *dst = t2;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++) sum += (int64_t)src[y + z * 8] * m.scale[x + z * 8];
                dst[x + y * 8] = (int32_t)((sum + 0x8000) >> 16);
            }
        int32_t *s = src; src = dst; dst = s;
    }
    for (int i = 0; i < 64; i++) {
        int v = src[i];
        /* result is a signed 9-bit value; hardware clips it to 8 bits */
        if (v < -128) v = -128;
        if (v > 127) v = 127;
        blk[i] = (int16_t)v;
    }
    return 1;
}

static void out_push(uint32_t w) {
    if (m.out_wr - m.out_rd < OUT_MAX) m.out[m.out_wr++ & (OUT_MAX - 1)] = w;
}

static int clamp8s(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

static void decode_color(int depth, int sign, int bit15) {
    int16_t cr[64], cb[64], y[4][64];
    for (;;) {
        if (!decode_block(cr, m.iq_uv)) break;
        if (!decode_block(cb, m.iq_uv)) break;
        int ok = 1;
        for (int b = 0; b < 4 && ok; b++) ok = decode_block(y[b], m.iq_y);
        if (!ok) break;
        uint8_t rgb[16 * 16 * 3];
        for (int py = 0; py < 16; py++)
            for (int px = 0; px < 16; px++) {
                int ci = (px >> 1) + (py >> 1) * 8;
                int CR = cr[ci], CB = cb[ci];
                int R = (1436 * CR) >> 10;              /* 1.402 */
                int G = (-352 * CB - 731 * CR) >> 10;   /* -0.3437, -0.7143 */
                int B = (1815 * CB) >> 10;              /* 1.772 */
                int Y = y[(px >> 3) + (py >> 3) * 2][(px & 7) + (py & 7) * 8];
                int r = clamp8s(Y + R), g = clamp8s(Y + G), bb = clamp8s(Y + B);
                if (!sign) { r ^= 0x80; g ^= 0x80; bb ^= 0x80; }
                uint8_t *o = rgb + (py * 16 + px) * 3;
                o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)bb;
            }
        if (depth == 2) {           /* 24-bit: 768 bytes */
            for (int i = 0; i < 768; i += 4)
                out_push(rgb[i] | rgb[i + 1] << 8 | rgb[i + 2] << 16 | (uint32_t)rgb[i + 3] << 24);
        } else {                    /* 15-bit */
            uint16_t px[256];
            for (int i = 0; i < 256; i++) {
                uint8_t *o = rgb + i * 3;
                px[i] = (uint16_t)((o[0] >> 3) | (o[1] >> 3) << 5 | (o[2] >> 3) << 10 | (bit15 ? 0x8000 : 0));
            }
            for (int i = 0; i < 256; i += 2) out_push(px[i] | (uint32_t)px[i + 1] << 16);
        }
    }
}

static void decode_mono(int depth, int sign) {
    int16_t yb[64];
    while (decode_block(yb, m.iq_y)) {
        uint8_t v[64];
        for (int i = 0; i < 64; i++) v[i] = (uint8_t)(sign ? yb[i] : (yb[i] ^ 0x80));
        if (depth == 1) {
            for (int i = 0; i < 64; i += 4) out_push(v[i] | v[i + 1] << 8 | v[i + 2] << 16 | (uint32_t)v[i + 3] << 24);
        } else {
            for (int i = 0; i < 64; i += 8) {
                uint32_t w = 0;
                for (int j = 0; j < 8; j++) w |= (uint32_t)(v[i + j] >> 4) << (j * 4);
                out_push(w);
            }
        }
    }
}

static void run_command(void) {
    uint32_t op = m.cmd >> 29;
    if (logon) rt_logf("mdec: cmd %08x (%u words)\n", m.cmd, m.nin);
    if (op == 1) {
        int depth = (m.cmd >> 27) & 3, sign = (m.cmd >> 26) & 1, bit15 = (m.cmd >> 25) & 1;
        rd_pos = 0; rd_end = m.nin * 2;
        if (depth >= 2) decode_color(depth, sign, bit15);
        else decode_mono(depth, sign);
    } else if (op == 2) {
        memcpy(m.iq_y, m.in, 64);
        if (m.cmd & 1) memcpy(m.iq_uv, (uint8_t *)m.in + 64, 64);
    } else if (op == 3) {
        memcpy(m.scale, m.in, 128);
    }
}

static void begin_command(uint32_t w) {
    m.cmd = w;
    m.nin = 0;
    switch (w >> 29) {
    case 1: m.need = w & 0xFFFF; break;
    case 2: m.need = (w & 1) ? 32 : 16; break;
    case 3: m.need = 32; break;
    default: m.need = 0; break;
    }
    if (m.need == 0) run_command();
}

void mdec_write(uint32_t off, uint32_t v) {
    if (logon < 0) {
        const char *e = getenv("RUGRATS_MDECLOG");
        logon = e && *e && *e != '0';
        for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (uint8_t)i;
    }
    if (off == 0) {
        if (m.need == 0) { begin_command(v); return; }
        if (m.nin < IN_MAX) m.in[m.nin++] = v;
        if (--m.need == 0) run_command();
    } else {
        if (v & 0x80000000u) {          /* reset */
            m.need = 0; m.nin = 0; m.cmd = 0;
            m.out_rd = m.out_wr = 0;
            m.block = 4;
        }
        m.ctrl = v & 0x60000000u;
    }
}

uint32_t mdec_read(uint32_t off) {
    if (off == 0) {
        if (m.out_rd == m.out_wr) return 0;
        return m.out[m.out_rd++ & (OUT_MAX - 1)];
    }
    uint32_t st = 0;
    int empty = m.out_rd == m.out_wr;
    if (empty) st |= 0x80000000u;
    if (m.need) st |= 0x20000000u;                                  /* busy */
    if ((m.ctrl & 0x40000000u) && m.need) st |= 0x10000000u;        /* data-in request */
    if ((m.ctrl & 0x20000000u) && !empty) st |= 0x08000000u;        /* data-out request */
    st |= ((m.cmd >> 27) & 0xF) << 23;                              /* depth, signed, bit15 */
    st |= (uint32_t)(m.block & 7) << 16;
    st |= (m.need - 1) & 0xFFFF;
    return st;
}

/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
void mdec_state(StateIO *st) {
    st_prefix(st, "mdec");
    ST(st, m.cmd); ST(st, m.need); ST(st, m.nin); ST(st, m.in); ST(st, m.ctrl); ST(st, m.iq_y); ST(st, m.iq_uv);
    ST(st, m.scale); ST(st, m.block);
    /* only the unread part of the output ring */
    uint32_t live = st_loading(st) ? 0 : m.out_wr - m.out_rd;
    ST_N(st, "out_live_words", &live, 4);
    if (live > OUT_MAX) live = OUT_MAX;
    uint32_t *tmp = live ? malloc((size_t)live * 4) : NULL;
    if (live && !tmp) live = 0;
    if (!st_loading(st))
        for (uint32_t i = 0; i < live; i++) tmp[i] = m.out[(m.out_rd + i) & (OUT_MAX - 1)];
    ST_N(st, "out_live", tmp, (size_t)live * 4);
    if (st_loading(st)) {
        for (uint32_t i = 0; i < live; i++) m.out[i] = tmp[i];
        m.out_rd = 0; m.out_wr = live;
    }
    free(tmp);
}
