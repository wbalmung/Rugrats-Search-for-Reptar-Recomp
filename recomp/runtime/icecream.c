/* Ice Cream Mountain (db 8), the cut level (2026-11-24, user).

   The game's level table still lists db 8 "Ice Cream Mountain" and DATA/DB08 holds a whole level, but its files are
   from an older version of the tools, so the released loader (8002a208) reads garbage. Nothing of the game ships
   here: when db 8 is requested we read the four old files from the player's own disc, convert them in memory to
   the released formats and serve them as extra sectors after the end of the disc (cdrom.c asks icm_sector first).

   Old -> released formats (worked out from the loaders; see HANDOFF "Ice Cream Mountain"):
   - LEVEL08.BIN: old = u32 0, u32 nrooms, nrooms x 1000-byte room headers {s32 origin/min/max (16 bytes each),
     u32 portal count, portals x 48}, nrooms x u32 sizes, room mesh blobs. Released = u32 0 (section A), u32 n +
     portals x 48 (corner 3 w = 66, bytes 40..42 = 05 05 7b), u32 0 (section B), u32 n + rooms x 112 (first 52
     bytes as old, +88 = 00 01 00 00), one count byte per room (0), padding, sizes, blobs, u32 n + 12-byte texture
     entries {uv0, clut, uv1, tpage, uv2, uv3}. Mesh blobs keep their header and prelit vertices; polygon groups
     (97 quads, 96 triangles) shrink from 16 bytes (indices + inline texture) to 8 (indices + texture index).
   - Textures: old sheet = 448x256 at VRAM (512,0), released levels load 576x256 at (0,256) (rect 0x800C3F64).
     The sheet moves by (-512,+256): tpage x 8..14 at y 0 -> x 0..6 at y 256, clut x-512 / y+256. Pages 24/25
     (old y 256) were never on the disc (52% of the polygons): they get a 64x64 brick tile from page 9.
   - LEVEL08.COL: old = 160 columns, 6-byte cells {floor, ceil, floor slope, 0, shade, last}, one slope table;
     released = 512 columns, 8-byte cells {floor, floor slope, ceil, ceil slope, 0, shade, flags (1 last, 4 no
     ceiling), room}, floor slopes + ceiling slopes (one: 0,4096,0).
   - DB08LP.BIN: old 40-byte records {old object id, xyz, rotation}; released 96-byte {type, id, xyz, rotation}.
     Only the start (old id 79) is kept, as Tommy (type 1, id 11); the other 24 objects (old ids 71/64/114)
     have no known released equivalent.
   Models / animations come from the Training Level (db 16, as for Be Bad); DB08MOD is the old model format.
   The per-db jump table 0x80012820 gives db 8 the multiplayer camera; we point it at the single-player case.
   LBAs are the USA disc's (the installer checks its SHA1). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "hw.h"

int cd_disc_read(uint32_t lba, uint8_t *out);

#define ICM_LBA 440000u   /* first virtual sector; mods.c keeps its own files below this */
#define FILE_TAB 0x800B2C40u

typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 65536;
        while (c < b->n + n) c *= 2;
        b->d = realloc(b->d, c); b->cap = c;
    }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void put32(Buf *b, uint32_t v) { put(b, &v, 4); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

typedef struct { uint32_t id, lba; uint8_t *d; uint32_t n, nsect; uint8_t sub[4]; } VFile;
enum { V_LEVEL, V_COL, V_RAW, V_LP, V_N };
static VFile vf[V_N] = { { 442 }, { 474 }, { 507 }, { 410 } };   /* file ids: bases 434/466/499/402 + db 8 */
/* the old files on the USA disc: LEVEL08.BIN, LEVEL08.COL, DB08A.RAW, DB08LP.BIN */
static const uint32_t old_lba[V_N] = { 119391, 119545, 119184, 119312 }, old_size[V_N] = { 314092, 16854, 229376, 1040 };
static int state;   /* 0 not built, 1 ready, -1 failed */

static uint8_t *disc_file(uint32_t lba, uint32_t size, uint8_t sub[4]) {
    uint8_t *b = malloc(size), sec[2352];
    if (!b) return NULL;
    for (uint32_t s = 0; s * 2048 < size; s++) {
        if (!cd_disc_read(lba + s, sec)) { free(b); return NULL; }
        if (s == 0) memcpy(sub, sec + 16, 4);
        uint32_t n = size - s * 2048 < 2048 ? size - s * 2048 : 2048;
        memcpy(b + s * 2048, sec + 24, n);
    }
    return b;
}

/* ---- texture entries ---- */
static Buf tex;
static uint32_t tex_n;
static int32_t *tex_hash;   /* 8192 slots, -1 = free */
static uint16_t sub_clut;
static uint32_t missing;
static uint32_t tex_index(const uint8_t e[12]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 12; i++) h = (h ^ e[i]) * 16777619u;
    for (uint32_t k = h & 8191;; k = (k + 1) & 8191) {
        if (tex_hash[k] < 0) { tex_hash[k] = (int32_t)tex_n; put(&tex, e, 12); return tex_n++; }
        if (!memcmp(tex.d + 12 * tex_hash[k], e, 12)) return (uint32_t)tex_hash[k];
    }
}
static int flat_missing;   /* showcase models: a missing page becomes one flat texel instead of the brick tile */
static uint16_t clut_new(uint16_t c) { return (uint16_t)((((c >> 6) + 256) << 6) | ((((c & 63) * 16) - 512) >> 4)); }
/* old texture info (uv0, clut, uv1, tpage, uv2, uv3; uv3 = 0,0 for triangles) -> released entry */
static uint32_t conv_tex(const uint8_t *o, int quad) {
    uint8_t uv[4][2] = { { o[0], o[1] }, { o[4], o[5] }, { o[8], o[9] }, { 0, 0 } };
    if (quad) { uv[3][0] = o[10]; uv[3][1] = o[11]; }
    uint16_t clut = rd16(o + 2), tp = rd16(o + 6), x = tp & 15, nt;
    int clut_ok = (clut >> 6) < 256 && (clut & 63) * 16 >= 512;   /* 0xFFFF = no palette (a few model faces) */
    if (!(tp & 0x10) && x >= 8 && x <= 14 && clut_ok) nt = (uint16_t)((tp & ~0x1F) | 0x10 | (x - 8));
    else {   /* page not on the disc: squeeze the UV rect into the 64x64 brick tile (page 9, u/v 0..63) */
        int n = quad ? 4 : 3, u0 = 255, u1 = 0, v0 = 255, v1 = 0;
        for (int i = 0; i < n; i++) {
            if (uv[i][0] < u0) u0 = uv[i][0];
            if (uv[i][0] > u1) u1 = uv[i][0];
            if (uv[i][1] < v0) v0 = uv[i][1];
            if (uv[i][1] > v1) v1 = uv[i][1];
        }
        for (int i = 0; i < n; i++) {
            uv[i][0] = (uint8_t)(u1 == u0 ? 0 : ((uv[i][0] - u0) * 63 + (u1 - u0) / 2) / (u1 - u0));
            uv[i][1] = (uint8_t)(v1 == v0 ? 0 : ((uv[i][1] - v0) * 63 + (v1 - v0) / 2) / (v1 - v0));
        }
        nt = (uint16_t)((tp & ~0x1F) | 0x10 | 1); clut = sub_clut; missing++;
        if (flat_missing) for (int i = 0; i < 4; i++) { uv[i][0] = 20; uv[i][1] = 4; }
    }
    uint8_t e[12];
    uint16_t c2 = clut_new(clut);
    e[0] = uv[0][0]; e[1] = uv[0][1]; memcpy(e + 2, &c2, 2); e[4] = uv[1][0]; e[5] = uv[1][1];
    memcpy(e + 6, &nt, 2); e[8] = uv[2][0]; e[9] = uv[2][1]; e[10] = uv[3][0]; e[11] = uv[3][1];
    return tex_index(e);
}

/* ---- showcase (2026-11-24, user: "one copy of each model in one of the big empty rooms") -----------------------
   DB08MOD / DB08HIE hold the level's own models in a pre-release format (n-Space test content: 4 jointed characters
   and 15 single models). The engine can't run those objects, so one copy of each is baked into room SHOW_ROOM's
   geometry as extra level sub-meshes (rest pose, static, no collision; textures from the converted sheet).
   DB08MOD: 768 x u32 offsets (0 = none), records from +3072: u32 0, s16 min xyz + 65, s16 max xyz + 64, u32 radius,
   u16 nverts x2, u32 colour offset, u32 face offset; 6-byte vertices from +36, every ODD vertex stored (z,x,y); face
   groups {u16 type, u16 count}, ff ends: 0x3c tex quad 20 B, 0x34 tex tri 20, 0x2c / 0x24 same + 4-byte colour (24),
   0x28 flat quad 12, 0x30 flat tri 8 (flat faces are left out: level polygons always take a texture).
   DB08HIE: 256 x u32 offsets (skeletons 0, 1, 6, 8), 44-byte header, 40-byte parts {u16, u16 depth (preorder),
   16 bytes, s32 xyz offset from the parent, u32 model slot}. Models face +z; they are turned to face -z (north door). */
#define SHOW_ROOM 38
#define SHOW_MAX 96
static Buf show[SHOW_MAX];
static int nshow;
typedef struct { int16_t x, y, z; float nx, ny, nz; } SVert;
static SVert sv[256];
static int snv;
static Buf sq, stri;              /* 8-byte polygon records of the sub-mesh being filled */
static uint32_t sqn, strn;
static int32_t show_org[3];       /* room origin: vertices are room-local */
static void show_flush(void) {
    if (!snv) return;
    if (nshow < SHOW_MAX) {
        Buf *b = &show[nshow++];
        int16_t mn[4] = { 32767, 32767, 32767, 0x80 }, mx[4] = { -32768, -32768, -32768, 0 };
        for (int i = 0; i < snv; i++) {
            int16_t v[3] = { sv[i].x, sv[i].y, sv[i].z };
            for (int k = 0; k < 3; k++) { if (v[k] < mn[k]) mn[k] = v[k]; if (v[k] > mx[k]) mx[k] = v[k]; }
        }
        put32(b, 0); put(b, mn, 8); put(b, mx, 8); put32(b, (uint32_t)snv); put32(b, 28u + 12u * (uint32_t)snv);
        const float L[3] = { 0.35f, -0.80f, -0.48f };   /* light from above, front-left (y is down) */
        for (int i = 0; i < snv; i++) {
            float n2 = sv[i].nx * sv[i].nx + sv[i].ny * sv[i].ny + sv[i].nz * sv[i].nz, d = 0;
            if (n2 > 0) {
                float inv = 1.0f / sqrtf(n2);
                d = (sv[i].nx * L[0] + sv[i].ny * L[1] + sv[i].nz * L[2]) * inv;
                if (d < 0) d = -d * 0.35f;   /* faces turned away still get some fill */
            }
            uint8_t c = (uint8_t)(58 + 70 * d), v[12];
            memcpy(v, &sv[i].x, 2); memcpy(v + 2, &sv[i].y, 2); memcpy(v + 4, &sv[i].z, 2);
            v[6] = v[7] = 0; v[8] = v[9] = v[10] = c; v[11] = 0;
            put(b, v, 12);
        }
        if (sqn) { uint16_t h[2] = { 97, (uint16_t)sqn }; put(b, h, 4); put(b, sq.d, sq.n); }
        if (strn) { uint16_t h[2] = { 96, (uint16_t)strn }; put(b, h, 4); put(b, stri.d, stri.n); }
        put32(b, 0x000100FFu);
    }
    snv = 0; sq.n = stri.n = 0; sqn = strn = 0;
}
static const uint8_t *smod;
static uint32_t smod_n;
static const uint8_t *show_rec(uint32_t slot, uint32_t *nv) {
    uint32_t o = rd32(smod + 4 * slot), p = 3072 + o;
    if ((!o && slot) || p + 36 > smod_n) return NULL;
    *nv = rd16(smod + p + 24);
    return smod + p;
}
static void show_vert(const uint8_t *r, uint32_t i, int32_t v[3]) {
    const uint8_t *q = r + 36 + 6 * i;
    int16_t a = (int16_t)rd16(q), b = (int16_t)rd16(q + 2), c = (int16_t)rd16(q + 4);
    if (i & 1) { v[0] = b; v[1] = c; v[2] = a; } else { v[0] = a; v[1] = b; v[2] = c; }
}
/* one model at world position pos (model space + pos, turned 180 degrees about y). Sub-meshes stay within what the
   released levels use (<= 111 vertices, <= 129 polygons; bigger ones blacked out the whole room): a model is split
   face by face, each sub-mesh getting the vertices its faces use. */
#define SHOW_NV 110
#define SHOW_NP 128
static int16_t remap[256];
static void show_model(uint32_t slot, const int32_t pos[3]) {
    uint32_t nv;
    const uint8_t *r = show_rec(slot, &nv);
    if (!r || !nv || nv > 256) return;
    int32_t mv[256][3];
    for (uint32_t i = 0; i < nv; i++) {
        int32_t v[3];
        show_vert(r, i, v);
        mv[i][0] = pos[0] - v[0] - show_org[0]; mv[i][1] = pos[1] + v[1] - show_org[1]; mv[i][2] = pos[2] - v[2] - show_org[2];
    }
    for (uint32_t i = 0; i < 256; i++) remap[i] = -1;
    static const uint8_t rs[64] = { [0x3c] = 20, [0x34] = 20, [0x2c] = 24, [0x24] = 24, [0x28] = 12, [0x30] = 8 };
    uint32_t q = rd32(r + 32);
    for (int g = 0; g < 64; g++) {
        uint16_t t = rd16(r + q), c = rd16(r + q + 2);
        if (t == 0xFF || t >= 64 || !rs[t]) break;
        for (uint32_t j = 0; j < c; j++) {
            const uint8_t *e = r + q + 4 + rs[t] * j;
            int quad = t == 0x3c || t == 0x2c, n = quad ? 4 : 3;
            if (t == 0x28 || t == 0x30) continue;
            if (e[4] >= nv || e[5] >= nv || e[6] >= nv || (quad && e[7] >= nv)) continue;
            int need = 0;
            for (int k = 0; k < n; k++) need += remap[e[4 + k]] < 0;
            if (snv + need > SHOW_NV || sqn + strn + 1 > SHOW_NP) {
                show_flush();
                for (uint32_t i = 0; i < 256; i++) remap[i] = -1;
            }
            uint8_t rec[8] = { 0 };
            for (int k = 0; k < n; k++) {
                int vi = e[4 + k];
                if (remap[vi] < 0) {
                    SVert *s = &sv[snv];
                    s->x = (int16_t)mv[vi][0]; s->y = (int16_t)mv[vi][1]; s->z = (int16_t)mv[vi][2];
                    s->nx = s->ny = s->nz = 0;
                    remap[vi] = (int16_t)snv++;
                }
                rec[k] = (uint8_t)remap[vi];
            }
            const uint8_t *tx = e + (t == 0x2c || t == 0x24 ? 12 : 8);
            uint32_t ti = conv_tex(tx, quad);
            memcpy(rec + 4, &ti, 4);
            if (quad) { put(&sq, rec, 8); sqn++; } else { put(&stri, rec, 8); strn++; }
            /* face normal into its vertices (for the baked light) */
            SVert *A = &sv[rec[0]], *B = &sv[rec[1]], *C = &sv[rec[2]];
            float ux = B->x - A->x, uy = B->y - A->y, uz = B->z - A->z, wx = C->x - A->x, wy = C->y - A->y, wz = C->z - A->z;
            float fx = uy * wz - uz * wy, fy = uz * wx - ux * wz, fz = ux * wy - uy * wx;
            for (int k = 0; k < n; k++) { SVert *s = &sv[rec[k]]; s->nx += fx; s->ny += fy; s->nz += fz; }
        }
        q += 4 + rs[t] * c;
    }
}
/* lowest point (largest y) of a model, in model space */
static int32_t show_bottom(uint32_t slot) {
    uint32_t nv; int32_t m = -100000;
    const uint8_t *r = show_rec(slot, &nv);
    if (!r) return 0;
    for (uint32_t i = 0; i < nv; i++) { int32_t v[3]; show_vert(r, i, v); if (v[1] > m) m = v[1]; }
    return m;
}
static void show_xext(uint32_t slot, int32_t *lo, int32_t *hi) {
    uint32_t nv;
    const uint8_t *r = show_rec(slot, &nv);
    *lo = *hi = 0;
    if (!r) return;
    for (uint32_t i = 0; i < nv; i++) { int32_t v[3]; show_vert(r, i, v); if (-v[0] < *lo) *lo = -v[0]; if (-v[0] > *hi) *hi = -v[0]; }
}
static void build_showcase(const uint8_t *mod, uint32_t modn, const uint8_t *hie, uint32_t hien, const uint8_t *room_hdr) {
    smod = mod; smod_n = modn; nshow = 0; flat_missing = 1;
    for (int k = 0; k < 3; k++) show_org[k] = (int32_t)rd32(room_hdr + 4 * k);
    const int32_t floor_y = (int32_t)rd32(room_hdr + 36) + 1;   /* room max y = its floor */
    uint8_t used[768] = { 0 };
    /* back row: the jointed characters, origin on the floor (the brain alien keeps its hover height) */
    static const int32_t char_x[4] = { -29300, -27100, -24900, -22700 };
    int nc = 0;
    for (uint32_t sk = 0; sk < 256 && nc < 4; sk++) {
        uint32_t o = rd32(hie + 4 * sk);
        if (!o || o + 44 > hien) continue;
        uint32_t parts = rd16(hie + o), hdr = 44;
        if (o + hdr + 40 * parts > hien || parts > 32) continue;
        int32_t stack[8][3], pos[3];
        for (uint32_t j = 0; j < parts; j++) {
            const uint8_t *pr = hie + o + hdr + 40 * j;
            uint32_t depth = rd16(pr + 2), slot = rd32(pr + 36);
            if (depth < 1 || depth > 8 || slot >= 768) break;
            for (int k = 0; k < 3; k++) {
                int32_t off = (int32_t)rd32(pr + 20 + 4 * k);
                stack[depth - 1][k] = (depth > 1 ? stack[depth - 2][k] : 0) + off;
            }
            /* turned 180: x and z of the part offset flip with the model (show_model negates model x/z) */
            pos[0] = char_x[nc] - stack[depth - 1][0]; pos[1] = floor_y + stack[depth - 1][1];
            pos[2] = -4300 - stack[depth - 1][2];
            show_model(slot, pos);
            used[slot] = 1;
        }
        show_flush();
        nc++;
    }
    /* front row: every model no skeleton uses (0 = the exporter's axes helper: left out), set on the floor */
    uint32_t list[64], nl = 0;
    int32_t width = 0;
    for (uint32_t s = 1; s < 768 && nl < 64; s++) {
        uint32_t o = rd32(mod + 4 * s), dup = 0;
        if (!o || used[s]) continue;
        for (uint32_t k = 1; k < s; k++) if (rd32(mod + 4 * k) == o) dup = 1;   /* same record listed twice */
        if (dup) continue;
        int32_t lo, hi; show_xext(s, &lo, &hi);
        width += hi - lo + 250;
        list[nl++] = s;
    }
    int32_t x = -26100 - width / 2;
    for (uint32_t k = 0; k < nl; k++) {
        int32_t lo, hi; show_xext(list[k], &lo, &hi);
        int32_t pos[3] = { x - lo + 125, floor_y - show_bottom(list[k]), -7000 };
        show_model(list[k], pos);
        x += hi - lo + 250;
    }
    show_flush();
    free(sq.d); free(stri.d); sq.d = stri.d = NULL; sq.cap = stri.cap = 0;
    flat_missing = 0;
    rt_logf("icecream: showcase in room %d: %d characters, %u single models, %d sub-meshes\n", SHOW_ROOM, nc, nl, nshow);
}

#define NEED(c) do { if (!(c)) { rt_logf("icecream: unexpected data (%s, line %d)\n", #c, __LINE__); goto fail; } } while (0)

static int build(void) {
    uint8_t *src[V_N] = { 0 }, *mod = NULL, *hie = NULL;
    uint32_t *cnt = NULL;
    Buf lv = { 0 }, cl = { 0 }, lp = { 0 };
    memset(&tex, 0, sizeof tex); tex_n = 0; missing = 0;
    tex_hash = malloc(8192 * sizeof *tex_hash);
    for (int i = 0; i < V_N; i++) {
        src[i] = disc_file(old_lba[i], old_size[i], vf[i].sub);
        NEED(src[i]);
    }
    {   uint8_t sub[4];   /* DB08MOD.BIN, DB08HIE.BIN: the level's own models, for the showcase */
        mod = disc_file(119313, 69064, sub); hie = disc_file(119310, 3760, sub);
        NEED(mod && hie);
    }
    NEED(tex_hash);
    memset(tex_hash, 0xFF, 8192 * sizeof *tex_hash);
    const uint8_t *L = src[V_LEVEL];
    const uint32_t LN = old_size[V_LEVEL];
    uint32_t n = rd32(L + 4);
    NEED(rd32(L) == 0 && n == 44 && 8 + n * 1004 <= LN);

    /* blob offsets */
    uint32_t boff[64], bsz[64], o = 8 + n * 1000 + 4 * n;
    for (uint32_t i = 0; i < n; i++) { bsz[i] = rd32(L + 8 + n * 1000 + 4 * i); boff[i] = o; o += bsz[i]; }
    NEED(o == LN);

    /* stand-in clut: the one most used by page-9 quads inside the brick tile */
    cnt = calloc(65536, sizeof *cnt);
    NEED(cnt);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *b = L + boff[i];
        uint32_t ns = rd32(b), p = 4 + 4 * ns;
        for (uint32_t k = 0; k < ns; k++) {
            const uint8_t *s = b + p;
            uint32_t ss = rd32(b + 4 + 4 * k), q = rd32(s + 24);
            p += ss;
            NEED(p <= bsz[i] && q < ss);
            for (;;) {
                uint16_t t = rd16(s + q), c = rd16(s + q + 2);
                if (t == 0xFF) break;
                NEED((t == 97 || t == 96) && q + 4 + 16u * c <= ss);
                for (uint32_t j = 0; j < c; j++) {
                    const uint8_t *e = s + q + 4 + 16 * j;
                    if (t == 97 && (rd16(e + 10) & 0x1F) == 9 && e[4] < 64 && e[8] < 64 && e[12] < 64 && e[14] < 64 &&
                        e[5] < 64 && e[9] < 64 && e[13] < 64 && e[15] < 64) cnt[rd16(e + 6)]++;
                }
                q += 4 + 16 * c;
            }
        }
    }
    for (uint32_t c = 1; c < 65536; c++) if (cnt[c] > cnt[sub_clut]) sub_clut = (uint16_t)c;

    build_showcase(mod, 69064, hie, 3760, L + 8 + SHOW_ROOM * 1000);

    /* ---- level ---- */
    put32(&lv, 0);
    uint32_t np = 0;
    for (uint32_t i = 0; i < n; i++) np += rd32(L + 8 + i * 1000 + 48);
    NEED(np < 1000);
    put32(&lv, np);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *h = L + 8 + i * 1000;
        uint32_t k = rd32(h + 48);
        NEED(k <= 16);
        for (uint32_t j = 0; j < k; j++) {
            uint8_t pr[48];
            memcpy(pr, h + 52 + 48 * j, 48);
            int16_t w = 66; memcpy(pr + 30, &w, 2);
            memset(pr + 32, 0, 16); pr[40] = 0x05; pr[41] = 0x05; pr[42] = 0x7b;
            put(&lv, pr, 48);
        }
    }
    put32(&lv, 0);
    put32(&lv, n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t r[112] = { 0 };
        memcpy(r, L + 8 + i * 1000, 52);
        r[89] = 1;
        put(&lv, r, 112);
    }
    { uint8_t z[64] = { 0 }; put(&lv, z, n); put(&lv, z, 4 - (n % 4)); }
    size_t sizes_at = lv.n;
    for (uint32_t i = 0; i < n; i++) put32(&lv, 0);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *b = L + boff[i];
        uint32_t ns = rd32(b), p = 4 + 4 * ns;
        size_t start = lv.n, tab = lv.n + 4;
        uint32_t extra = i == SHOW_ROOM ? (uint32_t)nshow : 0;
        put32(&lv, ns + extra);
        for (uint32_t k = 0; k < ns + extra; k++) put32(&lv, 0);
        for (uint32_t k = 0; k < ns; k++) {
            const uint8_t *s = b + p;
            uint32_t ss = rd32(b + 4 + 4 * k), q = rd32(s + 24);
            p += ss;
            size_t sstart = lv.n;
            uint8_t hdr[28];
            memcpy(hdr, s, 28);
            hdr[10] = 0x80; hdr[11] = 0; hdr[18] = 0; hdr[19] = 0;
            put(&lv, hdr, 28);
            put(&lv, s + 28, q - 28);
            for (;;) {
                uint16_t t = rd16(s + q), c = rd16(s + q + 2);
                if (t == 0xFF) { put32(&lv, 0x000100FFu); break; }
                put(&lv, s + q, 4);
                for (uint32_t j = 0; j < c; j++) {
                    const uint8_t *e = s + q + 4 + 16 * j;
                    uint8_t rec[8];
                    memcpy(rec, e, 4);
                    if (t == 96) rec[3] = 0;
                    uint32_t ti = conv_tex(e + 4, t == 97);
                    memcpy(rec + 4, &ti, 4);
                    put(&lv, rec, 8);
                }
                q += 4 + 16 * c;
            }
            uint32_t sl = (uint32_t)(lv.n - sstart);
            memcpy(lv.d + tab + 4 * k, &sl, 4);
        }
        for (uint32_t k = 0; k < extra; k++) {
            uint32_t sl = (uint32_t)show[k].n;
            memcpy(lv.d + tab + 4 * (ns + k), &sl, 4);
            put(&lv, show[k].d, show[k].n);
        }
        uint32_t bl = (uint32_t)(lv.n - start);
        memcpy(lv.d + sizes_at + 4 * i, &bl, 4);
    }
    put32(&lv, tex_n);
    put(&lv, tex.d, tex.n);

    /* ---- texture sheet ---- */
    uint8_t *sheet = calloc(576 * 256, 2);
    NEED(sheet);
    for (int y = 0; y < 256; y++) memcpy(sheet + y * 576 * 2, src[V_RAW] + y * 448 * 2, 448 * 2);

    /* ---- collision ---- */
    const uint8_t *C = src[V_COL];
    uint32_t cn = rd32(C + 32), coff = rd32(C + 36), cp = 40 + 160 * 8;
    int32_t minx = (int32_t)rd32(C), minz = (int32_t)rd32(C + 8), maxy = (int32_t)rd32(C + 20);
    NEED(cn < 64 && coff + 6 * cn == old_size[V_COL]);
    put(&cl, C, 32);
    put32(&cl, cn); put32(&cl, 1); put32(&cl, 0);   /* offset patched below */
    size_t heads = cl.n;
    { uint8_t z[8] = { 0 }; for (int i = 0; i < 512; i++) put(&cl, z, 8); }
    for (int i = 0; i < 160; i++) {
        const uint8_t *ch = C + 40 + 8 * i;
        int16_t a = (int16_t)rd16(ch + 4), bn = (int16_t)rd16(ch + 6);
        NEED(a >= 0 && bn >= 0 && cp + 2 * a + 6 * bn <= coff);
        memcpy(cl.d + heads + 8 * i + 4, ch + 4, 4);
        const uint8_t *idx = C + cp;
        put(&cl, idx, 2 * a);
        for (int j = 0; j < bn; j++) {
            const uint8_t *c = C + cp + 2 * a + 6 * j;
            int zc = 0;
            for (int k = 0; k < a; k++) if (idx[2 * k + 1] <= j) zc = idx[2 * k];
            int32_t x = minx + i * 1024 + 512, z = minz + zc * 1024 + 512, fy = maxy - c[0] * 128;
            int room = 0xFF;
            for (uint32_t r = 0; r < n; r++) {
                const uint8_t *h = L + 8 + r * 1000;
                int32_t lx = (int32_t)rd32(h + 16), ly = (int32_t)rd32(h + 20), lz = (int32_t)rd32(h + 24);
                int32_t hx = (int32_t)rd32(h + 32), hy = (int32_t)rd32(h + 36), hz = (int32_t)rd32(h + 40);
                if (x < lx || x > hx || z < lz || z > hz) continue;
                if (ly - 64 <= fy - 1 && fy - 1 <= hy + 64) { room = (int)r; break; }
                if (room == 0xFF) room = (int)r;
            }
            uint8_t cell[8] = { c[0], (uint8_t)(c[2] < cn ? c[2] : 0), c[1], 0, 0, c[4],
                                (uint8_t)((c[5] & 1) | (c[1] == 255 ? 4 : 0)), (uint8_t)room };
            put(&cl, cell, 8);
        }
        cp += 2 * a + 6 * bn;
    }
    NEED(cp == coff);
    { uint32_t so = (uint32_t)cl.n; memcpy(cl.d + 40, &so, 4); }
    put(&cl, C + coff, 6 * cn);
    { int16_t up[3] = { 0, 4096, 0 }; put(&cl, up, 6); }

    /* ---- objects: header + Tommy at the old start ---- */
    {
        const uint8_t *P = src[V_LP];
        int32_t rec[24] = { 99, 2, 0 };
        put32(&lp, 0);
        put(&lp, rec, 96);
        uint32_t cntr = 1;
        for (uint32_t i = 1; i < old_size[V_LP] / 40; i++) {
            int32_t w[10];
            memcpy(w, P + 40 * i, 40);
            if (w[0] != 79) continue;
            memset(rec, 0, sizeof rec);
            rec[0] = 1; rec[1] = 11; rec[2] = w[1]; rec[3] = w[2]; rec[4] = w[3];
            for (int k = 0; k < 5; k++) rec[5 + k] = w[4 + k];
            put(&lp, rec, 96); cntr++;
        }
        NEED(cntr == 2);
        memcpy(lp.d, &cntr, 4);
    }

    vf[V_LEVEL].d = lv.d; vf[V_LEVEL].n = (uint32_t)lv.n;
    vf[V_COL].d = cl.d; vf[V_COL].n = (uint32_t)cl.n;
    vf[V_RAW].d = sheet; vf[V_RAW].n = 576 * 256 * 2;
    vf[V_LP].d = lp.d; vf[V_LP].n = (uint32_t)lp.n;
    uint32_t at = ICM_LBA;
    for (int i = 0; i < V_N; i++) {
        vf[i].lba = at;
        vf[i].nsect = (vf[i].n + 2047) / 2048;
        at += (vf[i].nsect + 15) & ~15u;
    }
    {   /* debug: RUGRATS_ICMDUMP=<dir> writes the converted files there (private checks; never share them) */
        const char *e = getenv("RUGRATS_ICMDUMP");
        static const char *nm[V_N] = { "LEVEL08.BIN", "LEVEL08.COL", "DB08A.RAW", "DB08LP.BIN" };
        for (int i = 0; e && i < V_N; i++) {
            char path[600];
            snprintf(path, sizeof path, "%s/%s", e, nm[i]);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(vf[i].d, 1, vf[i].n, f); fclose(f); }
        }
    }
    rt_logf("icecream: converted from the disc: level %u bytes (%u rooms, %u portals, %u texture entries, %u polygons "
            "on the stand-in tile), collision %u, objects %u, textures %u\n", vf[V_LEVEL].n, n, np, tex_n, missing,
            vf[V_COL].n, vf[V_LP].n, vf[V_RAW].n);
    for (int i = 0; i < V_N; i++) free(src[i]);
    for (int k = 0; k < nshow; k++) free(show[k].d);
    free(mod); free(hie); free(cnt); free(tex.d); free(tex_hash);
    return 1;
fail:
    for (int i = 0; i < V_N; i++) free(src[i]);
    for (int k = 0; k < nshow; k++) free(show[k].d);
    free(mod); free(hie);
    free(cnt); free(tex.d); free(tex_hash); free(lv.d); free(cl.d); free(lp.d);
    return 0;
}

/* called before the level loads (level request and level loop start): file slots, models, camera */
int icm_restore(void) {
    if (state == 0) state = build() ? 1 : -1;
    if (state < 0) { rt_logf("icecream: can't convert Ice Cream Mountain from this disc\n"); return 0; }
    for (int i = 0; i < V_N; i++) {
        mem_w32(FILE_TAB + vf[i].id * 8, vf[i].lba);
        mem_w32(FILE_TAB + vf[i].id * 8 + 4, vf[i].n);
    }
    static const uint16_t base[] = { 242, 274, 306, 338, 370 };   /* MOD ANM FRM ROT HIE */
    for (unsigned i = 0; i < sizeof base / sizeof base[0]; i++) {
        uint32_t s = FILE_TAB + (base[i] + 16u) * 8, d = FILE_TAB + (base[i] + 8u) * 8;
        mem_w32(d, mem_r32(s)); mem_w32(d + 4, mem_r32(s + 4));
    }
    mem_w32(0x80012820u + 8 * 4, 0x80017CB0u);
    return 1;
}

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
/* cdrom.c asks here first: 1 = this sector is one of ours */
int icm_sector(uint32_t lba, uint8_t *out) {
    if (lba < ICM_LBA || lba >= ICM_LBA + 1024) return 0;
    if (state == 0) state = build() ? 1 : -1;   /* a quick load straight into db 8 */
    if (state < 0) return 0;
    for (int i = 0; i < V_N; i++) {
        const VFile *f = &vf[i];
        if (lba < f->lba || lba >= f->lba + f->nsect) continue;
        uint32_t k = lba - f->lba, a = lba + 150;
        memset(out, 0, 2352);
        out[0] = 0; memset(out + 1, 0xFF, 10);
        out[12] = (uint8_t)bcd((int)(a / 4500)); out[13] = (uint8_t)bcd((int)(a / 75 % 60)); out[14] = (uint8_t)bcd((int)(a % 75));
        out[15] = 2;
        memcpy(out + 16, f->sub, 4);
        out[18] = (uint8_t)((out[18] & ~0x81) | (k + 1 == f->nsect ? 0x81 : 0));
        memcpy(out + 20, out + 16, 4);
        uint32_t off = k * 2048, m = f->n - off < 2048 ? f->n - off : 2048;
        memcpy(out + 24, f->d + off, m);
        return 1;
    }
    return 0;
}
