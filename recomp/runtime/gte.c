/* Geometry Transformation Engine (COP2), per psx-spx. Widescreen hook in perspective transform. */
#include <math.h>
#include "fastmath.h"
#include <stdlib.h>
#include <stdio.h>
#include "hw.h"

typedef struct { int16_t x, y, z; } V3;

static V3 V[3];
static uint8_t RGBC[4];
static uint16_t OTZ;
static int16_t IR[4];
static int16_t SXY[4][2]; /* fifo 0..2, [3] unused */
/* sub-pixel fraction of each SXY fifo entry (1/256 px; SXK 1 = has one, 2 = projected without one, 0 = unknown,
   e.g. written by mtc2). gte_swc2 tags the RAM word the game stores it to, so gpu.c finds each packet vertex's own
   position instead of guessing by screen pixel (see subpx_tag). */
static int16_t SXF[3][2];
static uint8_t SXK[3];
static float SXZ[3];   /* 4096*4096/z of each entry (-1 unknown): tags carry the exact depth too */
static float SXP[3][3]; static uint8_t SXPK[3];   /* camera-space point of each entry (SXPK 0 unknown) */
void subpx_tag_p3(const float *p);
const float *subpx_tag_p3out(void);
void subpx_tag(uint32_t addr, uint32_t val, int has, int fx, int fy);
void subpx_tag_z(float iz);
float subpx_tag_zout(void);
static uint16_t SZ[4];
static uint8_t RGB[3][4];
static uint32_t RES1;
static int32_t MAC[4];
static uint32_t LZCS, LZCR;

static int16_t RT[3][3], LM[3][3], LCM[3][3];
static int32_t TR[3], BK[3], FC[3];
static int32_t OFX, OFY;
static uint16_t H;
static int16_t DQA;
static int32_t DQB;
static int16_t ZSF3, ZSF4;
static uint32_t FLAG;

static int sf, lm;

/* ---- helpers -------------------------------------------------------- */
static int64_t mac_chk(int i, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL) FLAG |= 1u << (31 - i);
    else if (v < -0x80000000000LL) FLAG |= 1u << (28 - i);
    /* sign-extend to 44 bits */
    return ((v << 20) >> 20);
}

static int32_t set_mac(int i, int64_t v) {
    if (i == 0) {
        if (v > 0x7FFFFFFFLL) FLAG |= 1u << 16;
        else if (v < -0x80000000LL) FLAG |= 1u << 15;
        MAC[0] = (int32_t)v;
        return MAC[0];
    }
    v = mac_chk(i, v);
    MAC[i] = (int32_t)(v >> (sf * 12));
    return MAC[i];
}

static int16_t sat_ir(int i, int32_t v, int lmv) {
    int32_t lo = lmv ? 0 : -0x8000;
    if (v < lo) { FLAG |= 1u << (25 - i); return (int16_t)lo; }
    if (v > 0x7FFF) { FLAG |= 1u << (25 - i); return 0x7FFF; }
    return (int16_t)v;
}

static void set_ir(int i, int32_t v, int lmv) { IR[i] = sat_ir(i, v, lmv); }

static void set_ir0(int64_t v) {
    if (v < 0) { FLAG |= 1u << 12; IR[0] = 0; }
    else if (v > 0x1000) { FLAG |= 1u << 12; IR[0] = 0x1000; }
    else IR[0] = (int16_t)v;
}

static uint16_t sat_z(int64_t v) {
    if (v < 0) { FLAG |= 1u << 18; return 0; }
    if (v > 0xFFFF) { FLAG |= 1u << 18; return 0xFFFF; }
    return (uint16_t)v;
}

static uint8_t sat_col(int i, int32_t v) {
    if (v < 0) { FLAG |= 1u << (21 - i); return 0; }
    if (v > 255) { FLAG |= 1u << (21 - i); return 255; }
    return (uint8_t)v;
}

static void push_color(void) {
    memcpy(RGB[0], RGB[1], 4);
    memcpy(RGB[1], RGB[2], 4);
    RGB[2][0] = sat_col(0, MAC[1] >> 4);
    RGB[2][1] = sat_col(1, MAC[2] >> 4);
    RGB[2][2] = sat_col(2, MAC[3] >> 4);
    RGB[2][3] = RGBC[3];
}

static void push_z(int64_t z) {
    SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
    SZ[3] = sat_z(z);
}

static void ws_init(void);
static uint8_t unr_table[0x101];
static void init_unr(void) {
    for (int i = 0; i < 0x101; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
}

static uint32_t gte_divide(uint16_t h, uint16_t sz3) {
    if (h >= sz3 * 2) { FLAG |= 1u << 17; return 0x1FFFF; }
    int z = 0;
    while (z < 16 && !(sz3 & (0x8000 >> z))) z++;
    uint64_t n = (uint64_t)h << z;
    uint32_t d = (uint32_t)sz3 << z;
    uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - (d * u)) >> 8;
    d = (0x0000080 + (d * u)) >> 8;
    uint64_t r = ((n * d) + 0x8000) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
}

/* ---- register access ------------------------------------------------ */
static uint32_t pack16(int16_t a, int16_t b) { return (uint16_t)a | ((uint32_t)(uint16_t)b << 16); }

uint32_t gte_read(int r) {
    switch (r) {
    case 0: return pack16(V[0].x, V[0].y);
    case 1: return (uint32_t)(int32_t)V[0].z;
    case 2: return pack16(V[1].x, V[1].y);
    case 3: return (uint32_t)(int32_t)V[1].z;
    case 4: return pack16(V[2].x, V[2].y);
    case 5: return (uint32_t)(int32_t)V[2].z;
    case 6: { uint32_t v; memcpy(&v, RGBC, 4); return v; }
    case 7: return OTZ;
    case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)IR[r - 8];
    case 12: case 13: case 14: return pack16(SXY[r - 12][0], SXY[r - 12][1]);
    case 15: return pack16(SXY[2][0], SXY[2][1]);
    case 16: case 17: case 18: case 19: return SZ[r - 16];
    case 20: case 21: case 22: { uint32_t v; memcpy(&v, RGB[r - 20], 4); return v; }
    case 23: return RES1;
    case 24: case 25: case 26: case 27: return (uint32_t)MAC[r - 24];
    case 28: case 29: {
        int r5 = IR[1] >> 7, g5 = IR[2] >> 7, b5 = IR[3] >> 7;
        r5 = r5 < 0 ? 0 : r5 > 31 ? 31 : r5;
        g5 = g5 < 0 ? 0 : g5 > 31 ? 31 : g5;
        b5 = b5 < 0 ? 0 : b5 > 31 ? 31 : b5;
        return (uint32_t)(r5 | (g5 << 5) | (b5 << 10));
    }
    case 30: return LZCS;
    case 31: return LZCR;
    /* control */
    case 32: return pack16(RT[0][0], RT[0][1]);
    case 33: return pack16(RT[0][2], RT[1][0]);
    case 34: return pack16(RT[1][1], RT[1][2]);
    case 35: return pack16(RT[2][0], RT[2][1]);
    case 36: return (uint32_t)(int32_t)RT[2][2];
    case 37: case 38: case 39: return (uint32_t)TR[r - 37];
    case 40: return pack16(LM[0][0], LM[0][1]);
    case 41: return pack16(LM[0][2], LM[1][0]);
    case 42: return pack16(LM[1][1], LM[1][2]);
    case 43: return pack16(LM[2][0], LM[2][1]);
    case 44: return (uint32_t)(int32_t)LM[2][2];
    case 45: case 46: case 47: return (uint32_t)BK[r - 45];
    case 48: return pack16(LCM[0][0], LCM[0][1]);
    case 49: return pack16(LCM[0][2], LCM[1][0]);
    case 50: return pack16(LCM[1][1], LCM[1][2]);
    case 51: return pack16(LCM[2][0], LCM[2][1]);
    case 52: return (uint32_t)(int32_t)LCM[2][2];
    case 53: case 54: case 55: return (uint32_t)FC[r - 53];
    case 56: return (uint32_t)OFX;
    case 57: return (uint32_t)OFY;
    case 58: return (uint32_t)(int32_t)(int16_t)H; /* hardware bug: sign-extended on read */
    case 59: return (uint32_t)(int32_t)DQA;
    case 60: return (uint32_t)DQB;
    case 61: return (uint32_t)(int32_t)ZSF3;
    case 62: return (uint32_t)(int32_t)ZSF4;
    case 63: {
        uint32_t f = FLAG & 0x7FFFF000;
        if (f & 0x7F87E000) f |= 0x80000000;
        return f;
    }
    }
    return 0;
}

static void lo_hi(uint32_t v, int16_t *a, int16_t *b) { *a = (int16_t)v; *b = (int16_t)(v >> 16); }

void gte_write(int r, uint32_t v) {
    static int inited;
    if (!inited) { init_unr(); ws_init(); inited = 1; }
    switch (r) {
    case 0: lo_hi(v, &V[0].x, &V[0].y); break;
    case 1: V[0].z = (int16_t)v; break;
    case 2: lo_hi(v, &V[1].x, &V[1].y); break;
    case 3: V[1].z = (int16_t)v; break;
    case 4: lo_hi(v, &V[2].x, &V[2].y); break;
    case 5: V[2].z = (int16_t)v; break;
    case 6: memcpy(RGBC, &v, 4); break;
    case 7: OTZ = (uint16_t)v; break;
    case 8: case 9: case 10: case 11: IR[r - 8] = (int16_t)v; break;
    case 12: case 13: case 14: lo_hi(v, &SXY[r - 12][0], &SXY[r - 12][1]); SXK[r - 12] = 0; SXZ[r - 12] = -1; SXPK[r - 12] = 0; break;
    case 15:
        SXK[0] = SXK[1]; SXK[1] = SXK[2]; SXK[2] = 0;
        SXZ[0] = SXZ[1]; SXZ[1] = SXZ[2]; SXZ[2] = -1;
        memcpy(SXP[0], SXP[1], sizeof SXP[0]); memcpy(SXP[1], SXP[2], sizeof SXP[1]); SXPK[0] = SXPK[1]; SXPK[1] = SXPK[2]; SXPK[2] = 0;
        memcpy(SXF[0], SXF[1], sizeof SXF[0]); memcpy(SXF[1], SXF[2], sizeof SXF[1]);
        SXY[0][0] = SXY[1][0]; SXY[0][1] = SXY[1][1];
        SXY[1][0] = SXY[2][0]; SXY[1][1] = SXY[2][1];
        lo_hi(v, &SXY[2][0], &SXY[2][1]);
        break;
    case 16: case 17: case 18: case 19: SZ[r - 16] = (uint16_t)v; break;
    case 20: case 21: case 22: memcpy(RGB[r - 20], &v, 4); break;
    case 23: RES1 = v; break;
    case 24: case 25: case 26: case 27: MAC[r - 24] = (int32_t)v; break;
    case 28:
        IR[1] = (int16_t)((v & 0x1F) << 7);
        IR[2] = (int16_t)(((v >> 5) & 0x1F) << 7);
        IR[3] = (int16_t)(((v >> 10) & 0x1F) << 7);
        break;
    case 29: break;
    case 30: {
        LZCS = v;
        uint32_t x = (v & 0x80000000) ? ~v : v;
        int n = 0;
        while (n < 32 && !(x & (0x80000000u >> n))) n++;
        LZCR = n;
        break;
    }
    case 31: break;
    case 32: lo_hi(v, &RT[0][0], &RT[0][1]); break;
    case 33: lo_hi(v, &RT[0][2], &RT[1][0]); break;
    case 34: lo_hi(v, &RT[1][1], &RT[1][2]); break;
    case 35: lo_hi(v, &RT[2][0], &RT[2][1]); break;
    case 36: RT[2][2] = (int16_t)v; break;
    case 37: case 38: case 39: TR[r - 37] = (int32_t)v; break;
    case 40: lo_hi(v, &LM[0][0], &LM[0][1]); break;
    case 41: lo_hi(v, &LM[0][2], &LM[1][0]); break;
    case 42: lo_hi(v, &LM[1][1], &LM[1][2]); break;
    case 43: lo_hi(v, &LM[2][0], &LM[2][1]); break;
    case 44: LM[2][2] = (int16_t)v; break;
    case 45: case 46: case 47: BK[r - 45] = (int32_t)v; break;
    case 48: lo_hi(v, &LCM[0][0], &LCM[0][1]); break;
    case 49: lo_hi(v, &LCM[0][2], &LCM[1][0]); break;
    case 50: lo_hi(v, &LCM[1][1], &LCM[1][2]); break;
    case 51: lo_hi(v, &LCM[2][0], &LCM[2][1]); break;
    case 52: LCM[2][2] = (int16_t)v; break;
    case 53: case 54: case 55: FC[r - 53] = (int32_t)v; break;
    case 56: OFX = (int32_t)v; break;
    case 57: OFY = (int32_t)v; break;
    case 58: H = (uint16_t)v; break;
    case 59: DQA = (int16_t)v; break;
    case 60: DQB = (int32_t)v; break;
    case 61: ZSF3 = (int16_t)v; break;
    case 62: ZSF4 = (int16_t)v; break;
    case 63: FLAG = v & 0x7FFFF000; break;
    }
}

/* widescreen horizontal squeeze (3/4 = 16:9 from 4:3); RUGRATS_WSX=<num>,<den> overrides it (debug: a stronger
   squeeze shows what the game culls against its 4:3 view as empty side bands) */
static int64_t ws_num = 3, ws_den = 4;
int gte_near_sort;   /* 1 = SZ at 1/4 (screen position unchanged), so the game sorts the faces near the front (hooks.c dark_panel) */
/* the same squeeze for guest code that projects on the CPU (portal culling hook) */
void gte_proj_params(double *ox, double *oy, double *h, double *w) {   /* SX = h * w * X / Z + ox, SY = h * Y / Z + oy */
    *ox = OFX / 65536.0; *oy = OFY / 65536.0; *h = H ? H : 1; *w = opt.widescreen ? (double)ws_num / (double)ws_den : 1.0;
}
/* current rotation (rows, 1.0 = 4096) and translation: camera = r * v / 4096 + t (hooks.c occlusion boxes) */
void gte_cam(double r[9], double t[3]) {
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) r[i * 3 + j] = RT[i][j]; t[i] = TR[i]; }
}
/* camera-space point of SXY entry s (0..2) when known (tags / RTPS), for hooks.c near faces */
int gte_sxp(int s, double out[3]) {
    if (s < 0 || s > 2 || !SXPK[s]) return 0;
    for (int k = 0; k < 3; k++) out[k] = SXP[s][k];
    return 1;
}
int32_t gte_ws_x(int32_t x) { return opt.widescreen ? (int32_t)((int64_t)x * ws_num / ws_den) : x; }
/* inverse of the squeeze (4/3), for models that must keep their 4:3 screen position (hooks.c 8003f934) */
int64_t ws_hud_num(void) { return ws_den; }
int64_t ws_hud_den(void) { return ws_num; }
static void ws_init(void) {
    const char *e = getenv("RUGRATS_WSX");
    int n, d;
    if (e && sscanf(e, "%d,%d", &n, &d) == 2 && n > 0 && d > 0) { ws_num = n; ws_den = d; }
}

/* ---- operations ----------------------------------------------------- */
unsigned fp_rtp;   /* RUGRATS_FRAMEPROF: projections this present (host_win32.c) */
static void rtp(int i, int last) {
    fp_rtp++;
    int64_t x = mac_chk(1, mac_chk(1, mac_chk(1, (int64_t)TR[0] * 0x1000 + RT[0][0] * V[i].x) + RT[0][1] * V[i].y) + RT[0][2] * V[i].z);
    int64_t y = mac_chk(2, mac_chk(2, mac_chk(2, (int64_t)TR[1] * 0x1000 + RT[1][0] * V[i].x) + RT[1][1] * V[i].y) + RT[1][2] * V[i].z);
    int64_t z = mac_chk(3, mac_chk(3, mac_chk(3, (int64_t)TR[2] * 0x1000 + RT[2][0] * V[i].x) + RT[2][1] * V[i].y) + RT[2][2] * V[i].z);
    MAC[1] = (int32_t)(x >> (sf * 12));
    MAC[2] = (int32_t)(y >> (sf * 12));
    MAC[3] = (int32_t)(z >> (sf * 12));
    set_ir(1, MAC[1], lm);
    set_ir(2, MAC[2], lm);
    /* IR3 saturation flag uses the unshifted value when sf=0 (hardware quirk) */
    {
        int32_t v = MAC[3];
        int32_t z12 = (int32_t)(z >> 12);
        if (z12 < -0x8000 || z12 > 0x7FFF) FLAG |= 1u << 22;
        IR[3] = (int16_t)(v < (lm ? 0 : -0x8000) ? (lm ? 0 : -0x8000) : v > 0x7FFF ? 0x7FFF : v);
    }
    push_z(z >> 12);
    uint32_t q = gte_divide(H, SZ[3]);
    if (gte_near_sort) SZ[3] /= 4;   /* hooks.c dark_panel: sorted in front of the room (projection above uses the real z) */
    int64_t px = (int64_t)q * IR[1];
    if (opt.widescreen) {
        /* Widescreen: project x from the unclamped camera-space x when IR1 saturates (|x| > 32767). The hardware
           pins such points at x = 32767, which squashes far side objects; in 4:3 they are almost always off-screen,
           but the extra 16:9 strip shows them (mini golf ice cream mountain card squished and warping, 2026-11-07).
           RUGRATS_WSIR=0 = hardware behaviour. IR1 itself keeps the saturated value. */
        static int wsir = -1;
        if (wsir < 0) { const char *e = getenv("RUGRATS_WSIR"); wsir = e ? atoi(e) != 0 : 1; }
        if (wsir && sf && (IR[1] == 0x7FFF || IR[1] == -0x8000)) {
            int64_t fx = x >> 12;
            if (fx > 0x7FFF || fx < -0x8000) px = (int64_t)q * fx;
        }
        px = px * ws_num / ws_den;
    }
    /* SX/SY come from the full-precision sum, not the 32-bit MAC0 register: on hardware a point behind the
       camera (divide overflow, q = 0x1FFFF) overflows MAC0 but still saturates to the correct screen edge.
       Taking SX from the wrapped 32-bit value flipped its sign (+1023 -> -1024), so near-camera wall pieces
       passed the GPU size check and flashed as big slabs (found against PCSX-Redux, 2026-11-06). */
    int64_t mx = px + OFX, my = (int64_t)q * IR[2] + OFY;
    set_mac(0, mx);
    int64_t sx = mx >> 16;
    set_mac(0, my);
    int64_t sy = my >> 16;
    /* sub-pixel position from the unrounded camera-space x/y/z and an exact divide (PGXP-style), recorded
       as the fraction the hardware drops; only for the normal case (sf=1, no saturation, in front) */
    /* "Near only" (subpixel 1): vertices deeper than SZ 16000 keep the hardware's whole-pixel position. Separate
       meshes meeting in the distance can be up to a pixel apart; the integer snap puts them on the same pixel,
       exact positions open a seam (mini golf: water showing under the far wall, 2026-11-07). Wobble is most
       visible up close, which keeps the sub-pixel positions. RUGRATS_SUBPXZ=<SZ> overrides the limit. */
    static int zmax = -1;
    if (zmax < 0) { const char *e = getenv("RUGRATS_SUBPXZ"); zmax = e ? atoi(e) : 16000; }
    int precise = opt.subpixel && sf && z > 0 && (opt.subpixel == 2 || !zmax || SZ[3] < zmax) && (uint32_t)H < (uint32_t)SZ[3] * 2 &&
                  x >= -(0x8000LL << 12) && x < (0x8000LL << 12) && y >= -(0x8000LL << 12) && y < (0x8000LL << 12) &&
                  sx >= -0x400 && sx <= 0x3FF && sy >= -0x400 && sy <= 0x3FF;
    double fxp = 0, fyp = 0;
    if (precise) {
        double k = (double)H / (double)z;
        double pxf = k * (double)x;
        if (opt.widescreen) pxf = pxf * ws_num / ws_den;
        fxp = pxf + OFX / 65536.0 - (double)sx;
        fyp = k * (double)y + OFY / 65536.0 - (double)sy;
        if (fxp < -1.0 || fxp > 2.0 || fyp < -1.0 || fyp > 2.0) precise = 0;
    }
    /* depth for the hi-res depth check (opt.depthfix): room geometry and world objects only (gte_dcls, set by
       hooks around their draw functions), in front of the camera and not pinned at the screen-coordinate limits */
    if (gte_dcls == DCLS_ROOM) gte_world_n++;   /* Stretch 2D: frames with room geometry show the world */
    int dv = opt.depthfix && gte_dcls != DCLS_NONE && sf && z > 0 && (uint32_t)H < (uint32_t)SZ[3] * 2 &&
             sx >= -0x400 && sx <= 0x3FF && sy >= -0x400 && sy <= 0x3FF;
    if (sx < -0x400) { FLAG |= 1u << 14; sx = -0x400; } else if (sx > 0x3FF) { FLAG |= 1u << 14; sx = 0x3FF; }
    if (sy < -0x400) { FLAG |= 1u << 13; sy = -0x400; } else if (sy > 0x3FF) { FLAG |= 1u << 13; sy = 0x3FF; }
    if (dv && gte_dcls == DCLS_OBJ) {   /* model id (object sort fix): one model = one rotation + translation */
        uint32_t h = (uint32_t)TR[0] * 73856093u ^ (uint32_t)TR[1] * 19349663u ^ (uint32_t)TR[2] * 83492791u;
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) h = h * 31u + (uint32_t)(uint16_t)RT[a][b];
        subpx_cur_oid = (1 + (h ^ h >> 16) % 4095u) | gte_oid_bits;
    }
    if (precise || dv)
        subpx_put(((uint32_t)(uint16_t)sy << 16) | (uint16_t)sx, precise, (int)floor(fxp * 256.0 + 0.5),
                  (int)floor(fyp * 256.0 + 0.5), dv ? (float)(4096.0 * 4096.0 / (double)z) : -1.0f, dv ? gte_dcls : 0);
    if (precise) subpx_putg(((uint32_t)(uint16_t)sy << 16) | (uint16_t)sx, (float)(4096.0 * 4096.0 / (double)z));
    if (!dv && opt.depthfix && gte_dcls != DCLS_NONE)   /* world point without depth: not 2D (HUD protection) */
        subpx_world(((uint32_t)(uint16_t)sy << 16) | (uint16_t)sx);
    if (opt.depthfix && gte_dcls == DCLS_ROOM && sf)   /* camera-space position for walls at the lens (gpu.c nearplane) */
        subpx_put3(((uint32_t)(uint16_t)sy << 16) | (uint16_t)sx, x / 4096.0, y / 4096.0, z / 4096.0);
    SXY[0][0] = SXY[1][0]; SXY[0][1] = SXY[1][1];
    SXY[1][0] = SXY[2][0]; SXY[1][1] = SXY[2][1];
    SXY[2][0] = (int16_t)sx; SXY[2][1] = (int16_t)sy;
    SXK[0] = SXK[1]; SXK[1] = SXK[2];
    memcpy(SXF[0], SXF[1], sizeof SXF[0]); memcpy(SXF[1], SXF[2], sizeof SXF[1]);
    SXK[2] = precise ? 1 : 2;
    SXZ[0] = SXZ[1]; SXZ[1] = SXZ[2];
    SXZ[2] = z > 0 ? (float)(4096.0 * 4096.0 / (double)z) : -1.0f;
    memcpy(SXP[0], SXP[1], sizeof SXP[0]); memcpy(SXP[1], SXP[2], sizeof SXP[1]); SXPK[0] = SXPK[1]; SXPK[1] = SXPK[2];
    SXP[2][0] = (float)(x / 4096.0); SXP[2][1] = (float)(y / 4096.0); SXP[2][2] = (float)(z / 4096.0); SXPK[2] = (uint8_t)sf;
    SXF[2][0] = (int16_t)(precise ? (int)floor(fxp * 256.0 + 0.5) : 0);
    SXF[2][1] = (int16_t)(precise ? (int)floor(fyp * 256.0 + 0.5) : 0);
    if (last) {
        int64_t dq = (int64_t)DQA * q + DQB; /* IR0 from the full value too (same as SX/SY) */
        set_mac(0, dq);
        set_ir0(dq >> 12);
    }
}

static void mvmva_mul(int16_t m[3][3], const int16_t *v, const int32_t *t, int tshift) {
    for (int i = 0; i < 3; i++) {
        int64_t acc = t ? (int64_t)t[i] << tshift : 0;
        acc = mac_chk(i + 1, acc + m[i][0] * v[0]);
        acc = mac_chk(i + 1, acc + m[i][1] * v[1]);
        acc = mac_chk(i + 1, acc + m[i][2] * v[2]);
        MAC[i + 1] = (int32_t)(acc >> (sf * 12));
    }
    set_ir(1, MAC[1], lm);
    set_ir(2, MAC[2], lm);
    set_ir(3, MAC[3], lm);
}

static void light(int i) { /* IR = LM * V */
    int16_t v[3] = { V[i].x, V[i].y, V[i].z };
    mvmva_mul(LM, v, 0, 0);
}

static void color_bk(void) { /* IR = BK + LCM * IR */
    int16_t v[3] = { IR[1], IR[2], IR[3] };
    mvmva_mul(LCM, v, BK, 12);
}

/* m = pre-scaled MAC values (before the sf shift). MAC = m + (FC<<12 - m) * IR0, then push colour. */
static void depth_cue(int64_t m1, int64_t m2, int64_t m3) {
    int64_t mm[3] = { m1, m2, m3 };
    int16_t tmp[3];
    for (int i = 0; i < 3; i++) {
        int64_t d = mac_chk(i + 1, ((int64_t)FC[i] << 12) - mm[i]);
        tmp[i] = sat_ir(i + 1, (int32_t)(d >> (sf * 12)), 0);
    }
    for (int i = 0; i < 3; i++) {
        int64_t r = mac_chk(i + 1, (int64_t)tmp[i] * IR[0] + mm[i]);
        MAC[i + 1] = (int32_t)(r >> (sf * 12));
    }
    set_ir(1, MAC[1], lm);
    set_ir(2, MAC[2], lm);
    set_ir(3, MAC[3], lm);
    push_color();
}

static void nc_finish(int dc, int cc) {
    int64_t m1 = ((int64_t)RGBC[0] * IR[1]) << 4, m2 = ((int64_t)RGBC[1] * IR[2]) << 4, m3 = ((int64_t)RGBC[2] * IR[3]) << 4;
    if (dc) { depth_cue(m1, m2, m3); return; }
    if (cc) {
        MAC[1] = (int32_t)(mac_chk(1, m1) >> (sf * 12));
        MAC[2] = (int32_t)(mac_chk(2, m2) >> (sf * 12));
        MAC[3] = (int32_t)(mac_chk(3, m3) >> (sf * 12));
        set_ir(1, MAC[1], lm);
        set_ir(2, MAC[2], lm);
        set_ir(3, MAC[3], lm);
    }
    /* plain NCS/NCT: MAC already holds BK + LCM*IR */
    push_color();
}

static void ncx(int i, int dc, int cc) {
    light(i);
    color_bk();
    nc_finish(dc, cc);
}

/* debug: RUGRATS_GTELOG=<n> logs the first n RTPS/RTPT after a quick load (gte_log_start, from savestate.c), as raw
   register words (inputs VXY/VZ 0-2, TR; outputs SXY0-2, SZ1-3, FLAG), to diff against a reference emulator */
static int gte_log_left;
void gte_log_start(void) { const char *e = getenv("RUGRATS_GTELOG"); gte_log_left = e ? atoi(e) : 0; }
static void gte_log_op(const char *name, uint32_t in[9]) {
    if (gte_log_left <= 0) return;
    gte_log_left--;
    rt_logf("gtelog %s in %08x %04x %08x %04x %08x %04x tr %08x %08x %08x out %08x %08x %08x %04x %04x %04x flag %08x\n",
            name, in[0], in[1] & 0xFFFF, in[2], in[3] & 0xFFFF, in[4], in[5] & 0xFFFF, in[6], in[7], in[8],
            gte_read(12), gte_read(13), gte_read(14), gte_read(17) & 0xFFFF, gte_read(18) & 0xFFFF, gte_read(19) & 0xFFFF,
            gte_read(63));
}

void gte_op(uint32_t op) {
    uint32_t log_in[9];
    if (gte_log_left > 0 && ((op & 0x3F) == 0x01 || (op & 0x3F) == 0x30)) {
        for (int i = 0; i < 6; i++) log_in[i] = gte_read(i);
        log_in[6] = gte_read(32 + 5); log_in[7] = gte_read(32 + 6); log_in[8] = gte_read(32 + 7);
    }
    sf = (op >> 19) & 1;
    lm = (op >> 10) & 1;
    FLAG = 0;
    cpu.cycles += 8;
    switch (op & 0x3F) {
    case 0x01: rtp(0, 1); gte_log_op("rtps", log_in);
        {
            extern int gpu_cmdlog;
            if (gpu_cmdlog)
                rt_logf("gte rtps ra %08x v (%d,%d,%d) -> sxy (%d,%d) sz %d flag %08x\n",
                        cpu.r[31], V[0].x, V[0].y, V[0].z, SXY[2][0], SXY[2][1], SZ[3], gte_read(63));
        }
        break;
    case 0x30: rtp(0, 0); rtp(1, 0); rtp(2, 1); gte_log_op("rtpt", log_in);
        {   /* debug: with RUGRATS_GP0LOG, log every RTPT of that vblank (inputs, outputs, caller) */
            extern int gpu_cmdlog;
            if (gpu_cmdlog)
                rt_logf("gte rtpt ra %08x v (%d,%d,%d) (%d,%d,%d) (%d,%d,%d) tr %d,%d,%d -> sxy (%d,%d) (%d,%d) (%d,%d) sz %d %d %d flag %08x\n",
                        cpu.r[31], V[0].x, V[0].y, V[0].z, V[1].x, V[1].y, V[1].z, V[2].x, V[2].y, V[2].z, TR[0], TR[1], TR[2],
                        SXY[0][0], SXY[0][1], SXY[1][0], SXY[1][1], SXY[2][0], SXY[2][1], SZ[1], SZ[2], SZ[3], gte_read(63));
        }
        break;
    case 0x06: { /* NCLIP */
        int64_t v = (int64_t)SXY[0][0] * SXY[1][1] + (int64_t)SXY[1][0] * SXY[2][1] + (int64_t)SXY[2][0] * SXY[0][1]
                  - (int64_t)SXY[0][0] * SXY[2][1] - (int64_t)SXY[1][0] * SXY[0][1] - (int64_t)SXY[2][0] * SXY[1][1];
        /* Sub-pixel face culling (2026-11-13): the game culls faces by the sign of this whole-pixel area. A face
           seen nearly edge-on (lamp base side faces, back corners ~0.6 px above the front ones) rounds its corners
           onto one row as the camera turns -> area 0 -> culled, then back: at 9x a large wedge blinks. When all
           three entries have exact positions and the exact area's sign differs, return that sign (+-1). Normal
           faces keep the hardware value bit-for-bit. RUGRATS_NCLIPFIX=0 = hardware behaviour. */
        static int ncfix = -1;
        if (ncfix < 0) { const char *e = getenv("RUGRATS_NCLIPFIX"); ncfix = e ? atoi(e) != 0 : 1; }
        if (ncfix && opt.subpixel && SXK[0] == 1 && SXK[1] == 1 && SXK[2] == 1) {
            double ex[3], ey[3];
            for (int k = 0; k < 3; k++) { ex[k] = SXY[k][0] + SXF[k][0] / 256.0; ey[k] = SXY[k][1] + SXF[k][1] / 256.0; }
            const double p = (ex[1] - ex[0]) * (ey[2] - ey[0]) - (ex[2] - ex[0]) * (ey[1] - ey[0]);
            const int ps = p > 1.0 / 64 ? 1 : p < -1.0 / 64 ? -1 : 0, vs = v > 0 ? 1 : v < 0 ? -1 : 0;
            if (ps && ps != vs) v = ps;
        }
        /* Hedge backs (2026-11-14, user preferred it): while the stick camera looks over the walls (hooks.c sets
           gte_backface_frame during this frame's room traversal), room faces seen from behind are drawn too (the
           far side of hedges seen from above). Rooms only; objects, HUD and the game's own camera untouched.
           RUGRATS_BACKFACE=0 = off. */
        if (v < 0 && gte_dcls == DCLS_ROOM && gte_backface_frame == present_count && hooks_pkt_room()) { v = -v; gpu_backface_note(SXY); }
        /* Tree backs (2026-11-14): props (not characters) in the gap-table levels, see hooks.c dcls_run. */
        if (v < 0 && gte_dcls == DCLS_OBJ && gte_obj_backface && hooks_pkt_room()) { v = -v; gpu_backface_note(SXY); }
        /* Packet ring nearly full this frame (hooks.c hooks_pkt_full, Reptar 2010 crash): drop further room / object
           faces rather than let the frame overrun the game's packet ring (corrupt list, crash). */
        if (v > 0 && (gte_dcls == DCLS_ROOM || gte_dcls == DCLS_OBJ) && hooks_pkt_full()) v = 0;
        set_mac(0, v);
        {   /* debug: with RUGRATS_GP0LOG, log every NCLIP of that vblank (face culling: MAC0 <= 0 = dropped) */
            extern int gpu_cmdlog;
            if (gpu_cmdlog)
                rt_logf("gte nclip c%d bf%d ra %08x (%d,%d) (%d,%d) (%d,%d) sz %d %d %d -> %lld\n", gte_dcls, gte_obj_backface, cpu.r[31], SXY[0][0], SXY[0][1],
                        SXY[1][0], SXY[1][1], SXY[2][0], SXY[2][1], SZ[1], SZ[2], SZ[3], (long long)v);
        }
        break;
    }
    case 0x0C: { /* OP */
        int64_t a = (int64_t)RT[1][1] * IR[3] - (int64_t)RT[2][2] * IR[2];
        int64_t b = (int64_t)RT[2][2] * IR[1] - (int64_t)RT[0][0] * IR[3];
        int64_t c = (int64_t)RT[0][0] * IR[2] - (int64_t)RT[1][1] * IR[1];
        set_mac(1, a); set_mac(2, b); set_mac(3, c);
        set_ir(1, MAC[1], lm); set_ir(2, MAC[2], lm); set_ir(3, MAC[3], lm);
        break;
    }
    case 0x10: depth_cue((int64_t)RGBC[0] << 16, (int64_t)RGBC[1] << 16, (int64_t)RGBC[2] << 16); break; /* DPCS */
    case 0x2A: for (int k = 0; k < 3; k++)                                                /* DPCT */
            depth_cue((int64_t)RGB[0][0] << 16, (int64_t)RGB[0][1] << 16, (int64_t)RGB[0][2] << 16);
        break;
    case 0x11: depth_cue((int64_t)IR[1] << 12, (int64_t)IR[2] << 12, (int64_t)IR[3] << 12); break; /* INTPL */
    case 0x29: nc_finish(1, 0); break;                                                    /* DCPL */
    case 0x12: { /* MVMVA */
        int mx = (op >> 17) & 3, vx = (op >> 15) & 3, tx = (op >> 13) & 3;
        int16_t (*m)[3] = mx == 0 ? RT : mx == 1 ? LM : mx == 2 ? LCM : 0;
        int16_t garbage[3][3] = { { (int16_t)(-(RGBC[0] << 4)), (int16_t)(RGBC[0] << 4), IR[0] },
                                  { RT[0][2], RT[0][2], RT[0][2] }, { RT[1][1], RT[1][1], RT[1][1] } };
        if (!m) m = garbage;
        int16_t v[3];
        if (vx < 3) { v[0] = V[vx].x; v[1] = V[vx].y; v[2] = V[vx].z; }
        else { v[0] = IR[1]; v[1] = IR[2]; v[2] = IR[3]; }
        const int32_t *t = tx == 0 ? TR : tx == 1 ? BK : tx == 2 ? FC : 0;
        mvmva_mul(m, v, t, 12);
        break;
    }
    case 0x13: ncx(0, 1, 0); break;                              /* NCDS */
    case 0x16: for (int k = 0; k < 3; k++) ncx(k, 1, 0); break;  /* NCDT */
    case 0x1B: ncx(0, 0, 1); break;                              /* NCCS */
    case 0x3F: for (int k = 0; k < 3; k++) ncx(k, 0, 1); break;  /* NCCT */
    case 0x1E: ncx(0, 0, 0); break;                              /* NCS */
    case 0x20: for (int k = 0; k < 3; k++) ncx(k, 0, 0); break;  /* NCT */
    case 0x14: color_bk(); nc_finish(1, 0); break;               /* CDP */
    case 0x1C: color_bk(); nc_finish(0, 1); break;               /* CC */
    case 0x28: /* SQR */
        set_mac(1, (int64_t)IR[1] * IR[1]); set_mac(2, (int64_t)IR[2] * IR[2]); set_mac(3, (int64_t)IR[3] * IR[3]);
        set_ir(1, MAC[1], lm); set_ir(2, MAC[2], lm); set_ir(3, MAC[3], lm);
        break;
    case 0x2D: { /* AVSZ3 */
        int64_t v = (int64_t)ZSF3 * (SZ[1] + SZ[2] + SZ[3]);
        set_mac(0, v);
        OTZ = sat_z(v >> 12);
        break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t v = (int64_t)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]);
        set_mac(0, v);
        OTZ = sat_z(v >> 12);
        break;
    }
    case 0x3D: /* GPF */
        set_mac(1, (int64_t)IR[0] * IR[1]); set_mac(2, (int64_t)IR[0] * IR[2]); set_mac(3, (int64_t)IR[0] * IR[3]);
        set_ir(1, MAC[1], lm); set_ir(2, MAC[2], lm); set_ir(3, MAC[3], lm);
        push_color();
        break;
    case 0x3E: /* GPL */
        set_mac(1, ((int64_t)MAC[1] << (sf * 12)) + (int64_t)IR[0] * IR[1]);
        set_mac(2, ((int64_t)MAC[2] << (sf * 12)) + (int64_t)IR[0] * IR[2]);
        set_mac(3, ((int64_t)MAC[3] << (sf * 12)) + (int64_t)IR[0] * IR[3]);
        set_ir(1, MAC[1], lm); set_ir(2, MAC[2], lm); set_ir(3, MAC[3], lm);
        push_color();
        break;
    default:
        rt_logf("GTE: unknown op %07x\n", op);
        break;
    }
}

/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
void gte_state(StateIO *s) {
    st_prefix(s, "gte");
    ST(s, V); ST(s, RGBC); ST(s, OTZ); ST(s, IR); ST(s, SXY); ST(s, SZ); ST(s, RGB); ST(s, RES1); ST(s, MAC);
    ST(s, LZCS); ST(s, LZCR); ST(s, RT); ST(s, LM); ST(s, LCM); ST(s, TR); ST(s, BK); ST(s, FC); ST(s, OFX); ST(s, OFY);
    ST(s, H); ST(s, DQA); ST(s, DQB); ST(s, ZSF3); ST(s, ZSF4); ST(s, FLAG); ST(s, sf); ST(s, lm);
}

/* swc2 of a screen position (SXY0-2, SXYP): store it and tag that RAM word with its sub-pixel fraction */
void gte_swc2_sxy(uint32_t addr, int r) {
    const uint32_t v = gte_read(r);
    mem_w32(addr, v);
    const int s = r == 15 ? 2 : r - 12;
    if (SXK[s] && opt.subpixel) { subpx_tag_z(SXZ[s]); subpx_tag_p3(SXPK[s] ? SXP[s] : NULL); subpx_tag(addr, v, SXK[s] == 1, SXF[s][0], SXF[s][1]); }
}

uint32_t tag_ld[32];   /* lw source address, or 0xFFFFFF00 | rt: holds a GTE screen position (mfc2), see gpu.c */
void subpx_tag_reg(int rt, uint32_t val, int has, int fx, int fy);
void gte_mfc2_sxy(int rt, int r) {
    const int s = r == 15 ? 2 : r - 12;
    if (!SXK[s] || !opt.subpixel) { tag_ld[rt] = 0; return; }
    subpx_tag_z(SXZ[s]); subpx_tag_p3(SXPK[s] ? SXP[s] : NULL);
    subpx_tag_reg(rt, gte_read(r), SXK[s] == 1, SXF[s][0], SXF[s][1]);
    tag_ld[rt] = 0xFFFFFF00u | (uint32_t)rt;
    tag_lpc[rt] = 0;
}
int subpx_tag_get(uint32_t addr, uint32_t val, int *fx, int *fy);
/* lwc2 of a screen position: the GTE entry gets the stored point's sub-pixel fraction back when the word is tagged */
void gte_lwc2_sxy(int r, uint32_t addr) {
    const uint32_t v = mem_r32(addr);
    gte_write(r, v);
    int fx = 0, fy = 0;
    const int t = opt.subpixel ? subpx_tag_get(addr, v, &fx, &fy) : -1;
    const int s = r == 15 ? 2 : r - 12;
    if (t >= 0) {
        SXK[s] = t ? 1 : 2; SXF[s][0] = (int16_t)fx; SXF[s][1] = (int16_t)fy; SXZ[s] = subpx_tag_zout();
        const float *p3 = subpx_tag_p3out();
        SXPK[s] = p3 != NULL; if (p3) memcpy(SXP[s], p3, sizeof SXP[s]);
    } else SXPK[s] = 0;
}
/* mtc2 of a screen position (2026-11-13): the object/room mesh code (800102dc) lw's projected vertices from its
   buffer and mtc2's them into SXY0-2 for NCLIP. Same as lwc2: when the register came from a tagged word (tag_ld,
   value checked), the entry gets its sub-pixel fraction back, so the NCLIP fix above can see edge-on faces.
   RUGRATS_MTC2TAG=0 = old (entry unknown). */
void gte_mtc2_sxy(int r, uint32_t v, uint32_t src) {
    gte_write(r, v);
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_MTC2TAG"); on = e ? atoi(e) != 0 : 1; }
    if (!on || !opt.subpixel || src >= 0xFFFFFF00u || !tag_src(src)) return;
    int fx = 0, fy = 0;
    const int t = subpx_tag_get(src, v, &fx, &fy);
    if (t < 0) return;
    const int s = r == 15 ? 2 : r - 12;
    SXK[s] = t ? 1 : 2; SXF[s][0] = (int16_t)fx; SXF[s][1] = (int16_t)fy; SXZ[s] = subpx_tag_zout();
    const float *p3 = subpx_tag_p3out();
    SXPK[s] = p3 != NULL; if (p3) memcpy(SXP[s], p3, sizeof SXP[s]);
}
