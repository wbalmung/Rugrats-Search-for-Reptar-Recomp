/* SPU: 24 ADPCM voices, ADSR, sound RAM, DMA, CD/XA input. Output 44.1 kHz stereo, Gaussian voice interpolation. Reverb at 22.05 kHz. */
#include <stdlib.h>
#include <math.h>
#include "hw.h"

static uint8_t sram[0x80000];
static uint16_t regs[0x200];  /* raw register mirror, index = offset/2 */

typedef struct {
    uint32_t addr, start, repeat;
    uint32_t counter;      /* 12-bit fraction pitch counter */
    int16_t buf[28]; int bpos;
    int16_t prev[2];
    int16_t last[4];
    int phase;             /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    int32_t level;
    int32_t env_count;
    int ignore_loop;
    int32_t cut, cut_n, cut_fade; /* spu_cut_next: fade out over cut_fade after `cut` output samples (0 = none); not saved */
} Voice;

static Voice vo[24];

/* Cut the next key-on of `voice` (0..23) after `samples` 44.1 kHz output samples, with a short fade. Used for the
   Reptar bar sound (chime + roar in one sample, see hooks.c). The request expires after ~0.5 s. By voice, not by
   sample address: each level loads its sound bank elsewhere in SPU RAM (0x61b40 in one level, 0x512c0 in the toy
   store), and a fixed address missed the sound there. */
static int cut_req_voice = -1;
static int32_t cut_req_samples, cut_req_fade, cut_req_ttl;
void spu_cut_next(int voice, int samples, int fade) {
    if (voice < 0 || voice > 23) return;
    cut_req_voice = voice; cut_req_samples = samples; cut_req_fade = fade > 0 ? fade : 1; cut_req_ttl = 22050;
}
static uint32_t xfer_addr;
static uint32_t endx;
static int16_t cd_ring[16384 * 2];
static uint32_t cd_w, cd_r;
enum { CD_PRIME = 1024 };
static int cd_prime = CD_PRIME;
static uint32_t cd_frac;
static int dbg_irq_seen;
/* RUGRATS_AUDIOLOG counters */
uint32_t spu_stat_cd_under, spu_stat_cd_over, spu_stat_clip;
static int spu_stat_cd_play;

#define REG(off) regs[(off) >> 1]

void spu_init(void) {
    memset(vo, 0, sizeof vo);
}

static void check_irq(uint32_t addr) {
    if ((REG(0x1AA) & 0x40) && (addr & ~7u) == ((uint32_t)REG(0x1A4) << 3)) {
        if (!dbg_irq_seen++) rt_logf("SPU IRQ hit at %05x\n", addr);
        irq_raise(IRQ_SPU);
    }
}

/* PS1 SPU 4-tap Gaussian interpolation table (512 entries, psx-spx order) */
static const int16_t gauss[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7,
    8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24,
    25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56,
    58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111,
    116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199,
    205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328,
    338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512,
    525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761,
    779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089,
    1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508,
    1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031,
    2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668,
    2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427,
    3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315,
    4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332,
    5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475,
    6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735,
    7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097,
    9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542,
    10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045,
    12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577,
    13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104,
    15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592,
    16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001,
    18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295,
    19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439,
    20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399,
    21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148,
    22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662,
    22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927,
    22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};
static int interp_linear = -1; /* RUGRATS_LINEAR=1: old linear interpolation (A/B) */

/* ---- ADPCM ---------------------------------------------------------- */
static const int k0[5] = { 0, 60, 115, 98, 122 }, k1[5] = { 0, 0, -52, -55, -60 };

static void decode_block(Voice *v, int n) {
    const uint8_t *b = sram + (v->addr & 0x7FFF0);
    check_irq(v->addr);
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 4;
    uint8_t flags = b[1];
    if (flags & 4 && !v->ignore_loop) v->repeat = v->addr;
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int32_t s = (int16_t)(nib << 12) >> shift;
        s += (v->prev[0] * k0[filt] + v->prev[1] * k1[filt] + 32) >> 6;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v->prev[1] = v->prev[0];
        v->prev[0] = (int16_t)s;
        v->buf[i] = (int16_t)s;
    }
    if (flags & 1) {
        endx |= 1u << n;
        if (flags & 2) v->addr = v->repeat;
        else { v->addr = v->repeat; v->phase = 4; v->level = 0; }
    } else {
        v->addr = (v->addr + 16) & 0x7FFFF;
    }
}

/* ---- ADSR ------------------------------------------------------------ */
static void env_step(Voice *v, int exp, int dec, int shift, int step, int32_t target, int *done) {
    int32_t s = dec ? (-8 + step) : (7 - step);
    int32_t cyc = 1 << (shift - 11 > 0 ? shift - 11 : 0);
    s = s << (11 - shift > 0 ? 11 - shift : 0);
    if (exp && !dec && v->level > 0x6000) cyc *= 4;
    if (exp && dec) s = (s * v->level) >> 15;
    if (--v->env_count > 0) return;
    v->env_count = cyc;
    v->level += s;
    if (v->level < 0) v->level = 0;
    if (v->level > 0x7FFF) v->level = 0x7FFF;
    if (done) *done = dec ? (v->level <= target) : (v->level >= target);
}

static void adsr(Voice *v, int n) {
    uint16_t lo = REG(n * 16 + 8), hi = REG(n * 16 + 10);
    int done = 0;
    switch (v->phase) {
    case 1:
        env_step(v, lo >> 15, 0, (lo >> 10) & 31, (lo >> 8) & 3, 0x7FFF, &done);
        if (done) { v->phase = 2; v->env_count = 0; }
        break;
    case 2: {
        int32_t sl = ((lo & 15) + 1) * 0x800;
        env_step(v, 1, 1, ((lo >> 4) & 15) << 2, 0, sl, &done);
        if (done) { v->phase = 3; v->env_count = 0; }
        break;
    }
    case 3:
        env_step(v, hi >> 15, (hi >> 14) & 1, (hi >> 8) & 31, (hi >> 6) & 3, 0, 0);
        break;
    case 4: {
        /* opt.note_fade: the music engine releases notes with ADSR2 = 0000 (linear rate 0 = -16384 per sample), so a
           note drops from full to 0 in 2 samples = a click at every note end (PCSX-Redux does the same: it is in the
           game). With the option on, a release step is capped at NOTE_FADE_STEP (full scale -> 0 in ~2.9 ms). */
        enum { NOTE_FADE_STEP = 256 };
        int32_t before = v->level;
        env_step(v, (hi >> 5) & 1, 1, (hi & 31) << 2, 0, 0, &done);
        if (opt.note_fade && before - v->level > NOTE_FADE_STEP) v->level = before - NOTE_FADE_STEP;
        if (v->level <= 0) { v->level = 0; v->phase = 0; }
        break;
    }
    }
    REG(n * 16 + 12) = (uint16_t)v->level;
}

static int32_t vol_of(uint16_t r) {
    if (r & 0x8000) return 0x7FFF; /* sweep mode: approximate as full */
    return (int16_t)(r << 1) ;
}

static void key_on(int n) {
    Voice *v = &vo[n];
    if (getenv("RUGRATS_KOFFLOG") && v->phase) {
        uint32_t hooks_frame(void);
        rt_logf("kon-retrig v%d vbl %u lvl %d ph %d adsr %04x/%04x start %05x\n", n, hooks_frame(), v->level, v->phase,
                REG(n * 16 + 8), REG(n * 16 + 10), v->start);
    }
    v->start = (uint32_t)REG(n * 16 + 6) << 3;
    v->addr = v->start;
    v->repeat = (uint32_t)REG(n * 16 + 14) << 3;
    v->counter = 0;
    v->bpos = 28;
    v->prev[0] = v->prev[1] = 0;
    memset(v->last, 0, sizeof v->last);
    v->phase = 1;
    v->level = 0;
    v->env_count = 0;
    v->ignore_loop = 0;
    v->cut = v->cut_n = 0;
    if (n == cut_req_voice) { v->cut = cut_req_samples; v->cut_fade = cut_req_fade; cut_req_voice = -1; }
    endx &= ~(1u << n);
}

static void key_off(int n) {
    if (getenv("RUGRATS_KOFFLOG") && vo[n].phase) {
        uint32_t hooks_frame(void);
        rt_logf("koff v%d vbl %u lvl %d ph %d adsr %04x/%04x start %05x last %d\n", n, hooks_frame(), vo[n].level, vo[n].phase,
                REG(n * 16 + 8), REG(n * 16 + 10), vo[n].start, vo[n].last[3]);
    }
    if (vo[n].phase) { vo[n].phase = 4; vo[n].env_count = 0; }
}

/* ---- reverb ------------------------------------------------------------
   PS1 reverb (nocash psx-spx algorithm). Runs at 22.05 kHz on a work area in sound RAM from mBASE
   to the end. Input = sum of EON voices (+ CD if SPUCNT bit 2), output added before main volume. */
static uint32_t rv_cur;          /* current buffer address (bytes) */
static int rv_off = -1;          /* RUGRATS_NOREVERB=1 disables (A/B by ear) */
static int rv_logged;

static inline uint32_t rv_addr(int32_t off) {
    uint32_t base = (uint32_t)REG(0x1A2) << 3;
    int32_t size = 0x80000 - (int32_t)base;
    int32_t rel = (int32_t)(rv_cur - base) + off;
    rel %= size;
    if (rel < 0) rel += size;
    return (base + (uint32_t)rel) & 0x7FFFE;
}
static inline int32_t rv_rd(int32_t off) {
    uint32_t a = rv_addr(off);
    return (int16_t)(sram[a] | (sram[a + 1] << 8));
}
static inline void rv_wr(int32_t off, int32_t v) {
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    uint32_t a = rv_addr(off);
    sram[a] = (uint8_t)v;
    sram[a + 1] = (uint8_t)(v >> 8);
}
#define RV(o) ((int32_t)(int16_t)REG(o))       /* signed volume/coefficient */
#define RA(o) ((int32_t)REG(o) << 3)           /* address/offset in bytes */
#define MUL(a, b) (((a) * (b)) >> 15)

static void reverb_tick(int32_t lin, int32_t rin, int32_t *lo, int32_t *ro) {
    int write = (REG(0x1AA) & 0x80) != 0;
    lin = MUL(lin, RV(0x1FC));
    rin = MUL(rin, RV(0x1FE));
    int32_t vIIR = RV(0x1C4), vWALL = RV(0x1CE);
    if (write) {
        int32_t ls = rv_rd(RA(0x1D4) - 2), rs = rv_rd(RA(0x1D6) - 2);
        int32_t ld = rv_rd(RA(0x1E4) - 2), rd = rv_rd(RA(0x1E6) - 2);
        rv_wr(RA(0x1D4), MUL(lin + MUL(rv_rd(RA(0x1E0)), vWALL) - ls, vIIR) + ls);
        rv_wr(RA(0x1D6), MUL(rin + MUL(rv_rd(RA(0x1E2)), vWALL) - rs, vIIR) + rs);
        rv_wr(RA(0x1E4), MUL(lin + MUL(rv_rd(RA(0x1F2)), vWALL) - ld, vIIR) + ld);
        rv_wr(RA(0x1E6), MUL(rin + MUL(rv_rd(RA(0x1F0)), vWALL) - rd, vIIR) + rd);
    }
    int32_t c1 = RV(0x1C6), c2 = RV(0x1C8), c3 = RV(0x1CA), c4 = RV(0x1CC);
    int32_t L = MUL(c1, rv_rd(RA(0x1D8))) + MUL(c2, rv_rd(RA(0x1DC))) + MUL(c3, rv_rd(RA(0x1E8))) + MUL(c4, rv_rd(RA(0x1EC)));
    int32_t R = MUL(c1, rv_rd(RA(0x1DA))) + MUL(c2, rv_rd(RA(0x1DE))) + MUL(c3, rv_rd(RA(0x1EA))) + MUL(c4, rv_rd(RA(0x1EE)));
    int32_t a1 = RV(0x1D0), a2 = RV(0x1D2), d1 = RA(0x1C0), d2 = RA(0x1C2);
    int32_t t;
    t = rv_rd(RA(0x1F4) - d1); L -= MUL(a1, t); if (write) rv_wr(RA(0x1F4), L); L = MUL(L, a1) + t;
    t = rv_rd(RA(0x1F6) - d1); R -= MUL(a1, t); if (write) rv_wr(RA(0x1F6), R); R = MUL(R, a1) + t;
    t = rv_rd(RA(0x1F8) - d2); L -= MUL(a2, t); if (write) rv_wr(RA(0x1F8), L); L = MUL(L, a2) + t;
    t = rv_rd(RA(0x1FA) - d2); R -= MUL(a2, t); if (write) rv_wr(RA(0x1FA), R); R = MUL(R, a2) + t;
    *lo = MUL(L, (int32_t)(int16_t)REG(0x184));
    *ro = MUL(R, (int32_t)(int16_t)REG(0x186));
    uint32_t base = (uint32_t)REG(0x1A2) << 3;
    rv_cur = (rv_cur + 2) & 0x7FFFE;
    if (rv_cur < base) rv_cur = base;
    if (write && !rv_logged && (REG(0x184) || REG(0x186))) {
        rv_logged = 1;
        rt_logf("SPU reverb on: mBASE=%05x vLOUT=%04x vROUT=%04x vIIR=%04x EON=%04x%04x\n", base, REG(0x184),
                REG(0x186), REG(0x1C4), REG(0x19A), REG(0x198));
    }
}

/* ---- mixing ----------------------------------------------------------- */
static int pop_lim = -2;          /* RUGRATS_SPUPOP=<jump>: log per-voice output jumps (debug) */
static int32_t pop_last[24];
static FILE *pop_stem;            /* RUGRATS_SPUSTEM=<file>: per-voice output (24 x int16 per 44.1 kHz frame, debug) */
static int16_t pop_row[24];
static void mix(int16_t *out, int frames) {
    uint16_t cnt = REG(0x1AA);
    int32_t mvl = vol_of(REG(0x180)), mvr = vol_of(REG(0x182));
    int32_t cdl = (int16_t)REG(0x1B0), cdr = (int16_t)REG(0x1B2);
    uint32_t pmon = REG(0x190) | ((uint32_t)REG(0x192) << 16);
    uint32_t eon = REG(0x198) | ((uint32_t)REG(0x19A) << 16);
    static int rv_phase;
    static int32_t rv_accL, rv_accR, rv_prevL, rv_prevR, rv_curL, rv_curR;
    if (rv_off < 0) rv_off = getenv("RUGRATS_NOREVERB") != 0;
    if (interp_linear < 0) interp_linear = getenv("RUGRATS_LINEAR") != 0;
    if (pop_lim == -2) {
        const char *e = getenv("RUGRATS_SPUPOP"); pop_lim = e ? atoi(e) : -1;
        if ((e = getenv("RUGRATS_SPUSTEM"))) pop_stem = fopen(e, "wb");
    }
    /* category volumes (F1 Audio page): libsnd's sequencer (music) only allocates voices below its reserved count
       (SsSetReservedVoice(14) at sound init, byte at 0x800EAD8C); the game keys its effects and ambient loops on
       voices it picks itself above that (SsUtKeyOnV, voices 15..23). XA from the CD outside movies = dialogue. */
    int resv = (int8_t)mem_r8(0x800EAD8Cu);
    if (resv < 1 || resv > 24) resv = 14;
    const int32_t g_mus = opt.vol_music * 32768 / 100, g_sfx = opt.vol_sfx * 32768 / 100;
    const int32_t g_cd = hooks_movie_active() ? 32768 : opt.vol_voice * 32768 / 100;
    if (cut_req_voice >= 0 && (cut_req_ttl -= frames) <= 0) cut_req_voice = -1;
    for (int f = 0; f < frames; f++) {
        int32_t L = 0, Rr = 0, revL = 0, revR = 0;
        int32_t prev_out = 0;
        if (pop_stem) { fwrite(pop_row, 2, 24, pop_stem); memset(pop_row, 0, sizeof pop_row); }
        for (int n = 0; n < 24; n++) {
            Voice *v = &vo[n];
            if (!v->phase) {
                if (pop_lim > 0 && (pop_last[n] > pop_lim || pop_last[n] < -pop_lim))
                    rt_logf("pop v%d OFF from s %d addr %05x start %05x\n", n, pop_last[n], v->addr, v->start);
                pop_last[n] = 0;
                prev_out = 0; continue;
            }
            uint32_t pitch = REG(n * 16 + 4);
            if (pitch > 0x4000) pitch = 0x4000;
            if (n > 0 && (pmon & (1u << n))) {
                int32_t factor = prev_out + 0x8000;
                pitch = (uint32_t)(((int32_t)pitch * factor) >> 15) & 0xFFFF;
                if (pitch > 0x4000) pitch = 0x4000;
            }
            int idx = v->counter >> 12;
            int frac = v->counter & 0xFFF;
            int32_t s;
            if (interp_linear) {
                int16_t s0 = v->last[2], s1 = v->last[3];
                s = s0 + (((s1 - s0) * frac) >> 12);
            } else {
                /* PS1 4-tap Gaussian: last[0] oldest .. last[3] newest, phase = 8 bits of the fraction */
                int i = frac >> 4;
                s = ((gauss[0x0FF - i] * v->last[0]) >> 15) + ((gauss[0x1FF - i] * v->last[1]) >> 15) +
                    ((gauss[0x100 + i] * v->last[2]) >> 15) + ((gauss[i] * v->last[3]) >> 15);
            }
            v->counter += pitch;
            int adv = v->counter >> 12;
            v->counter &= 0xFFF;
            (void)idx;
            for (int k = 0; k < adv; k++) {
                if (v->bpos >= 28) { decode_block(v, n); v->bpos = 0; }
                v->last[0] = v->last[1];
                v->last[1] = v->last[2];
                v->last[2] = v->last[3];
                v->last[3] = v->buf[v->bpos++];
            }
            adsr(v, n);
            s = (s * v->level) >> 15;
            if (v->cut && ++v->cut_n > v->cut) {   /* spu_cut_next: fade, then stop */
                int32_t left = v->cut + v->cut_fade - v->cut_n;
                if (left <= 0) { v->phase = 0; v->level = 0; v->cut = 0; endx |= 1u << n; s = 0; }
                else s = (int32_t)((int64_t)s * left / v->cut_fade);
            }
            {   /* RUGRATS_SPUPOP=<jump>: log per-voice output jumps (debug) */
                static uint32_t popn;
                if (pop_lim > 0) {
                    int32_t j = s - pop_last[n];
                    if ((j > pop_lim || j < -pop_lim) && popn < 3000) {
                        popn++;
                        uint32_t hooks_frame(void);
                        rt_logf("pop v%d vbl %u f %d jump %d s %d lvl %d ph %d addr %05x start %05x pitch %04x vol %04x/%04x adsr %04x/%04x non %04x%04x\n",
                                n, hooks_frame(), f, j, s, v->level, v->phase, v->addr, v->start, pitch, REG(n * 16), REG(n * 16 + 2),
                                REG(n * 16 + 8), REG(n * 16 + 10), REG(0x196), REG(0x194));
                    }
                    pop_last[n] = s;
                }
            }
            if (pop_stem) pop_row[n] = (int16_t)s;
            prev_out = s;
            int32_t vl =(s * vol_of(REG(n * 16 + 0))) >> 15, vr = (s * vol_of(REG(n * 16 + 2))) >> 15;
            {
                const int32_t g = n < resv ? g_mus : g_sfx;
                if (g != 32768) { vl = (vl * g) >> 15; vr = (vr * g) >> 15; }
            }
            L += vl;
            Rr += vr;
            if (eon & (1u << n)) { revL += vl; revR += vr; }
        }
        /* CD start-up cushion: once the ring has run empty, new CD audio waits CD_PRIME frames (23 ms) before it
           plays, so sector arrival jitter at the start of a voice line doesn't cut holes into it */
        int cd_has = cd_r != cd_w;
        if (!cd_has) cd_prime = CD_PRIME;
        else if (cd_prime > 0) { cd_prime--; cd_has = 0; }
        /* CD into reverb (SPUCNT bit 2) */
        if ((cnt & 5) == 5 && cd_has) {
            revL += (((cd_ring[(cd_r & 16383) * 2] * cdl) >> 15) * g_cd) >> 15;
            revR += (((cd_ring[(cd_r & 16383) * 2 + 1] * cdr) >> 15) * g_cd) >> 15;
        }
        /* reverb at 22.05 kHz: average two input samples, linear-interpolate the output */
        if (!rv_phase) {
            rv_accL = revL; rv_accR = revR;
            L += (rv_prevL + rv_curL) >> 1;
            Rr += (rv_prevR + rv_curR) >> 1;
        } else {
            rv_prevL = rv_curL; rv_prevR = rv_curR;
            if (rv_off) rv_curL = rv_curR = 0;
            else reverb_tick((rv_accL + revL) >> 1, (rv_accR + revR) >> 1, &rv_curL, &rv_curR);
            L += rv_curL;
            Rr += rv_curR;
        }
        rv_phase ^= 1;
        /* PS1: the voice + reverb sum is clamped to 16 bits BEFORE the main volume. Until 2026-11-10 it was not,
           and L * mvl (int32) overflowed once the sum passed ~65535 (Reptar 2010 landings: up to 5 near-full-scale
           stomp voices): samples flipped sign = loud static. */
        if (L > 32767 || L < -32768 || Rr > 32767 || Rr < -32768) spu_stat_clip++;
        if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
        if (Rr > 32767) Rr = 32767; else if (Rr < -32768) Rr = -32768;
        L = (L * mvl) >> 15;
        Rr = (Rr * mvr) >> 15;
        if (!(cnt & 0x4000)) L = Rr = 0; /* muted */
        /* CD input */
        if ((cnt & 1) && cd_has) {
            L += (((cd_ring[(cd_r & 16383) * 2] * cdl) >> 15) * g_cd) >> 15;
            Rr += (((cd_ring[(cd_r & 16383) * 2 + 1] * cdr) >> 15) * g_cd) >> 15;
            cd_r++;
            spu_stat_cd_play = 1;
        } else if (cd_has) {
            cd_r++;
        } else if (spu_stat_cd_play) {
            spu_stat_cd_under++; /* CD stream ran dry while playing */
            spu_stat_cd_play = 0;
        }
        if (L > 32767 || L < -32768 || Rr > 32767 || Rr < -32768) spu_stat_clip++;
        out[f * 2] = (int16_t)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        out[f * 2 + 1] = (int16_t)(Rr > 32767 ? 32767 : Rr < -32768 ? -32768 : Rr);
    }
}

void spu_frame(void) {
    static int16_t out[800 * 2];
    static int acc;
    /* 44100 / 60 = 735 */
    int frames = 735;
    acc += 0;
    mix(out, frames);
    host_audio_push(out, frames);
}

/* Catmull-Rom cubic between y0 and y1 at t in [0,1) (16.16 fraction) */
static inline int16_t cubic(int ym1, int y0, int y1, int y2, uint32_t frac) {
    double t = (frac & 0xFFFF) / 65536.0;
    double v = y0 + 0.5 * t * (y1 - ym1 + t * (2.0 * ym1 - 5.0 * y0 + 4.0 * y1 - y2 + t * (3.0 * (y0 - y1) + y2 - ym1)));
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void spu_cd_audio(const int16_t *lr, int frames, int rate) {
    /* resample 37.8 / 18.9 kHz XA to 44.1 kHz with cubic interpolation (nearest-neighbour sounded gritty);
       the last 3 input frames of the previous sector carry over so sector edges are seamless */
    static int16_t hist[3 * 2];
    static int16_t tmp[(3 + 18 * 8 * 28) * 2];
    if (frames > 18 * 8 * 28) frames = 18 * 8 * 28;
    memcpy(tmp, hist, sizeof hist);
    memcpy(tmp + 6, lr, (size_t)frames * 4);
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 44100);
    uint32_t pos = cd_frac;
    while ((pos >> 16) < (uint32_t)frames) {
        if (cd_w - cd_r >= 16000) { spu_stat_cd_over++; break; }
        int i = pos >> 16; /* interpolate between tmp[i+1] and tmp[i+2] */
        for (int ch = 0; ch < 2; ch++)
            cd_ring[(cd_w & 16383) * 2 + ch] =
                cubic(tmp[i * 2 + ch], tmp[(i + 1) * 2 + ch], tmp[(i + 2) * 2 + ch], tmp[(i + 3) * 2 + ch], pos);
        cd_w++;
        pos += step;
    }
    cd_frac = pos - ((uint32_t)frames << 16);
    if ((int32_t)cd_frac < 0) cd_frac = 0;
    memcpy(hist, tmp + (size_t)frames * 2, sizeof hist);
}

/* ---- registers -------------------------------------------------------- */
uint16_t spu_read(uint32_t off) {
    off &= 0x3FE;
    if (off >= 0x200) return 0;
    if (off < 0x180 && (off & 15) == 12) return (uint16_t)vo[off >> 4].level;
    switch (off) {
    case 0x19C: return (uint16_t)endx;
    case 0x19E: return (uint16_t)(endx >> 16);
    case 0x1A6: return (uint16_t)(xfer_addr >> 3);
    case 0x1AE: {
        uint16_t cnt = REG(0x1AA);
        return (cnt & 0x3F) | ((cnt & 0x20) ? 0x80 : 0) | (((cnt >> 4) & 3) == 2 ? 0x200 : 0) |
               (((cnt >> 4) & 3) == 3 ? 0x100 : 0);
    }
    }
    return REG(off);
}

void spu_write(uint32_t off, uint16_t v) {
    off &= 0x3FE;
    if (off >= 0x200) return;
    {
        static int nlog;
        if (((off >= 0x1C0) || off == 0x180 || off == 0x182 || off == 0x1B0 || off == 0x1B2 || off == 0x184 || off == 0x186 || off == 0x1A2 || off == 0x198 || off == 0x19A ||
             off == 0x1AA) && ((v ^ REG(off)) & (off == 0x1AA ? 0x84 : 0xFFFF)) && nlog < 200 &&
            getenv("RUGRATS_SPULOG")) {
            nlog++;
            rt_logf("SPU w %03x = %04x\n", off, v);
        }
    }
    if (off < 0x180 && ((off & 15) == 8 || (off & 15) == 10) && getenv("RUGRATS_ADSRLOG")) {
        uint32_t hooks_frame(void);
        rt_logf("adsr%d v%d = %04x (was %04x) vbl %u ra %08x\n", (off & 15) == 8 ? 1 : 2, off >> 4, v, REG(off), hooks_frame(), cpu.r[31]);
    }
    REG(off) = v;
    if (off < 0x180 && (off & 15) == 6) { /* RUGRATS_KONLOG=1: voice start address writes (debug) */
        static int kl2 = -1;
        if (kl2 < 0) kl2 = getenv("RUGRATS_KONLOG") != NULL;
        if (kl2) {
            uint32_t hooks_frame(void); int hw_in_exception(void);
            rt_logf("vaddr %d = %05x vbl %u irq %d ra %08x\n", off >> 4, v << 3, hooks_frame(), hw_in_exception(), cpu.r[31]);
        }
    }
    if ((off == 0x188 || off == 0x18A) && v) { /* RUGRATS_KONLOG=1: key-ons with a stack scan (debug) */
        static int kl = -1;
        if (kl < 0) kl = getenv("RUGRATS_KONLOG") != NULL;
        if (kl) {
            uint32_t hooks_frame(void);
            char b[400]; int n = 0;
            for (uint32_t a = cpu.r[29]; a < cpu.r[29] + 0x200 && n < 360; a += 4) {
                uint32_t w = mem_r32(a);
                if (w >= 0x80010000u && w < 0x800B6000u && (w & 3) == 0) n += snprintf(b + n, sizeof b - n, " %08x", w);
            }
            int hw_in_exception(void);
            rt_logf("kon %03x=%04x vbl %u irq %d ra %08x |%s\n", off, v, hooks_frame(), hw_in_exception(), cpu.r[31], b);
        }
    }
    switch (off) {
    case 0x188: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
    case 0x18A: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
    case 0x18C: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
    case 0x18E: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
    case 0x19C: case 0x19E: break;
    case 0x1A2: rv_cur = (uint32_t)v << 3; break;
    case 0x1A6: xfer_addr = (uint32_t)v << 3; break;
    case 0x1A8:
        check_irq(xfer_addr);
        sram[xfer_addr & 0x7FFFF] = (uint8_t)v;
        sram[(xfer_addr + 1) & 0x7FFFF] = (uint8_t)(v >> 8);
        xfer_addr = (xfer_addr + 2) & 0x7FFFF;
        break;
    case 0x1AA:
        if (!(v & 0x40)) i_stat &= ~(1u << IRQ_SPU);
        break;
    default:
        if (off < 0x180 && (off & 15) == 14) {
            int n = off >> 4;
            vo[n].repeat = (uint32_t)v << 3;
            vo[n].ignore_loop = 1;
        }
        break;
    }
}

void spu_dma_write(uint32_t w) {
    check_irq(xfer_addr);
    memcpy(sram + (xfer_addr & 0x7FFFC), &w, 4);
    xfer_addr = (xfer_addr + 4) & 0x7FFFF;
}

uint32_t spu_dma_read(void) {
    uint32_t w;
    memcpy(&w, sram + (xfer_addr & 0x7FFFC), 4);
    xfer_addr = (xfer_addr + 4) & 0x7FFFF;
    return w;
}

/* ---- save state (savestate.c) ---------------------------------------- */
/* debug (emuref): sound RAM + raw SPU registers for RUGRATS_EXPORT */
const uint8_t *spu_export_sram(void) { return sram; }
const uint16_t *spu_export_regs(void) { return regs; }
#include "savestate.h"
void spu_state(StateIO *s) {
    st_prefix(s, "spu");
    ST_N(s, "sram", sram, sizeof sram);
    ST(s, regs);
    for (int i = 0; i < 24; i++) {
        char n[40];
        #define VF(f) snprintf(n, sizeof n, "vo%d." #f, i); ST_N(s, n, &vo[i].f, sizeof vo[i].f)
        VF(addr); VF(start); VF(repeat); VF(counter); VF(buf); VF(bpos); VF(prev); VF(last); VF(phase); VF(level);
        VF(env_count); VF(ignore_loop);
        #undef VF
    }
    ST(s, xfer_addr); ST(s, endx); ST(s, cd_ring); ST(s, cd_w); ST(s, cd_r); ST(s, cd_frac); ST(s, rv_cur);
}
