/* Hub World Enhancements (2026-11-26, user): the family room and the garage in the house (db 1).

   In the hub the hall door (room 28, south wall) and the kitchen door (room 22, east wall) are closed: teal door
   faces in the room geometry plus a door knob object (model 527) each, and nothing behind them. Chuckie's Glasses
   (db 10) is the same house in the same coordinates with both doors open: the family room (its room 15, behind a
   1024-deep doorway room 44) and the garage (room 18, doorway room 33). Room boxes, portals and collision cells of
   the rooms both levels share are identical; only the room numbers differ (db 10 32 = hub 28, 28 = hub 15, ...).

   With the option on, when the hub loads we serve it merged files, built once from the player's own disc:
   - LEVEL01.BIN: the hub's 44 rooms; rooms 22 / 28 take db 10's geometry (open doorways) and portals; new rooms
     44 = family room, 45 = its doorway, 46 = garage, 47 = its doorway (db 10's records, portals renumbered).
   - Textures: room polygons index the level's texture table (12-byte entries: uv0, clut, uv1, tpage, uv2, uv3) that
     point into the level sheet (DBxxA.RAW, 576 x 256 at VRAM 0,256). The two sheets are laid out differently, so
     every donor picture is looked up in the hub sheet by its texel indices (same spot first, else anywhere in a
     page) with the same 16 colours; 6 garage palettes the hub lacks go into all-zero palette slots of the hub
     sheet (x 512..575, y 292..469: nothing the hub draws uses them). New entries are appended to the hub's table.
   - LEVEL01.COL: same grid origin x, z origin 5 cells apart, heights from a different base (+48 x 128 units):
     db 10's cells of the four new rooms are added (room byte renumbered; a column's cells sorted lowest floor
     first, bit 0 of byte 6 = last). Header: 44 bytes {min xyz, max xyz, n floor/ceil slopes, slope offset},
     512 column heads {0, s16 z entries, s16 cells}, per column {u8 z, u8 first cell} + 8-byte cells, slopes.
   - DB01LP.BIN (96-byte spawn records): the two door knobs are dropped, db 10's type 1 (static model) objects
     inside the two new rooms are added (17: 14 in the garage - shelves, paint cans, boxes, bags, rake - and 3 in
     the family room). Record indices are not referenced by type 1 objects, so removing and appending is safe.
   - DB01MOD.BIN: an object's model is looked up in a global id -> model bank id table the game builds in RAM
     (56-byte records, +0 = bank id); banks are binary-searched by id, so entries stay sorted. Most of the models already are in the hub's bank with the same pictures
     (checked: same faces, same decoded texels); bank 464 (a family room piece, id 415) differs and 482 / 483
     (garage pieces, ids 176 / 177) are missing: db 10's records, textures moved as above.
   - DB01A.RAW: the hub sheet plus the new palettes.
   Memory: the hub is the fullest level (17 KB of its 1.09 MB heap free, 1160 bytes of its 64388-byte object
   pool). The pool size is a per-db table in RAM (0x800B7F20 + db * 4, read by 80046340): raised by 12 KB for the
   hub. The heap (blocks with a word header: size in words, negative = used, 0 = end; start gp+0xba8, end gp+0xba4)
   gets a 512 KB extension in the port's RAM beyond the PS1's 2 MB: the end marker becomes a used block that bridges
   to HUB_EXT, followed by one free block and a new end marker (hub_heap_ext). Save states keep that region
   (hubrooms_state). Everything is applied when the game reads one of the hub's files through its file table
   (readers 80019794 / 80019a04 / 80019da4 / 8001a058, a0 = file id) and when it sets up a level's object pool
   (80046340, a0 = db); option off = the table's own entries and pool size again.
   RUGRATS_HUBWORLD=0/1 overrides the option; RUGRATS_HUBDUMP=<dir> writes the merged files (private checks only,
   never share them). LBAs and file ids are the USA disc's (the installer checks its SHA1). */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "hw.h"
#include "savestate.h"

int cd_disc_read(uint32_t lba, uint8_t *out);
extern const unsigned char game_exe[];

#define HUB_LBA   442000u          /* first virtual sector (icecream.c uses 440000..441023) */
#define HUB_NSECT 2048u
#define FILE_TAB  0x800B2C40u
#define POOL_TAB  0x800B7F20u      /* per-db object memory size, u32 */
#define POOL_MORE 12288u
#define HUB_EXT   0x80700000u      /* heap extension (packet ring 0x80200000.., leaf / arch at 0x807FF000..) */
#define HUB_EXT_SIZE 0x80000u
#define GP        0x800C3E80u
#define W         576               /* level sheet width in halfwords */

enum { F_LEVEL, F_COL, F_LP, F_MOD, F_RAW, F_N };
static const uint16_t hub_id[F_N] = { 435, 467, 403, 243, 500 };   /* LEVEL01.BIN .COL DB01LP DB01MOD DB01A.RAW */
static const uint16_t don_id[F_N] = { 444, 476, 412, 252, 509 };   /* the same for db 10 */
static const char *const fname[F_N] = { "LEVEL01.BIN", "LEVEL01.COL", "DB01LP.BIN", "DB01MOD.BIN", "DB01A.RAW" };

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
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

typedef struct { uint32_t lba, n, nsect; uint8_t *d; uint8_t sub[4]; } VFile;
static VFile vf[F_N];
static int state;                  /* 0 not built, 1 ready, -1 failed */
static int lg = -1;

/* the disc's own table entry (the program image, untouched by mods and by us) */
static void exe_entry(uint32_t id, uint32_t *lba, uint32_t *size) {
    const unsigned char *p = game_exe + 0x800 + (FILE_TAB - 0x80010000u) + id * 8;
    *lba = rd32(p); *size = rd32(p + 4);
}
static uint8_t *disc_file(uint32_t id, uint32_t *size, uint8_t sub[4]) {
    uint32_t lba;
    exe_entry(id, &lba, size);
    if (!lba || !*size || *size > 4u << 20) return NULL;
    uint8_t *b = malloc(*size), sec[2352];
    if (!b) return NULL;
    for (uint32_t s = 0; s * 2048 < *size; s++) {
        if (!cd_disc_read(lba + s, sec)) { free(b); return NULL; }
        if (s == 0 && sub) memcpy(sub, sec + 16, 4);
        uint32_t k = *size - s * 2048 < 2048 ? *size - s * 2048 : 2048;
        memcpy(b + s * 2048, sec + 24, k);
    }
    return b;
}

/* ---- level files -------------------------------------------------------------------------------------------- */
#define MAXR 64
typedef struct {
    const uint8_t *d; uint32_t n;
    uint32_t np, nr, nt;
    const uint8_t *portals, *rooms, *tex;
    const uint8_t *blob[MAXR]; uint32_t blen[MAXR], pfirst[MAXR];
} Lvl;
static int lvl_parse(Lvl *L, const uint8_t *d, uint32_t n) {
    memset(L, 0, sizeof *L);
    L->d = d; L->n = n;
    uint32_t o = 0;
#define NEED(c) do { if (!(c)) return 0; } while (0)
    NEED(o + 4 <= n); o += 4 + 56 * rd32(d + o);
    NEED(o + 4 <= n); L->np = rd32(d + o); o += 4; L->portals = d + o; o += 48 * L->np;
    NEED(o + 4 <= n); o += 4 + 24 * rd32(d + o);
    NEED(o + 4 <= n); L->nr = rd32(d + o); o += 4; NEED(L->nr >= 1 && L->nr <= MAXR);
    L->rooms = d + o; o += 112 * L->nr;
    NEED(o + L->nr <= n);
    uint32_t extra = 0;
    for (uint32_t i = 0; i < L->nr; i++) extra += d[o + i];
    o += L->nr + extra;
    o += 4 - (o % 4);
    NEED(o + 4 * L->nr <= n);
    uint32_t b = o + 4 * L->nr, pc = 0;
    for (uint32_t i = 0; i < L->nr; i++) {
        L->blen[i] = rd32(d + o + 4 * i);
        L->blob[i] = d + b; b += L->blen[i];
        NEED(b <= n);
        L->pfirst[i] = pc; pc += rd32(L->rooms + 112 * i + 48);
    }
    NEED(pc == L->np && b + 4 <= n);
    L->nt = rd32(d + b); L->tex = d + b + 4;
    NEED(b + 4 + 12 * L->nt == n);
    return 1;
#undef NEED
}

/* ---- textures ------------------------------------------------------------------------------------------------ */
static uint16_t *HA, *HA0;          /* hub sheet (gets the new palettes), and as on the disc */
static const uint16_t *HB;          /* donor sheet */
static uint8_t *NA, *NB;            /* 4-bit texel indices, 256 rows x 2304 */
#define NW (W * 4)
static int nfree, newpal, tex_moved, tex_same;
static uint16_t freeslot[64][2];
static uint16_t newrow[16][16], newclut[16];
static int nnew;
static struct { uint8_t key[13]; uint8_t out[12]; int ok; } tcache[512];
static int ntcache;

static int clut_find(const uint16_t row[16], uint16_t *clut) {
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < W; x += 16)
            if (!memcmp(HA0 + y * W + x, row, 32)) { *clut = (uint16_t)(((y + 256) << 6) | (x >> 4)); return 1; }
    for (int i = 0; i < nnew; i++) if (!memcmp(newrow[i], row, 32)) { *clut = newclut[i]; return 1; }
    if (nnew >= 16 || newpal >= nfree) return 0;
    const int x = freeslot[newpal][0], y = freeslot[newpal][1];
    newpal++;
    memcpy(HA + y * W + x, row, 32);
    memcpy(newrow[nnew], row, 32);
    newclut[nnew] = *clut = (uint16_t)(((y + 256) << 6) | (x >> 4));
    nnew++;
    return 1;
}
/* db 10 texture info (12 bytes) -> the same picture in the hub sheet; 0 = not found */
static int tex_remap(const uint8_t e[12], int quad, uint8_t out[12]) {
    uint8_t key[13];
    memcpy(key, e, 12); key[12] = (uint8_t)quad;
    for (int i = 0; i < ntcache; i++) if (!memcmp(tcache[i].key, key, 13)) { memcpy(out, tcache[i].out, 12); return tcache[i].ok; }
    int ok = 0;
    const uint16_t clut = rd16(e + 2), tp = rd16(e + 6);
    const int n = quad ? 4 : 3;
    static const int uo[4] = { 0, 4, 8, 10 };
    int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
    for (int k = 0; k < n; k++) {
        const int u = e[uo[k]], v = e[uo[k] + 1];
        if (u < u0) u0 = u;
        if (u > u1) u1 = u;
        if (v < v0) v0 = v;
        if (v > v1) v1 = v;
    }
    if (((tp >> 7) & 3) == 0 && (tp & 0x10) && (clut >> 6) >= 256 && (clut & 63) * 16 + 16 <= W) {
        const int bx = (tp & 15) * 256, w = u1 - u0 + 1, h = v1 - v0 + 1;
        int fx = -1, fy = -1;
        if (bx + u1 < NW) {
            int same = 1;
            for (int v = v0; v <= v1 && same; v++) same = !memcmp(NA + v * NW + bx + u0, NB + v * NW + bx + u0, (size_t)w);
            if (same) { fx = bx + u0; fy = v0; }
            for (int Y = 0; fx < 0 && Y + h <= 256; Y++)
                for (int X = 0; X + w <= NW; X++) {
                    if ((X % 256) + w > 256 || NA[Y * NW + X] != NB[v0 * NW + bx + u0]) continue;
                    int k = 0;
                    while (k < h && !memcmp(NA + (Y + k) * NW + X, NB + (v0 + k) * NW + bx + u0, (size_t)w)) k++;
                    if (k == h) { fx = X; fy = Y; break; }
                }
        }
        uint16_t row[16], nc;
        memcpy(row, HB + ((clut >> 6) - 256) * W + (clut & 63) * 16, 32);
        if (fx >= 0 && clut_find(row, &nc)) {
            memcpy(out, e, 12);
            const int du = fx % 256 - u0, dv = fy - v0;
            for (int k = 0; k < n; k++) { out[uo[k]] = (uint8_t)(e[uo[k]] + du); out[uo[k] + 1] = (uint8_t)(e[uo[k] + 1] + dv); }
            wr16(out + 2, nc);
            wr16(out + 6, (uint16_t)((tp & ~0x1F) | 0x10 | (fx / 256)));
            if (fx == bx + u0 && fy == v0) tex_same++; else tex_moved++;
            ok = 1;
        }
    }
    if (ntcache < 512) { memcpy(tcache[ntcache].key, key, 13); memcpy(tcache[ntcache].out, out, 12); tcache[ntcache++].ok = ok; }
    if (!ok && lg) rt_logf("hubworld: no place for texture %02x%02x clut %04x tpage %04x\n", e[0], e[1], clut, tp);
    return ok;
}

/* hub texture table + appended entries */
static Buf tex;
static uint32_t ntex;
static uint32_t tex_add(const uint8_t e[12]) {
    for (uint32_t i = 0; i < ntex; i++) if (!memcmp(tex.d + 12 * i, e, 12)) return i;
    put(&tex, e, 12);
    return ntex++;
}
/* a db 10 room blob with its texture indices pointing into the hub's table */
static int conv_blob(const Lvl *D, uint32_t r, Buf *out) {
    const uint8_t *b = D->blob[r];
    const uint32_t len = D->blen[r];
    size_t at = out->n;
    put(out, b, len);
    uint8_t *o = out->d + at;
    if (len < 4) return 0;
    const uint32_t ns = rd32(b);
    uint32_t p = 4 + 4 * ns;
    for (uint32_t k = 0; k < ns; k++) {
        if (4 + 4 * k + 4 > len) return 0;
        const uint32_t ss = rd32(b + 4 + 4 * k);
        if (p + 28 > len) return 0;
        uint32_t q = p + rd32(b + p + 24);
        for (;;) {
            if (q + 4 > len) return 0;
            const uint16_t t = rd16(b + q), c = rd16(b + q + 2);
            if (t == 0xFF) break;
            if ((t != 96 && t != 97) || q + 4 + 8u * c > len) return 0;
            for (uint32_t j = 0; j < c; j++) {
                const uint32_t ti = rd32(b + q + 8 + 8 * j);
                uint8_t ne[12];
                if (ti >= D->nt || !tex_remap(D->tex + 12 * ti, t == 97, ne)) return 0;
                wr32(o + q + 8 + 8 * j, tex_add(ne));
            }
            q += 4 + 8u * c;
        }
        p += ss;
    }
    return 1;
}

/* ---- models: face groups {u16 type, u16 count}; textured faces (0x24 / 0x2c / 0x34 / 0x3c, 24 bytes) hold their
   texture info at +0xc in the same 12-byte layout as the level's entries; flat ones (12 bytes) have none ---- */
static int conv_model(uint8_t *m, uint32_t len) {
    uint32_t a = rd32(m + 0x1c);
    for (int g = 0; g < 64 && a + 4 <= len; g++) {
        const uint32_t hd = rd32(m + a), t = hd & 0xffff, c = hd >> 16;
        a += 4;
        if (t == 0xff || !c) return 1;
        const uint32_t st = (t == 0x34 || t == 0x24 || t == 0x3c || t == 0x2c) ? 0x18 :
                            (t == 0x30 || t == 0x20 || t == 0x28 || t == 0x38) ? 0xc : 0;
        if (!st || a + c * st > len) return 0;
        for (uint32_t k = 0; k < c; k++, a += st) {
            if (st != 0x18) continue;
            uint8_t ne[12];
            if (!tex_remap(m + a + 0xc, t == 0x3c || t == 0x2c, ne)) return 0;
            memcpy(m + a + 0xc, ne, 12);
        }
    }
    return 1;
}
/* bank: +0 c0de0001, +4 n, n x (id << 21 | offset), records from 8 + 4n; a record ends at the next offset */
static int bank_rec(const uint8_t *b, uint32_t n, uint32_t id, uint32_t *off, uint32_t *len) {
    if (n < 8 || rd32(b) != 0xc0de0001u) return 0;
    const uint32_t cnt = rd32(b + 4), base = 8 + 4 * cnt;
    if (base > n) return 0;
    uint32_t o = 0xffffffffu, next = n - base;
    for (uint32_t i = 0; i < cnt; i++) if ((rd32(b + 8 + 4 * i) >> 21) == id) o = rd32(b + 8 + 4 * i) & 0x1fffff;
    if (o == 0xffffffffu) return 0;
    for (uint32_t i = 0; i < cnt; i++) { const uint32_t x = rd32(b + 8 + 4 * i) & 0x1fffff; if (x > o && x < next) next = x; }
    *off = base + o; *len = next - o;
    return *off + *len <= n;
}

static int build(void) {
    uint8_t *h[F_N] = { 0 }, *d[F_N] = { 0 };
    uint32_t hn[F_N], dn[F_N];
    Buf lv = { 0 }, cl = { 0 }, lp = { 0 }, md = { 0 };
    uint8_t *raw = NULL;
    int ok = 0;
    Lvl H, D;
    memset(tcache, 0, sizeof tcache); ntcache = 0; nnew = 0; newpal = 0; nfree = 0; tex_moved = tex_same = 0;
    tex.n = 0; ntex = 0;
    for (int i = 0; i < F_N; i++) {
        h[i] = disc_file(hub_id[i], &hn[i], vf[i].sub);
        d[i] = disc_file(don_id[i], &dn[i], NULL);
        if (!h[i] || !d[i]) { rt_logf("hubworld: can't read %s or its db 10 version\n", fname[i]); goto out; }
    }
#define NEED(c) do { if (!(c)) { rt_logf("hubworld: unexpected data (%s:%d)\n", __FILE__, __LINE__); goto out; } } while (0)
    NEED(hn[F_RAW] == W * 256 * 2 && dn[F_RAW] == W * 256 * 2);
    NEED(lvl_parse(&H, h[F_LEVEL], hn[F_LEVEL]) && lvl_parse(&D, d[F_LEVEL], dn[F_LEVEL]));
    NEED(H.nr == 44 && D.nr > 44);

    /* sheets */
    raw = malloc(hn[F_RAW]); HA0 = malloc(hn[F_RAW]); NA = malloc(256 * NW); NB = malloc(256 * NW);
    NEED(raw && HA0 && NA && NB);
    memcpy(raw, h[F_RAW], hn[F_RAW]); memcpy(HA0, h[F_RAW], hn[F_RAW]);
    HA = (uint16_t *)raw; HB = (const uint16_t *)d[F_RAW];
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < NW; x++) {
            NA[y * NW + x] = (uint8_t)((HA0[y * W + x / 4] >> (4 * (x % 4))) & 15);
            NB[y * NW + x] = (uint8_t)((HB[y * W + x / 4] >> (4 * (x % 4))) & 15);
        }
    for (int y = 292 - 256; y < 470 - 256; y++)
        for (int x = 512; x < W; x += 16) {
            int z = 1;
            for (int k = 0; k < 16; k++) z &= HA0[y * W + x + k] == 0;
            if (z && nfree < 64) { freeslot[nfree][0] = (uint16_t)x; freeslot[nfree][1] = (uint16_t)y; nfree++; }
        }

    /* ---- level ---- */
    int map[MAXR];                                   /* db 10 room -> hub room */
    for (int i = 0; i < MAXR; i++) map[i] = -1;
    static const int newr[4] = { 15, 44, 18, 33 };   /* -> hub 44 family room, 45 doorway, 46 garage, 47 doorway */
    map[15] = 44; map[44] = 45; map[18] = 46; map[33] = 47; map[22] = 22; map[32] = 28;
    for (uint32_t i = 0; i < D.nr; i++)
        for (uint32_t j = 0; j < H.nr && map[i] < 0; j++)
            if (!memcmp(D.rooms + 112 * i + 16, H.rooms + 112 * j + 16, 32)) map[i] = (int)j;
    /* the two shared rooms we take from db 10 must be the hub's kitchen / hall */
    NEED(!memcmp(D.rooms + 112 * 22 + 16, H.rooms + 112 * 22 + 16, 32) && !memcmp(D.rooms + 112 * 32 + 16, H.rooms + 112 * 28 + 16, 32));
    put(&tex, H.tex, 12 * H.nt); ntex = H.nt;
    uint32_t src[48]; const Lvl *from[48];           /* each hub room's record / portals / blob comes from */
    for (uint32_t i = 0; i < 44; i++) { src[i] = i; from[i] = &H; }
    src[22] = 22; from[22] = &D; src[28] = 32; from[28] = &D;
    for (int k = 0; k < 4; k++) { src[44 + k] = (uint32_t)newr[k]; from[44 + k] = &D; }
    Buf ports = { 0 }, blobs = { 0 };
    uint32_t pcount[48], blen[48], np = 0;
    for (int i = 0; i < 48; i++) {
        const Lvl *L = from[i];
        const uint32_t r = src[i], n = rd32(L->rooms + 112 * r + 48);
        for (uint32_t j = 0; j < n; j++) {
            uint8_t p[48];
            memcpy(p, L->portals + 48 * (L->pfirst[r] + j), 48);
            if (L == &D) {
                const int16_t t = (int16_t)rd16(p + 14);
                if (t < 0 || t >= MAXR || map[t] < 0) { free(ports.d); free(blobs.d); NEED(0); }
                wr16(p + 14, (uint16_t)map[t]);
            }
            put(&ports, p, 48);
        }
        pcount[i] = n; np += n;
        const size_t at = blobs.n;
        if (L == &H) put(&blobs, H.blob[r], H.blen[r]);
        else if (!conv_blob(&D, r, &blobs)) { free(ports.d); free(blobs.d); rt_logf("hubworld: room %u textures\n", r); goto out; }
        blen[i] = (uint32_t)(blobs.n - at);
    }
    put32(&lv, 0);
    put32(&lv, np); put(&lv, ports.d, ports.n);
    put32(&lv, 0);
    put32(&lv, 48);
    for (int i = 0; i < 48; i++) {
        uint8_t r[112];
        memcpy(r, i < 44 ? H.rooms + 112 * i : D.rooms + 112 * src[i], 112);   /* 22 / 28 keep the hub's record */
        wr32(r + 48, pcount[i]);
        put(&lv, r, 112);
    }
    { uint8_t z[52] = { 0 }; put(&lv, z, 48); put(&lv, z, 4 - (lv.n % 4)); }
    for (int i = 0; i < 48; i++) put32(&lv, blen[i]);
    put(&lv, blobs.d, blobs.n);
    put32(&lv, ntex); put(&lv, tex.d, tex.n);
    free(ports.d); free(blobs.d);

    /* ---- collision ---- */
    {
        const uint8_t *CH = h[F_COL], *CD = d[F_COL];
        NEED(hn[F_COL] > 44 + 4096 && dn[F_COL] > 44 + 4096 && rd32(CH) == rd32(CD));
        const int dz = ((int32_t)rd32(CD + 8) - (int32_t)rd32(CH + 8)) / 1024;
        const int dy = ((int32_t)rd32(CH + 20) - (int32_t)rd32(CD + 20)) / 128;
        NEED(dz >= 0 && dz < 64 && dy >= 0 && dy < 128);
        Buf heads = { 0 }, data = { 0 };
        uint32_t ph = 44 + 4096, pd = 44 + 4096, added = 0;
        for (int x = 0; x < 512; x++) {
            const int16_t ha = (int16_t)rd16(CH + 44 + 8 * x + 4), hb = (int16_t)rd16(CH + 44 + 8 * x + 6);
            const int16_t da = (int16_t)rd16(CD + 44 + 8 * x + 4), db = (int16_t)rd16(CD + 44 + 8 * x + 6);
            if (ha < 0 || hb < 0 || da < 0 || db < 0 || ph + 2 * ha + 8 * hb > hn[F_COL] || pd + 2 * da + 8 * db > dn[F_COL]) {
                free(heads.d); free(data.d); NEED(0);
            }
            const uint8_t *hi = CH + ph, *hc = hi + 2 * ha, *di = CD + pd, *dc = di + 2 * da;
            /* z -> cells: hub's, then the donor's new-room cells */
            static uint8_t cells[256][64][8];
            static int ncell[256];
            memset(ncell, 0, sizeof ncell);
            uint8_t touched[256] = { 0 }, present[256] = { 0 };
            for (int k = 0; k < ha; k++) {
                const int z = hi[2 * k], st = hi[2 * k + 1], en = k + 1 < ha ? hi[2 * k + 3] : hb;
                present[z] = 1;
                if (en - st > 64) { free(heads.d); free(data.d); NEED(0); }
                for (int j = st; j < en; j++) memcpy(cells[z][ncell[z]++], hc + 8 * j, 8);
            }
            for (int k = 0; k < da; k++) {
                const int z = di[2 * k] + dz, st = di[2 * k + 1], en = k + 1 < da ? di[2 * k + 3] : db;
                for (int j = st; j < en; j++) {
                    const uint8_t *c = dc + 8 * j;
                    if (c[7] != 15 && c[7] != 44 && c[7] != 18 && c[7] != 33) continue;
                    if (z > 255 || ncell[z] >= 64 || c[0] + dy > 254 || (c[2] != 255 && c[2] + dy > 254)) { free(heads.d); free(data.d); NEED(0); }
                    uint8_t *n = cells[z][ncell[z]++];
                    memcpy(n, c, 8);
                    n[0] = (uint8_t)(c[0] + dy);
                    if (n[2] != 255) n[2] = (uint8_t)(n[2] + dy);
                    n[7] = (uint8_t)map[c[7]];
                    present[z] = touched[z] = 1; added++;
                }
            }
            Buf idx = { 0 }, cb = { 0 };
            int nz = 0, kc = 0;
            for (int z = 0; z < 256; z++) {
                if (!present[z]) continue;
                if (touched[z]) {   /* stable sort by floor byte (lowest floor first), last flag on the top one */
                    for (int a = 1; a < ncell[z]; a++)
                        for (int b = a; b > 0 && cells[z][b - 1][0] > cells[z][b][0]; b--) {
                            uint8_t t[8]; memcpy(t, cells[z][b], 8); memcpy(cells[z][b], cells[z][b - 1], 8); memcpy(cells[z][b - 1], t, 8);
                        }
                    for (int a = 0; a < ncell[z]; a++) cells[z][a][6] = (uint8_t)((cells[z][a][6] & ~1) | (a == ncell[z] - 1));
                }
                const uint8_t e[2] = { (uint8_t)z, (uint8_t)kc };
                put(&idx, e, 2);
                for (int a = 0; a < ncell[z]; a++) put(&cb, cells[z][a], 8);
                kc += ncell[z]; nz++;
            }
            if (kc >= 256) { free(idx.d); free(cb.d); free(heads.d); free(data.d); NEED(0); }
            put32(&heads, 0);
            { const int16_t v[2] = { (int16_t)nz, (int16_t)kc }; put(&heads, v, 4); }
            put(&data, idx.d, idx.n); put(&data, cb.d, cb.n);
            free(idx.d); free(cb.d);
            ph += 2 * ha + 8 * hb; pd += 2 * da + 8 * db;
        }
        const uint32_t so = rd32(CH + 40);
        if (so < ph || so > hn[F_COL]) { free(heads.d); free(data.d); NEED(0); }
        uint8_t hd[44];
        memcpy(hd, CH, 44);
        wr32(hd + 40, (uint32_t)(44 + heads.n + data.n));
        put(&cl, hd, 44); put(&cl, heads.d, heads.n); put(&cl, data.d, data.n);
        put(&cl, CH + so, hn[F_COL] - so);
        free(heads.d); free(data.d);
        if (lg) rt_logf("hubworld: collision +%u cells\n", added);
    }

    /* ---- objects ---- */
    {
        const uint32_t nh = rd32(h[F_LP]), nd = rd32(d[F_LP]);
        NEED(4 + 96 * nh == hn[F_LP] && 4 + 96 * nd == dn[F_LP]);
        static const int32_t knob[2][3] = { { 33986, -874, -898 }, { 39802, -874, 16501 } };
        put32(&lp, 0);
        uint32_t cnt = 0, gone = 0, added = 0;
        for (uint32_t i = 0; i < nh; i++) {
            const uint8_t *r = h[F_LP] + 4 + 96 * i;
            int32_t v[5];
            memcpy(v, r, 20);
            int drop = 0;
            for (int k = 0; k < 2; k++) drop |= v[0] == 1 && v[1] == 527 && v[2] == knob[k][0] && v[3] == knob[k][1] && v[4] == knob[k][2];
            if (drop) { gone++; continue; }
            put(&lp, r, 96); cnt++;
        }
        for (uint32_t i = 0; i < nd; i++) {
            const uint8_t *r = d[F_LP] + 4 + 96 * i;
            int32_t v[5];
            memcpy(v, r, 20);
            const int fam = v[2] >= 31744 && v[2] <= 41984 && v[3] >= -4096 && v[3] <= 1024 && v[4] >= -10240 && v[4] <= -1024;
            const int gar = v[2] >= 39936 && v[2] <= 52224 && v[3] >= -4096 && v[3] <= 1024 && v[4] >= 6144 && v[4] <= 20480;
            const int hall = v[2] >= 33792 && v[2] <= 39936 && v[4] >= -1024;   /* the hub's own kitchen / hall */
            if (v[0] != 1 || !(fam || gar) || hall) continue;
            put(&lp, r, 96); cnt++; added++;
        }
        wr32(lp.d, cnt);
        NEED(gone == 2);
        if (lg) rt_logf("hubworld: objects -%u knobs +%u\n", gone, added);
    }

    /* ---- models ---- */
    {
        static const uint32_t take[] = { 464, 482, 483 };   /* see the header comment */
        const uint8_t *M = h[F_MOD];
        const uint32_t n = rd32(M + 4), base = 8 + 4 * n;
        NEED(rd32(M) == 0xc0de0001u && base <= hn[F_MOD]);
        uint8_t *recs = malloc(hn[F_MOD] - base + 65536);
        NEED(recs);
        uint32_t rl = hn[F_MOD] - base, *ents = malloc(4 * (n + 8)), ne = 0;
        if (!ents) { free(recs); NEED(0); }
        memcpy(recs, M + base, rl);
        uint32_t addid[8], addoff[8], na = 0;
        for (unsigned t = 0; t < sizeof take / sizeof take[0]; t++) {
            uint32_t doff, dlen, hoff, hlen;
            if (!bank_rec(d[F_MOD], dn[F_MOD], take[t], &doff, &dlen) || dlen > 65536) { free(recs); free(ents); NEED(0); }
            uint8_t *m = malloc(dlen);
            if (!m) { free(recs); free(ents); NEED(0); }
            memcpy(m, d[F_MOD] + doff, dlen);
            if (!conv_model(m, dlen)) { free(m); free(recs); free(ents); rt_logf("hubworld: model %u textures\n", take[t]); goto out; }
            if (bank_rec(M, hn[F_MOD], take[t], &hoff, &hlen) && hlen == dlen) memcpy(recs + hoff - base, m, dlen);
            else {
                recs = realloc(recs, rl + dlen);
                memcpy(recs + rl, m, dlen);
                addid[na] = take[t]; addoff[na++] = rl; rl += dlen;
            }
            free(m);
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t e = rd32(M + 8 + 4 * i);
            int skip = 0;
            for (uint32_t k = 0; k < na; k++) skip |= (e >> 21) == addid[k];
            if (!skip) ents[ne++] = e;
        }
        for (uint32_t k = 0; k < na; k++) ents[ne++] = addid[k] << 21 | addoff[k];
        for (uint32_t a = 1; a < ne; a++)            /* the game looks models up by binary search on the id */
            for (uint32_t b = a; b > 0 && (ents[b - 1] >> 21) > (ents[b] >> 21); b--) { const uint32_t t = ents[b]; ents[b] = ents[b - 1]; ents[b - 1] = t; }
        put32(&md, 0xc0de0001u); put32(&md, ne); put(&md, ents, 4 * ne); put(&md, recs, rl);
        free(recs); free(ents);
    }
#undef NEED
    vf[F_LEVEL].d = lv.d; vf[F_LEVEL].n = (uint32_t)lv.n; lv.d = NULL;
    vf[F_COL].d = cl.d; vf[F_COL].n = (uint32_t)cl.n; cl.d = NULL;
    vf[F_LP].d = lp.d; vf[F_LP].n = (uint32_t)lp.n; lp.d = NULL;
    vf[F_MOD].d = md.d; vf[F_MOD].n = (uint32_t)md.n; md.d = NULL;
    vf[F_RAW].d = raw; vf[F_RAW].n = hn[F_RAW]; raw = NULL;
    {
        uint32_t at = HUB_LBA;
        for (int i = 0; i < F_N; i++) { vf[i].lba = at; vf[i].nsect = (vf[i].n + 2047) / 2048; at += (vf[i].nsect + 15) & ~15u; }
        if (at > HUB_LBA + HUB_NSECT) { rt_logf("hubworld: files too big\n"); goto out; }
    }
    {   /* debug: RUGRATS_HUBDUMP=<dir> writes the merged files (game data: never share them) */
        const char *e = getenv("RUGRATS_HUBDUMP");
        for (int i = 0; e && *e && i < F_N; i++) {
            char path[600];
            snprintf(path, sizeof path, "%s/%s", e, fname[i]);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(vf[i].d, 1, vf[i].n, f); fclose(f); }
        }
    }
    rt_logf("hubworld: family room + garage from db 10: level %u bytes (48 rooms, %u textures, %d same place, %d moved, "
            "%d new palettes), collision %u, objects %u, models %u\n", vf[F_LEVEL].n, ntex, tex_same, tex_moved, newpal,
            vf[F_COL].n, vf[F_LP].n, vf[F_MOD].n);
    ok = 1;
out:
    for (int i = 0; i < F_N; i++) { free(h[i]); free(d[i]); }
    free(lv.d); free(cl.d); free(lp.d); free(md.d); free(raw);
    free(HA0); free(NA); free(NB); HA0 = NULL; NA = NB = NULL; HA = NULL; HB = NULL;
    free(tex.d); tex.d = NULL; tex.n = tex.cap = 0;
    if (!ok) for (int i = 0; i < F_N; i++) { free(vf[i].d); vf[i].d = NULL; }
    return ok;
}

/* ---- heap extension ---------------------------------------------------------------------------------------- */
static void hub_heap_ext(void) {
    const uint32_t end = mem_r32(GP + 0xba4), last = HUB_EXT + HUB_EXT_SIZE - 4;
    if (end == last) return;                                  /* already there */
    if (end < 0x80000000u || end >= 0x80200000u || mem_r32(end) != 0) {
        rt_logf("hubworld: heap end %08x not as expected, no extension\n", end);
        return;
    }
    mem_w32(end, (uint32_t)-(int32_t)((HUB_EXT - (end + 4)) / 4));   /* used block bridging to the extension */
    mem_w32(HUB_EXT, (HUB_EXT_SIZE - 8) / 4);                         /* one free block */
    mem_w32(last, 0);                                                 /* new end marker */
    mem_w32(GP + 0xba4, last);
    rt_logf("hubworld: heap extended by %u KB at %08x (old end %08x)\n", (HUB_EXT_SIZE - 8) / 1024, HUB_EXT, end);
}
void hubrooms_state(StateIO *s) {
    st_prefix(s, "hubworld");
    uint32_t heap_ext_on = !st_loading(s) && mem_r32(GP + 0xba4) == HUB_EXT + HUB_EXT_SIZE - 4;
    ST(s, heap_ext_on);
    if (heap_ext_on) ST_N(s, "heap_ext", ram + (HUB_EXT & 0x7FFFFF), HUB_EXT_SIZE);
}

/* ---- applying ------------------------------------------------------------------------------------------------ */
static int want(void) {
    static int env = -2;
    if (env == -2) { const char *e = getenv("RUGRATS_HUBWORLD"); env = e ? atoi(e) != 0 : -1; }
    return env >= 0 ? env : opt.hub_world;
}
int hubrooms_on(void) { return want(); }   /* hooks.c: sleeping Grandpa is part of the same option */
static uint32_t orig[F_N][2];
static int orig_ok[F_N];
static void apply(void) {
    if (lg < 0) lg = getenv("RUGRATS_HUBLOG") != NULL;
    int on = want();
    if (on && state == 0) state = build() ? 1 : -1;
    if (state < 0) on = 0;
    for (int i = 0; i < F_N; i++) {
        const uint32_t a = FILE_TAB + hub_id[i] * 8u, l = mem_r32(a), n = mem_r32(a + 4);
        if (l < HUB_LBA || l >= HUB_LBA + HUB_NSECT) { orig[i][0] = l; orig[i][1] = n; orig_ok[i] = 1; }   /* the table's own (or a mod's) */
        if (!orig_ok[i]) { exe_entry(hub_id[i], &orig[i][0], &orig[i][1]); orig_ok[i] = 1; }
        mem_w32(a, on ? vf[i].lba : orig[i][0]);
        mem_w32(a + 4, on ? vf[i].n : orig[i][1]);
    }
    uint32_t pool;
    memcpy(&pool, game_exe + 0x800 + (POOL_TAB + 4 - 0x80010000u), 4);
    mem_w32(POOL_TAB + 4, on ? pool + POOL_MORE : pool);
    if (on) hub_heap_ext();
    static int last = -1;
    if (on != last) { rt_logf("hubworld: %s\n", on ? "family room + garage on" : "off (original house)"); last = on; }
}
/* the game's file readers (a0 = file id) */
void hubrooms_file(uint32_t id) {
    for (int i = 0; i < F_N; i++) if (id == hub_id[i]) { apply(); return; }
}
/* object pool setup 80046340 (a0 = db) */
void hubrooms_pool(uint32_t db) { if (db == 1) apply(); }

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
/* cdrom.c asks here first: 1 = this sector is one of ours */
int hub_sector(uint32_t lba, uint8_t *out) {
    if (lba < HUB_LBA || lba >= HUB_LBA + HUB_NSECT) return 0;
    if (lg < 0) lg = getenv("RUGRATS_HUBLOG") != NULL;
    if (state == 0) state = build() ? 1 : -1;   /* a quick load straight into the hub */
    if (state < 0) return 0;
    for (int i = 0; i < F_N; i++) {
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
