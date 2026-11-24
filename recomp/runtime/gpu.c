/* PS1 GPU command processor + software rasterizer into a 1024x512 16-bit VRAM. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fastmath.h"
#include "hw.h"
#include "gpu_d3d11.h"

uint16_t vram[512 * 1024];
uint32_t gpu_flips; /* display buffer swaps (= game frames shown) */

static struct {
    /* draw state */
    int tpx, tpy, tdepth, semi, dither, draw_disp, tex_disable;
    int twmx, twmy, twox, twoy;
    int dx1, dy1, dx2, dy2, offx, offy;
    int mask_set, mask_check;
    int rect_flipx, rect_flipy;
    /* display */
    int disp_x, disp_y, h1, h2, v1, v2, mode, disp_off, dma_dir;
    int odd;
    uint32_t irq;
    /* command fifo */
    uint32_t fifo[16];
    int fifo_n, fifo_need;
    /* transfers */
    int xfer, xx, xy, xw, xh, xcx, xcy;  /* xfer: 1 = cpu->vram, 2 = vram->cpu */
    int polyline;
    uint32_t gpuread;
} g;

void gpu_init(void) {
    memset(&g, 0, sizeof g);
    g.dx2 = 1023; g.dy2 = 511;
    g.disp_off = 1;
    g.h1 = 0x200; g.h2 = 0x200 + 2560; g.v1 = 0x10; g.v2 = 0x10 + 240;
}

static int sext11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

/* ---- pixel helpers -------------------------------------------------- */
static const int dither_tab[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

static inline uint16_t blend(uint16_t back, uint16_t front, int mode) {
    int br = back & 31, bg = (back >> 5) & 31, bb = (back >> 10) & 31;
    int fr = front & 31, fg = (front >> 5) & 31, fb = (front >> 10) & 31;
    int r, gg, b;
    switch (mode) {
    case 0: r = (br + fr) >> 1; gg = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; gg = bg + fg; b = bb + fb; break;
    case 2: r = br - fr; gg = bg - fg; b = bb - fb; break;
    default: r = br + (fr >> 2); gg = bg + (fg >> 2); b = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    gg = gg < 0 ? 0 : gg > 31 ? 31 : gg;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)(r | (gg << 5) | (b << 10) | (front & 0x8000));
}

/* Render targets. "nat" is the real 1024x512 VRAM: always drawn exactly like the console, on the
   calling thread, so texture reads, VRAM copies and CPU readback stay correct. "hr" is an optional
   shadow at s x the resolution: every drawing command is queued and rasterized into it again at full
   detail by worker threads, each owning interleaved 8-row bands. The host displays the shadow. */
typedef struct { uint16_t *buf; int s, w, h; } Tgt;
static Tgt nat = { vram, 1, 1024, 512 };
static int nat_nocost; /* native raster of a clipped piece: the caller charges the GPU time (see native_mode) */
static Tgt hr = { NULL, 1, 1024, 512 };
/* depth of the hi-res shadow (CPU renderer, opt.depthfix): 4096 / camera z of the last room polygon drawn on each
   pixel, 0 = none. Room geometry writes it, world objects are hidden where they are clearly behind it. */
static float *hr_z;
/* object depth (opt.objsort): stored by camera-facing opaque objects (objsort_store), checked by objects only,
   cleared where room geometry or a fill paints. Room pieces never look at it. */
static float *hr_oz;
/* an object pixel is hidden only when it is more than DEPTH_GAP camera units (GTE SZ scale; a room block is 1024)
   behind the room pixel (RUGRATS_DEPTHGAP to try others). A fixed gap, not a percentage: 10% let a box ~360 units
   behind a block at ~7000 show through, 5% cut slivers out of books leaning ~130 units into a wall at ~2500.
   With iz = 4096 / z: visible while z_obj <= z_room + gap, i.e. iz_room <= iz_obj / (1 - iz_obj * gap / 4096). */
float gpu_depth_k(void) {   /* gap / 4096 */
    static float k = -1;
    if (k < 0) { const char *e = getenv("RUGRATS_DEPTHGAP"); k = (e ? (float)atof(e) : 96.0f) / 4096.0f; }
    return k;
}
/* slope bias: room depth is also pushed back by its own depth change over this many native pixels, so surfaces seen
   at a grazing angle (floors) get a larger margin than faces turned to the camera (RUGRATS_DEPTHSLOPE) */
/* anisotropic filtering (opt.aniso, gpu_d3d11.c shade): most texel taps per pixel, 1 = off */
int gpu_aniso(void) { return opt.aniso > 1 ? (opt.aniso > 16 ? 16 : opt.aniso) : 1; }
/* MSAA samples for the GPU renderer's target (opt.msaa: 1 off, 2, 4, 8) */
int gpu_msaa(void) { return opt.msaa >= 8 ? 8 : opt.msaa >= 4 ? 4 : opt.msaa >= 2 ? 2 : 1; }

/* faces of still props seen from above (floor-like, OID_FLOOR) are hidden behind a character's pixel only when this
   much further behind it (camera units, RUGRATS_FLOORGAP, default 96): the game draws floors and shadows over a
   character's feet on purpose (sinking into the soap), but the armchair's armrest edge (a thin top face ~600 units
   behind Tommy's head) painted a pink spike across his face. Returns the extra gap / 4096. */
float gpu_floor_k(void) {
    static float k = -1;
    if (k < 0) { const char *e = getenv("RUGRATS_FLOORGAP"); k = (e ? (float)atof(e) : 96.0f) / 4096.0f; }
    return k;
}
float gpu_depth_slope(void) {
    static float k = -1;
    if (k < 0) { const char *e = getenv("RUGRATS_DEPTHSLOPE"); k = e ? (float)atof(e) : 2.0f; }
    return k;
}
/* room pieces are also hidden where they are more than the gap behind nearer room depth: the room is sorted by each
   polygon's farthest corner, so a far piece (a box on a shelf) can be drawn after a big near wall. Same rule as for
   objects. RUGRATS_ROOMTEST=0 turns it off (debug; 2 = no gap, for comparison). */
static int room_test(void) {
    static int r = -1;
    if (r < 0) r = getenv("RUGRATS_ROOMTEST") ? atoi(getenv("RUGRATS_ROOMTEST")) : 1;
    return r;
}
int gpu_room_test(void) { return room_test() == 1; }
static inline float depth_reach(float iz) {   /* nearest room depth an object point at iz still shows in front of */
    float k = iz * gpu_depth_k();
    return k >= 0.999f ? 1e30f : iz / (1.0f - k);
}
/* Object sort fix (opt.objsort, DM_TW). The game sorts each face of a model on its own, with a per-face rule (average,
   nearest or farthest corner), so a far face can be drawn after a nearer one of the same model: a toy store shelf's
   purple back panel (sorted by its average) cut a wedge into the slanted wood top of a box in front of it (sorted by
   its farthest corner) while the camera turned. GTE output matches PCSX-Redux exactly; it is the game's own order.
   Fix: an opaque object triangle facing the camera also stores its depth where it is drawn, in a separate object
   depth buffer (hr_oz; D3D11: R32_FLOAT target), so later object pixels clearly behind it are hidden. Room pieces
   and fills clear it where they paint (a floor drawn over part of an object must not leave that object's depth to
   hide things later: the first version shared the room depth and cut pieces out of Tommy) and never test it, so
   the room / object rules stay as they were. Back-facing triangles (double-sided faces seen from behind, e.g. a lid
   from below that the game always paints over) store nothing and draw as before. Object vs object uses a smaller gap
   (RUGRATS_OBJGAP, camera units, default 24): the stored value is pushed back by (room gap - object gap), so the one
   test rule (depth_reach) gives the object gap. */
float gpu_objsort_k(void) {   /* (room gap - object gap) / 4096 */
    static float k = -1;
    if (k < 0) {
        const char *e = getenv("RUGRATS_OBJGAP");
        float og = e ? (float)atof(e) : 24.0f;
        k = gpu_depth_k() - og / 4096.0f;
        if (k < 0) k = 0;
    }
    return k;
}
static inline float objsort_store(float iz) {   /* depth value a DM_TW pixel stores */
    float k = iz * gpu_objsort_k();
    return k >= 0.99f ? 1e29f : iz / (1.0f - k);
}
/* object depth values carry the model id in their 13 lowest mantissa bits (D3D11 does the same; MAX still keeps the
   nearest, to ~0.1%). Bit 12 (OID_ACTOR) = a character's pixel: it also hides later pixels of still props' faces
   (polygon oid bit OID_STATIC, see exec_poly and hooks.c dcls_run) that are clearly behind it. Characters are never
   hidden by this, they only keep what the game drew of them. */
/* Room over object (part of objsort; RUGRATS_ROOMOBJ=0 = off). The game sorts room pieces and object faces in one
   list by per-polygon depth, so at some camera angles a floor piece lands AFTER an object face standing on it and
   paints over it (the armchair's skirt lost its lower triangle in single frames while the stick camera turned;
   same order with every fix off). A room pixel now leaves the picture alone where a stored object pixel would have
   is nearer than it (its true depth vs the room depth with the slope bias, as stored). The room pixel still stores
   its depth. Optional second condition (gpu_room_obj_span, off by default, see there): AND not nearer than the
   whole room polygon (Vtx.zl = its nearest corner + the room gap).
   Why that second condition existed: a sort tie can only happen between polygons whose depth ranges overlap. An object
   pixel nearer than every point of a room polygon drawn after it was put behind it by the game on purpose: the toy
   store stand has a light-blue lid right at the lens of a low camera that sits near the back of the ordering
   table and is painted over by the floor and then by the stand's own faces (the first version, without this
   condition, showed it; the user had already rejected that lid once). It also leaves far scenery as the game
   draws it (a doghouse behind a brick porch in the trike level).
   No gap on the first condition (RUGRATS_ROOMOBJGAP=<camera units> to try one); the slope bias alone keeps things
   standing on a floor: the armchair's bottom edge is ~1% deeper than the floor under it, the bias is ~1.6% there.
   Where the room pixel does paint, the object depth is replaced by "room marks": depths as an object would have
   stored them, model id 0 (hides nothing: every object depth check needs an id): MAX with the room depth, then MIN
   with zl. D3D11 can do that in two draws (it cannot read and clear oz in one); the CPU path does the same. What
   survives are exactly the object values that were kept on screen. (Fills still clear.) */
int gpu_room_obj(void) {
    static int r = -1;
    if (r < 0) r = getenv("RUGRATS_ROOMOBJ") ? atoi(getenv("RUGRATS_ROOMOBJ")) != 0 : 1;
    return r;
}
/* The "not nearer than the whole polygon" condition is OFF by default (RUGRATS_ROOMOBJSPAN=1 = on): the user decided
   (2026-11-10) that the stand's light-blue lid is correct and must show. Without it the rule is plain depth: an
   object pixel in front of a room pixel stays, whatever the draw order. */
int gpu_room_obj_span(void) {
    static int r = -1;
    if (r < 0) r = getenv("RUGRATS_ROOMOBJSPAN") ? atoi(getenv("RUGRATS_ROOMOBJSPAN")) != 0 : 0;
    return r;
}
/* Room pieces with an unsure corner depth (DM_WNT: two room depths on a corner's pixel) store the FAR candidate
   (safe for the object test: hides less), but for this rule the far one is the unsafe side: a split wall piece next to
   the house stairs looked behind the handrail and left its pixels (flicker through the wall). They use their NEAR
   candidates here (Vtx.izn, D3D11 HwVert.zn). RUGRATS_ROOMOBJWNT: 2 = near (default), 1 = far (before 2026-11-10),
   0 = they do not take part (paint and clear the object depth: the armchair skirt lost its triangle again). */
int gpu_room_obj_wnt(void) {
    static int r = -1;
    if (r < 0) r = getenv("RUGRATS_ROOMOBJWNT") ? atoi(getenv("RUGRATS_ROOMOBJWNT")) : 2;
    return r;
}
float gpu_room_obj_k(void) {   /* objsort_k - gap / 4096: takes the stored push back out (and adds a test gap) */
    static float k = -1e9f;
    if (k < -1e8f) { const char *e = getenv("RUGRATS_ROOMOBJGAP"); k = gpu_objsort_k() - (e ? (float)atof(e) : 0.0f) / 4096.0f; }
    return k;
}
static inline float oz_reach(float oz) {   /* stored object depth (objsort_store) -> that object pixel's depth (+ gap) */
    float k = 1.0f + oz * gpu_room_obj_k();
    return k <= 0.001f ? 1e30f : oz / k;
}
static inline float oz_mark(float rz) {    /* inverse of oz_reach: room depth as an object pixel there would be stored */
    if (rz <= 0) return 0;
    float k = 1.0f - rz * gpu_room_obj_k();
    return k <= 0.001f ? 1e29f : rz / k;
}
static inline float oz_pack(float z, unsigned id) { uint32_t u; memcpy(&u, &z, 4); u = (u & ~OID_MASK) | id; memcpy(&z, &u, 4); return z; }
static inline unsigned oz_id(float z) { uint32_t u; memcpy(&u, &z, 4); return u & OID_MASK; }
/* Props in front of props (part of objsort; 2026-11-20, laundry room): the model sort fix only hides faces of the SAME
   model, so two props standing close together keep the game's order, and the game can draw a face of one AFTER a
   nearer face of the other: the top-loader's front painted over most of the front-loader's left border, ~70 camera
   units in front of it (only a notch of it was left). Now a still prop's opaque pixel (polygon OID_STATIC, model id
   set) stores its id with OID_ACTOR (bit 12) like a character's pixel, so a later still prop face of ANOTHER model
   clearly behind it is hidden by the "behind a character" rule (object gap; floor-like faces also the floor gap).
   Its own model's faces still match it as the same model (id | OID_ACTOR). Same 13 id bits as before (a first try
   with an extra bit 13 made the stored depth coarser and cut pixels of Tommy's hair strands).
   Moving props and characters are never hidden by this (only still props mark and get hidden). Cross-model sorting
   was first dropped (2026-11-08) because it showed the toy store stand's light-blue lid; the user wants that lid
   shown since 2026-11-10. RUGRATS_PROPSORT=0 = off (old). */
int gpu_propsort(void) {
    static int r = -1;
    if (r < 0) { const char *e = getenv("RUGRATS_PROPSORT"); r = e ? atoi(e) != 0 : 1; }
    return r && opt.objsort;
}
static int depth_vis = -1;   /* RUGRATS_DEPTHVIS=1 (CPU renderer, debug): show what the depth check misses */
static int hw_s; /* scale when the D3D11 hardware renderer (gpu_d3d11.c) draws the hi-res picture, else 0 */

/* drawing state a command needs (snapshot, so queued commands don't see later changes) */
typedef struct {
    int dx1, dy1, dx2, dy2, dither, semi, mask_set, mask_check;
    int twmx, twmy, twox, twoy, rect_flipx, rect_flipy, tpx, tpy, tdepth;
} St;

static St cur_st(void) {
    /* opt.dither != 1: no dither pattern anywhere (native VRAM too; it only changes colours, not game logic) */
    St s = { g.dx1, g.dy1, g.dx2, g.dy2, g.dither && opt.dither == 1, g.semi, g.mask_set, g.mask_check,
             g.twmx, g.twmy, g.twox, g.twoy, g.rect_flipx, g.rect_flipy, g.tpx, g.tpy, g.tdepth };
    return s;
}

static inline void plot(const St *st, const Tgt *t, int x, int y, uint16_t c, int semi_on) {
    uint16_t *p = &t->buf[y * t->w + x];
    if (st->mask_check && (*p & 0x8000)) return;
    if (semi_on) c = blend(*p, c, st->semi);
    *p = c | (st->mask_set ? 0x8000 : 0);
}

/* row ownership for worker `band` of `nb` (nb == 1: all rows) */
#define OWNS(y, band, nb) ((nb) == 1 || (((y) >> 3) % (nb)) == (band))

static inline uint16_t texel(const St *st, int u, int v, int clutx, int cluty, int tpx, int tpy, int depth) {
    u = (u & ~(st->twmx * 8)) | ((st->twox & st->twmx) * 8);
    v = (v & ~(st->twmy * 8)) | ((st->twoy & st->twmy) * 8);
    u &= 255; v &= 255;
    int y = (tpy + v) & 511;
    switch (depth) {
    case 0: {
        uint16_t w = vram[y * 1024 + ((tpx + u / 4) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 15;
        return vram[cluty * 1024 + ((clutx + idx) & 1023)];
    }
    case 1: {
        uint16_t w = vram[y * 1024 + ((tpx + u / 2) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 255;
        return vram[cluty * 1024 + ((clutx + idx) & 1023)];
    }
    default:
        return vram[y * 1024 + ((tpx + u) & 1023)];
    }
}

static inline uint16_t shade(uint16_t t, int r, int gg, int b, int x, int y, int dith) {
    int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
    int R = (tr * r) >> 7, G = (tg * gg) >> 7, B = (tb * b) >> 7;
    if (dith) { int d = dither_tab[y & 3][x & 3]; R += d; G += d; B += d; }
    R = R < 0 ? 0 : R > 255 ? 255 : R;
    G = G < 0 ? 0 : G > 255 ? 255 : G;
    B = B < 0 ? 0 : B > 255 ? 255 : B;
    return (uint16_t)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10) | (t & 0x8000));
}

static inline uint16_t rgb15(int r, int gg, int b, int x, int y, int dith) {
    if (dith) { int d = dither_tab[y & 3][x & 3]; r += d; gg += d; b += d; }
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)((r >> 3) | ((gg >> 3) << 5) | ((b >> 3) << 10));
}

/* ---- triangles ------------------------------------------------------ */
/* fx/fy: sub-pixel offset in 1/256 px (hi-res only); iz: 4096 / camera z for the depth check, dm: depth mode
   of the primitive (DM_*, same in all its vertices) */
typedef struct { int x, y, r, g, b, u, v; int fx, fy; float iz; int dm; int oid; float zl; float izn; float pw; uint32_t seq; float uf, vf; float zs; } Vtx;   /* pw: perspective weight (1/z), 0 = affine */   /* izn: see gpu_room_obj_wnt */   /* oid: model id (objsort), 0 = none */
/* zl (room polygons, same in all vertices): depth_reach of the polygon's nearest corner, see gpu_room_obj */
enum { DM_NONE, DM_WRITE, DM_TEST, DM_WNT = 4, DM_HUD = 5, DM_HTEST = 6, DM_TW = 7 };   /* values = HW_DM_* (gpu_d3d11.h); WNT: writes
   depth, skips the room test; HUD / HTEST: HUD protection (see exec_poly); TW: object test + store (objsort_store) */
#define HUD_Z 1e30f   /* CPU depth value of a HUD pixel */

/* ---- sub-pixel vertex table (filled by the GTE, read when packets are parsed) ---- */
#define SUBPX_BITS 17
#define FX_AMB INT16_MIN        /* two different points on the same pixel this frame: don't guess */
#define FX_NONE (INT16_MIN + 1) /* depth only, no sub-pixel position */
/* izr / izo: depth (4096 / z) of a room / world-object point on this pixel, -1 = none; izrn: nearest room depth
   (== izr unless two room points at different depths share the pixel; exec_poly then picks per polygon) */
/* world: projected by the room or object renderer (even without a usable depth: too near, behind, clamped) */
/* oid[]: model ids of the object points on this pixel (subpx_cur_oid), 0 = free slot; oid[0] 0xFFFF = unknown (a
   split point between two models). Several ids: where models touch (a box on a shelf, neighbouring boxes) their corners
   share pixels; with one id those corners counted as "mixed" and the faces lost the objsort fix in some frames
   (blinking). Four, not two: Tommy's head parts (a new id every frame: animated) landed on a box corner, 3 models made
   the pixel "unknown" and the box top lost the fix in single frames. More than SUBPX_OIDS: the extra ones are dropped
   (a face can only lose the fix that way, never get a wrong id). */
#define SUBPX_OIDS 4
static struct { uint32_t key, gen; int16_t fx, fy; float izr, izo, izrn, izg; uint8_t world; uint16_t oid[SUBPX_OIDS]; float izid[SUBPX_OIDS];
                float cx, cy, cz; uint8_t c3, mk, ccn; uint32_t ma, mb; float cc[4][3]; int16_t fxc[3], fyc[3]; int16_t fxid[SUBPX_OIDS], fyid[SUBPX_OIDS]; } subpx_tab[1 << SUBPX_BITS];
/* fxc/fyc[0 room far, 1 object, 2 room near]: the sub-pixel position of that kind of point alone (room: of the point
   at izr / izrn). fx goes "unsure" (FX_AMB) when two different points share a pixel; a wall corner that a character
   corner (or a far wall's corner) lands on then lost its position in single frames and the whole wall piece jumped up
   to a pixel (castle wall, mini golf, while Angelica / the camera turns: 2026-11-11). exec_poly falls back to the
   polygon's own kind, for room polygons the point whose depth it took (RUGRATS_SUBPXCLS=0: off). */
/* izid[]: each model's own depth on this pixel (the nearest point of that model). izo keeps the nearest of all
   models; a face uses its own model's depth (exec_poly, RUGRATS_OIDZ=0 = off): a handrail corner under Tommy's head
   took the head's depth (1.12 vs 0.39), stored it, and the wall drawn after it left those pixels (room over object). */
/* fxid/fyid[]: each model's own sub-pixel position on this pixel (FX_NONE / FX_AMB as fx). A character's parts are
   separate models (head, body, arms); where two of them meet on one pixel the shared position was unsure and those
   corners snapped to whole pixels in single frames (Angelica jittering while she turns). exec_poly takes the face's
   own model's position there (RUGRATS_SUBPXCLS=0: off). */
static void subpx_oidfx(int h, int i, int fx, int fy) {   /* merge a point's position into model slot i */
    int16_t *o = &subpx_tab[h].fxid[i], *p = &subpx_tab[h].fyid[i];
    if (*o == FX_AMB || fx == FX_NONE) return;
    if (*o != FX_NONE && (abs(*o - fx) > 8 || abs(*p - fy) > 8)) {
        extern int gpu_cmdlog;
        if (gpu_cmdlog) rt_logf("oidamb %08x id %u %d,%d vs %d,%d\n", subpx_tab[h].key, subpx_tab[h].oid[i], *o, *p, fx, fy);
        *o = FX_AMB; return;
    }
    *o = (int16_t)fx; *p = (int16_t)fy;
}
static inline void subpx_addoid(int h, unsigned id, float iz, int fx, int fy) {
    uint16_t *o = subpx_tab[h].oid;
    float *z = subpx_tab[h].izid;
    if (!id || o[0] == 0xFFFF) return;
    if (id == 0xFFFF) { memset(o, 0, sizeof subpx_tab[h].oid); o[0] = 0xFFFF; return; }
    for (int i = 0; i < SUBPX_OIDS; i++) {
        if (o[i] == id) { if (iz > z[i]) z[i] = iz; subpx_oidfx(h, i, fx, fy); return; }
        if (!o[i]) { o[i] = (uint16_t)id; z[i] = iz; subpx_tab[h].fxid[i] = FX_NONE; subpx_oidfx(h, i, fx, fy); return; }
    }
    /* full: drop the oldest (ids are in the order they arrived; the newest ones are the frame being drawn) */
    memmove(o, o + 1, (SUBPX_OIDS - 1) * sizeof *o);
    memmove(z, z + 1, (SUBPX_OIDS - 1) * sizeof *z);
    memmove(subpx_tab[h].fxid, subpx_tab[h].fxid + 1, (SUBPX_OIDS - 1) * sizeof(int16_t));
    memmove(subpx_tab[h].fyid, subpx_tab[h].fyid + 1, (SUBPX_OIDS - 1) * sizeof(int16_t));
    o[SUBPX_OIDS - 1] = (uint16_t)id; z[SUBPX_OIDS - 1] = iz;
    subpx_tab[h].fxid[SUBPX_OIDS - 1] = FX_NONE; subpx_oidfx(h, SUBPX_OIDS - 1, fx, fy);
}
unsigned subpx_cur_oid;
static uint32_t subpx_gen = 1, subpx_n, subpx_hit, subpx_hist[32];
/* the table holds one game frame: a new generation starts at each display flip (above 60 fps the game draws several
   frames per vblank, and points of two frames on one pixel looked like two depths -> no depth while the camera turns).
   Falls back to every 3rd vblank when the game stops flipping. RUGRATS_SUBPXGEN=0: old per-vblank generations. */
static int subpx_byflip(void) { static int b = -1; if (b < 0) { const char *e = getenv("RUGRATS_SUBPXGEN"); b = e ? atoi(e) : 1; } return b; }
static int subpx_noflip;
extern int gpu_cmdlog;
static inline uint32_t subpx_hash(uint32_t k) { return (k * 2654435761u) >> (32 - SUBPX_BITS); }
/* Exact per-vertex positions: the game stores each projected point with swc2 into its packet (gte_swc2_sxy), which
   tags that RAM word here; the DMA tells gpu_gp0 where each packet word came from (gpu_src_addr -> gfa[]). A packet
   vertex whose word is still the tagged value (this or the last table generation) takes that point's own position,
   with no pixel guessing. Two points of one model on one pixel (Angelica's hair and face, dense far bushes) were
   unsure by pixel and snapped to whole pixels in single frames. Words the game writes otherwise (split midpoints,
   mfc2 + sw) use the pixel table as before. RUGRATS_SUBPXTAG=0: off. */
/* Up to TAG_PROBE slots per address (RUGRATS_TAGPROBE=1: old direct-mapped table). The table holds 2-3 game frames of
   tagged words (vertex buffers, packets, copies); direct-mapped, another word hashing to the same slot evicted a live
   tag. A near wall vertex in the vertex buffer lost its tag in single frames, the game's splits of that wall then fell
   back to flat 2D midpoints, and the wall cap jumped up and back while standing still (mini golf, 2026-11-11). */
#define TAG_BITS 17
#define TAG_PROBE 4
/* camera-space point of a tagged screen position (GTE x, y, z of the vertex; split points: the average of their ends).
   Kept also for points clamped at the screen limit or behind the camera, whose screen position says nothing about
   where they are (see clamp_uv in exec_poly). Same side channel as the depth: tp_in is stored by the next subpx_tag /
   subpx_tag_reg, tp_out is the last hit's. */
typedef struct { float p[3]; uint8_t ok; } TagP;
static TagP tp_in, tp_out;
void subpx_tag_p3(const float *p) { if (p) { tp_in.p[0] = p[0]; tp_in.p[1] = p[1]; tp_in.p[2] = p[2]; tp_in.ok = 1; } else tp_in.ok = 0; }
const float *subpx_tag_p3out(void) { return tp_out.ok ? tp_out.p : NULL; }
#define TP_STORE(d) do { (d) = tp_in; tp_in.ok = 0; } while (0)
static struct { uint32_t addr, val, gen; int32_t fx, fy; uint8_t has; float iz; TagP p3; } subpx_at[1 << TAG_BITS];   /* 32-bit: split points move far */
static int tagprobe(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_TAGPROBE"); z = e ? atoi(e) : TAG_PROBE; if (z < 1) z = 1; if (z > 8) z = 8; } return z; }
static uint32_t tag_evict;   /* debug: live tags overwritten by another address (RUGRATS_SUBPXLOG) */
static inline uint32_t tag_hash(uint32_t addr) { return (addr >> 2) * 2654435761u >> (32 - TAG_BITS); }
/* slot of addr for a store: its own slot, else a free / stale one, else the oldest of the probed slots */
static uint32_t tag_slot_put(uint32_t addr) {
    const uint32_t h = tag_hash(addr), n = (uint32_t)tagprobe();
    uint32_t free_i = 0xFFFFFFFFu, old_i = h, old_g = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t i = (h + k) & ((1u << TAG_BITS) - 1);
        if (subpx_at[i].addr == addr) return i;
        if (subpx_gen - subpx_at[i].gen > 1) { if (free_i == 0xFFFFFFFFu) free_i = i; }
        else if (subpx_at[i].gen < old_g) { old_g = subpx_at[i].gen; old_i = i; }
    }
    if (free_i != 0xFFFFFFFFu) return free_i;
    tag_evict++;
    return old_i;
}
static int tag_slot_get(uint32_t addr) {
    const uint32_t h = tag_hash(addr), n = (uint32_t)tagprobe();
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t i = (h + k) & ((1u << TAG_BITS) - 1);
        if (subpx_at[i].addr == addr) return (int)i;
    }
    return -1;
}
/* depth side channel: tag_iz_in is stored by the next subpx_tag / subpx_tag_reg (then reset), tag_iz_out is the
   depth (4096*4096/z, -1 unknown) of the last hit of subpx_tag_get / mid_cache_get. Nested split points take their
   ends' exact depths from here instead of the pixel table (two game frames per refresh share pixels there). */
static float tag_iz_in = -1.0f, tag_iz_out = -1.0f;
void subpx_tag_z(float iz) { tag_iz_in = iz; }
float subpx_tag_zout(void) { return tag_iz_out; }
uint32_t gpu_src_addr = 0xFFFFFFFF;   /* RAM address of the word being sent to GP0 (0xFFFFFFFF: CPU port write) */
static uint32_t gfa[16];              /* source address of each word in g.fifo */
uint8_t tag_pg[2048];
static int subpxtag(void);
static int subpx_tag_on(void) { return subpxtag(); }
static int subpxtag(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_SUBPXTAG"); z = e ? atoi(e) : 1; } return z; }
static struct { uint32_t val, gen; int32_t fx, fy; uint8_t has; float iz; TagP p3; } subpx_sp[256];   /* scratchpad words */
static inline int is_spad(uint32_t a) { return (a & 0x7FFFFC00u) == 0x1F800000u; }
int subpx_tag_get(uint32_t addr, uint32_t val, int *fx, int *fy);
void subpx_tag(uint32_t addr, uint32_t val, int has, int fx, int fy) {
    if (is_spad(addr)) {   /* the game projects into the scratchpad first, then copies into packets */
        const int k = (addr >> 2) & 255;
        subpx_sp[k].val = val; subpx_sp[k].gen = subpx_gen;
        subpx_sp[k].fx = fx; subpx_sp[k].fy = fy; subpx_sp[k].has = (uint8_t)has;
        subpx_sp[k].iz = tag_iz_in; tag_iz_in = -1.0f; TP_STORE(subpx_sp[k].p3);
        return;
    }
    addr &= 0x7FFFFC;
    tag_pg[addr >> 12] = 1;
    const uint32_t h = tag_slot_put(addr);
    subpx_at[h].addr = addr; subpx_at[h].val = val; subpx_at[h].gen = subpx_gen;
    subpx_at[h].fx = fx; subpx_at[h].fy = fy; subpx_at[h].has = (uint8_t)has;
    subpx_at[h].iz = tag_iz_in; tag_iz_in = -1.0f; TP_STORE(subpx_at[h].p3);
}
static struct { uint32_t val, gen; int32_t fx, fy; uint8_t has; float iz; TagP p3; } subpx_rg[32];   /* registers (mfc2) */
void subpx_tag_reg(int rt, uint32_t val, int has, int fx, int fy) {
    subpx_rg[rt].val = val; subpx_rg[rt].gen = subpx_gen; subpx_rg[rt].has = (uint8_t)has;
    subpx_rg[rt].fx = fx; subpx_rg[rt].fy = fy;
    subpx_rg[rt].iz = tag_iz_in; tag_iz_in = -1.0f; TP_STORE(subpx_rg[rt].p3);
}
/* sw of a register that holds a tagged screen position (mfc2, or lw of a tagged scratchpad / RAM word) with the same
   value: the copy gets the tag. The game projects into a vertex buffer and builds the packets from it. */
static int mid_cache_get(uint32_t val, int e[3]);
static int tag_mid;   /* the last subpx_tag_get hit was a split point (has bit 1) */
int subpx_tag_cp(uint32_t dst, uint32_t src, uint32_t val) {
    int fx, fy;
    if ((src & 0xFFFFFF00u) == 0xFFFFFF00u) {
        const int r = src & 31;
        if (!subpxtag()) return 0;
        if (subpx_rg[r].val != val || subpx_gen - subpx_rg[r].gen > 1) {
            /* the register was reused (a nested split refilled it) after the move that copied it: a split point
               keeps its position by value (recent midpoints) */
            int e[3];
            if (!mid_cache_get(val, e)) return 0;
            tag_iz_in = tag_iz_out; tp_in = tp_out;
            subpx_tag(dst, val, e[0] | 2, e[1], e[2]);
            return 1;
        }
        tag_iz_in = subpx_rg[r].iz; tp_in = subpx_rg[r].p3;
        subpx_tag(dst, val, subpx_rg[r].has, subpx_rg[r].fx, subpx_rg[r].fy);
        return 1;
    }
    const int t = subpx_tag_get(src, val, &fx, &fy);
    if (t >= 0) { tag_iz_in = tag_iz_out; tp_in = tp_out; subpx_tag(dst, val, t | tag_mid << 1, fx, fy); }
    return t >= 0;
}
/* profiling build (RECOMP_TAGALL=1): each new lw / sw pair that copied a tagged point, as "TAGSITE <lw> <sw>"
   (lw 0 = the register came from mfc2); recomp/tools/tagsites.txt lists them */
uint32_t tag_lpc[32];
/* debug build (RECOMP_WATCH=1): log every store to RAM word RUGRATS_WATCH=<hex> during GP0-logged vblanks */
void rt_watch(uint32_t a, uint32_t pc, uint32_t v, int rt, uint32_t src) {
    static uint32_t w = 1;
    if (w == 1) { const char *e = getenv("RUGRATS_WATCH"); w = e ? (uint32_t)strtoul(e, NULL, 16) & 0x1FFFFC : 0; }
    if (w && (a & 0x1FFFFC) == w)   /* RECOMP_WATCH=1 builds only */
        rt_logf("WATCH %06x <- %08x pc %08x ra %08x r%d src %08x lpc %08x\n", w, v, pc, cpu.r[31], rt, src, tag_lpc[rt]);
}
void tag_site(uint32_t lw_pc, uint32_t sw_pc) {
    static uint64_t seen[4096];
    const uint64_t k = (uint64_t)lw_pc << 32 | sw_pc;
    uint32_t h = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 52);
    while (seen[h] && seen[h] != k) h = (h + 1) & 4095;
    if (seen[h]) return;
    seen[h] = k;
    rt_logf("TAGSITE %08x %08x\n", lw_pc, sw_pc);
}
/* -1: no tag (use the pixel table), 0: tagged point without a sub-pixel position, 1: position in fx/fy */
int subpx_tag_get(uint32_t addr, uint32_t val, int *fx, int *fy) {
    tag_mid = 0; tp_out.ok = 0;
    if (addr == 0xFFFFFFFF || !subpxtag()) return -1;
    if (is_spad(addr)) {
        const int k = (addr >> 2) & 255;
        if (subpx_sp[k].val != val || subpx_gen - subpx_sp[k].gen > 1) return -1;
        tag_iz_out = subpx_sp[k].iz; tp_out = subpx_sp[k].p3;
        if (!subpx_sp[k].has) return 0;
        *fx = subpx_sp[k].fx; *fy = subpx_sp[k].fy;
        return 1;
    }
    addr &= 0x7FFFFC;
    const int h = tag_slot_get(addr);
    if (h < 0 || subpx_at[h].val != val || subpx_gen - subpx_at[h].gen > 1) return -1;
    tag_mid = subpx_at[h].has >> 1;
    tag_iz_out = subpx_at[h].iz; tp_out = subpx_at[h].p3;
    if (!(subpx_at[h].has & 1)) return 0;
    *fx = subpx_at[h].fx; *fy = subpx_at[h].fy;
    return 1;
}
/* up to 4 slots per point: a point still in use (this frame or the last) is never overwritten by another point that
   happens to hash to the same slot (that dropped ~1-2% of corners, e.g. a split wall's centre -> no depth there) */
#define SUBPX_PROBE 4
static inline int subpx_live(uint32_t i) { return subpx_gen - subpx_tab[i].gen <= 1; }
static int subpx_find(uint32_t sxy) {
    uint32_t h = subpx_hash(sxy);
    for (int k = 0; k < SUBPX_PROBE; k++) {
        uint32_t i = (h + k) & ((1u << SUBPX_BITS) - 1);
        if (subpx_tab[i].key == sxy && subpx_live(i)) return (int)i;
    }
    return -1;
}
static uint32_t subpx_slot(uint32_t sxy) {   /* slot to store sxy: its own, else a free one, else the oldest */
    uint32_t h = subpx_hash(sxy), best = h;
    for (int k = 0; k < SUBPX_PROBE; k++) {
        uint32_t i = (h + k) & ((1u << SUBPX_BITS) - 1);
        if (subpx_tab[i].key == sxy) return i;
        if (subpx_tab[i].gen < subpx_tab[best].gen) best = i;
    }
    return best;
}
/* iz < 0 = no depth (cls must then be DCLS_NONE). Two points of one kind on a pixel: a room point keeps the farther
   depth, an object point the nearer one, so a clash never hides more than either point alone would. Points without
   depth (other GTE users projecting the same corners) leave the depths alone. */
static inline void subpx_merge(float *d, float iz, int nearer) {
    if (*d < 0 || (nearer ? iz > *d : iz < *d)) *d = iz;
}
static void subpx_putc(int h, int c, int fx, int fy) {   /* per-kind position (slot c, see fxc); fx FX_NONE: none */
    int16_t *o = &subpx_tab[h].fxc[c], *p = &subpx_tab[h].fyc[c];
    if (*o == FX_AMB) return;
    if (fx == FX_NONE) return;   /* same depth without a position (a split midpoint's depth before its position) */
    if (*o != FX_NONE && (abs(*o - fx) > 8 || abs(*p - fy) > 8)) { *o = FX_AMB; return; }
    *o = (int16_t)fx; *p = (int16_t)fy;
}
static void subpx_setc(int h, int c, int fx, int fy) { subpx_tab[h].fxc[c] = (int16_t)fx; subpx_tab[h].fyc[c] = (int16_t)fy; }
static inline void subpx_initc(int h) { subpx_tab[h].fxc[0] = subpx_tab[h].fxc[1] = subpx_tab[h].fxc[2] = FX_NONE; }
/* a room point's position at depth iz, called before izr / izrn take iz in: the farthest point sets the far slot, the
   nearest the near one; another point at the same depth must agree (else unsure) */
static void subpx_putr(int h, float iz, int fx, int fy) {
    const float fr = subpx_tab[h].izr, nr = subpx_tab[h].izrn, e = iz * 1e-4f;
    if (fr < 0 || iz < fr - e) subpx_setc(h, 0, fx, fy); else if (iz <= fr + e) subpx_putc(h, 0, fx, fy);
    if (nr < 0 || iz > nr + e) subpx_setc(h, 2, fx, fy); else if (iz >= nr - e) subpx_putc(h, 2, fx, fy);
}
void subpx_put(uint32_t sxy, int has_fx, int fx, int fy, float iz, int cls) {
    uint32_t h = subpx_slot(sxy);
    static int amb = -1;
    if (amb < 0) amb = !getenv("RUGRATS_SUBPXAMB") || atoi(getenv("RUGRATS_SUBPXAMB"));
    if (iz < 0) cls = DCLS_NONE;
    if (subpx_tab[h].key != sxy || subpx_tab[h].gen != subpx_gen) {
        subpx_initc((int)h);
        subpx_tab[h].izr = subpx_tab[h].izrn = -1.0f;   /* (set again below) so subpx_putr sees a fresh pixel */
    }
    if (cls == DCLS_ROOM) subpx_putr((int)h, iz, has_fx ? fx : FX_NONE, fy);
    if (has_fx && cls == DCLS_OBJ) subpx_putc((int)h, 1, fx, fy);
    if (subpx_tab[h].key != sxy || subpx_tab[h].gen != subpx_gen) { /* first point on this pixel this frame */
        subpx_tab[h].key = sxy; subpx_tab[h].gen = subpx_gen;
        subpx_tab[h].fx = has_fx ? (int16_t)fx : FX_NONE; subpx_tab[h].fy = (int16_t)fy;
        subpx_tab[h].izr = cls == DCLS_ROOM ? iz : -1.0f; subpx_tab[h].izo = cls == DCLS_OBJ ? iz : -1.0f;
        subpx_tab[h].izrn = subpx_tab[h].izr; subpx_tab[h].izg = -1.0f;
        subpx_tab[h].world = cls != DCLS_NONE;
        subpx_tab[h].c3 = subpx_tab[h].mk = subpx_tab[h].ccn = 0;
        memset(subpx_tab[h].oid, 0, sizeof subpx_tab[h].oid);
        if (cls == DCLS_OBJ) subpx_addoid((int)h, subpx_cur_oid, iz, has_fx ? fx : FX_NONE, fy);
        if (has_fx) { subpx_hist[(fx + 256) >> 6 & 15]++; subpx_hist[16 + ((fy + 256) >> 6 & 15)]++; }
        return;
    }
    if (cls != DCLS_NONE) subpx_tab[h].world = 1;
    if (cls == DCLS_ROOM) { subpx_merge(&subpx_tab[h].izr, iz, 0); subpx_merge(&subpx_tab[h].izrn, iz, 1); }
    else if (cls == DCLS_OBJ) {
        subpx_merge(&subpx_tab[h].izo, iz, 1);
        subpx_addoid((int)h, subpx_cur_oid, iz, has_fx ? fx : FX_NONE, fy);
    }
    if (!has_fx) return;
    int16_t o = subpx_tab[h].fx;
    if (amb && o == FX_AMB) return;
    if (amb && o != FX_NONE && (abs(o - fx) > 8 || abs(subpx_tab[h].fy - fy) > 8)) {
        subpx_tab[h].fx = FX_AMB;
        return;
    }
    subpx_tab[h].fx = (int16_t)fx; subpx_tab[h].fy = (int16_t)fy;
    subpx_hist[(fx + 256) >> 6 & 15]++; subpx_hist[16 + ((fy + 256) >> 6 & 15)]++;
}
/* 4096/z of any sub-pixel point on this pixel, whatever drew it (the nearest; poly_grow's depth ramp). Called right
   after subpx_put for the same point. */
void subpx_putg(uint32_t sxy, float iz) {
    int h = subpx_find(sxy);
    if (h >= 0 && iz > subpx_tab[h].izg) subpx_tab[h].izg = iz;
}
static float subpx_zg(uint32_t sxy) {
    int h = subpx_find(sxy);
    return h < 0 ? -1.0f : subpx_tab[h].izg;
}
/* a room / object renderer point without a usable depth (HUD protection only needs to know it is not 2D) */
void subpx_world(uint32_t sxy) {
    uint32_t h = subpx_slot(sxy);
    if (subpx_tab[h].key != sxy || subpx_tab[h].gen != subpx_gen) {
        subpx_tab[h].key = sxy; subpx_tab[h].gen = subpx_gen;
        subpx_tab[h].fx = FX_NONE; subpx_tab[h].fy = 0;
        subpx_tab[h].izr = subpx_tab[h].izo = subpx_tab[h].izrn = subpx_tab[h].izg = -1.0f;
        subpx_tab[h].c3 = subpx_tab[h].mk = subpx_tab[h].ccn = 0;
        memset(subpx_tab[h].oid, 0, sizeof subpx_tab[h].oid);
        subpx_initc((int)h);
    }
    subpx_tab[h].world = 1;
}
/* the model ids on this pixel: returns how many (0..SUBPX_OIDS) into id[]; 0 also when unknown */
static int subpx_oids(uint32_t sxy, unsigned id[SUBPX_OIDS]) {
    int h = subpx_find(sxy), n = 0;
    if (h < 0 || subpx_tab[h].oid[0] == 0xFFFF) return 0;
    while (n < SUBPX_OIDS && subpx_tab[h].oid[n]) { id[n] = subpx_tab[h].oid[n]; n++; }
    return n;
}
static int oidz(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_OIDZ"); z = e ? atoi(e) : 1; } return z; }
/* depth of model id's point on this pixel (see subpx_addoid) */
static int subpx_oid_z(uint32_t sxy, unsigned id, float *iz) {
    int h = subpx_find(sxy);
    if (h < 0 || !id || subpx_tab[h].oid[0] == 0xFFFF) return 0;
    for (int i = 0; i < SUBPX_OIDS; i++)
        if (subpx_tab[h].oid[i] == id && subpx_tab[h].izid[i] > 0) { *iz = subpx_tab[h].izid[i]; return 1; }
    return 0;
}
static int subpx_hasoid(uint32_t sxy, unsigned id) {
    unsigned o[SUBPX_OIDS];
    int n = subpx_oids(sxy, o);
    for (int i = 0; i < n; i++) if (o[i] == id) return 1;
    return 0;
}
/* the model all these pixels share (0 = none). Several shared ids: the newest one. A table generation (display flip)
   holds 2-3 game frames above 60 fps, and a model's id changes whenever the camera moves, so a face whose corners stay
   on the same pixels for two game frames carries last frame's id and this frame's on every corner. That used to count
   as ambiguous (no id): the face then stored no object depth for that frame (the armchair skirt, painted over by the
   floor in single frames while the camera turned slowly). RUGRATS_OIDNEW=0: old rule. */
static unsigned subpx_common_oid(const uint32_t *sxy, int n) {
    static int newest = -1;
    if (newest < 0) newest = getenv("RUGRATS_OIDNEW") ? atoi(getenv("RUGRATS_OIDNEW")) != 0 : 1;
    unsigned c[SUBPX_OIDS], found = 0;
    int nc = subpx_oids(sxy[0], c);
    for (int i = 0; i < nc; i++) {
        int k = 1;
        while (k < n && subpx_hasoid(sxy[k], c[i])) k++;
        if (k == n) { if (found && !newest) return 0; found = c[i]; }
    }
    return found;
}
int hooks_actor_front(void);
int subpx_oid(uint32_t sxy) {
    unsigned o[SUBPX_OIDS];
    return subpx_oids(sxy, o) == 1 ? (int)o[0] : 0;
}
static int subpx_isworld(uint32_t sxy) {
    int h = subpx_find(sxy);
    return h >= 0 && (subpx_tab[h].world || subpx_tab[h].izr > 0 || subpx_tab[h].izo > 0);
}
int subpx_get(uint32_t sxy, int *fx, int *fy) {
    /* only this frame's or the previous frame's projections (packets are often drawn one frame later) */
    int h = subpx_find(sxy);
    if (h < 0 || subpx_tab[h].fx == FX_AMB || subpx_tab[h].fx == FX_NONE) return 0;
    *fx = subpx_tab[h].fx; *fy = subpx_tab[h].fy;
    return 1;
}
static int subpxcls(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_SUBPXCLS"); z = e ? atoi(e) : 1; } return z; }
/* the position of kind c (0 room, 1 object) on a pixel whose shared position is unsure (see fxc); room: of the point
   at depth iz (near or far; iz <= 0: far) */
static int subpx_getc(uint32_t sxy, int c, float iz, int *fx, int *fy) {
    int h = subpx_find(sxy);
    if (h < 0 || subpx_tab[h].fx != FX_AMB || !subpxcls()) return 0;
    if (c == 0 && iz > 0 && fabsf(iz - subpx_tab[h].izrn) < fabsf(iz - subpx_tab[h].izr)) c = 2;
    if (subpx_tab[h].fxc[c] == FX_AMB || subpx_tab[h].fxc[c] == FX_NONE) return 0;
    *fx = subpx_tab[h].fxc[c]; *fy = subpx_tab[h].fyc[c];
    return 1;
}
/* model id's own position on a pixel whose shared position is unsure (see fxid) */
static int subpx_getid(uint32_t sxy, unsigned id, int *fx, int *fy) {
    int h = subpx_find(sxy);
    if (h < 0 || !id || subpx_tab[h].fx != FX_AMB || subpx_tab[h].oid[0] == 0xFFFF || !subpxcls()) return 0;
    for (int i = 0; i < SUBPX_OIDS; i++)
        if (subpx_tab[h].oid[i] == id) {
            if (subpx_tab[h].fxid[i] == FX_AMB || subpx_tab[h].fxid[i] == FX_NONE) return 0;
            *fx = subpx_tab[h].fxid[i]; *fy = subpx_tab[h].fyid[i];
            return 1;
        }
    return 0;
}
/* depth of a cls (DCLS_ROOM / DCLS_OBJ) point on this pixel this frame or the previous one */
int subpx_z(uint32_t sxy, int cls, float *iz) {
    int h = subpx_find(sxy);
    if (h < 0) return 0;
    float d = cls == DCLS_ROOM ? subpx_tab[h].izr : cls == DCLS_OBJ ? subpx_tab[h].izo : -1.0f;
    if (d < 0) return 0;
    *iz = d;
    return 1;
}
/* both room depths on this pixel (far = what subpx_z returns, near = the nearest room point seen there) */
static int subpx_zr(uint32_t sxy, float *zfar, float *znear) {
    int h = subpx_find(sxy);
    if (h < 0 || subpx_tab[h].izr < 0) return 0;
    *zfar = subpx_tab[h].izr; *znear = subpx_tab[h].izrn;
    return 1;
}
/* RUGRATS_ZPICK=0: old rule (a shared pixel always takes the farther room depth) */
static int zpick(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_ZPICK"); z = e ? atoi(e) : 1; } return z; }
/* RUGRATS_NEARPLANE=0: old rule (room pieces with corners clamped near the camera write no depth), see exec_poly */
static int nearplane(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_NEARPLANE"); z = e ? atoi(e) : 1; } return z; }
/* RUGRATS_MIDNEAR=0: old rule (split midpoints only get the far room depth of their ends), see subpx_mid */
static int midnear(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_MIDNEAR"); z = e ? atoi(e) : 1; } return z; }
/* m = the game's 2D midpoint of screen points a and b (polygon subdivision): give it their depth. 1/z is linear in
   screen space, so the average is exact. Depth only, no sub-pixel position (unchanged from before). */
/* Perspective split (opt.perspective, RUGRATS_PERSPMID=0: off): the game's 2D split gives the midpoint the average
   texture coordinate of its ends, which only matches the flat (affine) PS1 mapping. With perspective textures that
   texture coordinate belongs to the 3D midpoint of the edge, which projects further from the closer end (the near half of an edge covers more of the screen):
   P = A + (B - A) * wa / (wa + wb) on the same screen edge (w = 4096/z), depth 2 wa wb / (wa + wb). Without it the
   pieces of a split wall disagreed with their unsplit neighbours: when the camera turned far enough for the game to
   split the near wall, its bricks and cap jumped out of line at the split (mini golf, 2026-11-11). */
static int perspmid(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_PERSPMID"); z = e ? atoi(e) : 1; } return z; }
/* 1/256 px offset from the game's midpoint pixel mx to A + (B - A) * t (ends at whole pixel + 1/256 fraction) */
static inline int mid_off(int ax, int fa, int bx, int fb, int mx, double t) {
    const double pa = ax * 256.0 + fa, pb = bx * 256.0 + fb;
    return (int)floor(pa + (pb - pa) * t - mx * 256.0 + 0.5);
}
#define MID_OK(x, y) ((x) > -16000 && (x) < 16000 && (y) > -16000 && (y) < 16000)
/* e[3] = { has, fx, fy }: an end's exact position when the caller knows it (NULL: look it up by pixel); out (may be
   NULL) gets the midpoint's own { has, fx, fy } */
static int np_clamped(uint32_t key);
/* za_in / zb_in: the ends' exact depths (> 0) when known, zout (may be NULL): the midpoint's depth (-1 none) */
void subpx_mid_ex(uint32_t a, uint32_t b, uint32_t m, const int *ea, const int *eb, int *out, float za_in, float zb_in,
                  float *zout) {
    float za, zb;
    if (out) out[0] = 0;
    if (zout) *zout = -1.0f;
    /* exact position (RUGRATS_MIDSUB=0: old): the game rounds the midpoint to a whole PS1 pixel, which moves a split
       edge up to half a pixel off the same edge of the unsplit neighbour; at hi-res that opened a crack (a thin seam
       down the pillar wall by the house boxes). The midpoint gets the average of its ends as the rasterizer places
       them (sub-pixel where they have one), so the split edge lies exactly on the original. */
    static int midsub = -1;
    if (midsub < 0) { const char *e = getenv("RUGRATS_MIDSUB"); midsub = e ? atoi(e) : 1; }
    /* where along the edge (0.5 = 2D midpoint; perspective: the 3D midpoint, from the ends' depths) */
    double t = 0.5;
    int pm = 0;
    {
        float wa, wb;
        const int known = za_in > 0 && zb_in > 0;
        if (known) { wa = za_in; wb = zb_in; }
        /* not when an end is clamped at the GTE screen limit: its drawn position is not where it projects, the 3D
           midpoint would land far off that edge (holes near the camera); the 2D split stays on the drawn edge */
        if (opt.perspective && perspmid() && midsub && !np_clamped(a) && !np_clamped(b) &&
            (known || (subpx_z(a, DCLS_ROOM, &wa) && subpx_z(b, DCLS_ROOM, &wb)) || (subpx_z(a, DCLS_OBJ, &wa) && subpx_z(b, DCLS_OBJ, &wb))) &&
            wa > 0 && wb > 0) {
            /* optional fade to the 2D midpoint for walls crossing the lens (one end many times nearer):
               RUGRATS_PERSPMIDR=<full up to ratio>,<2D from ratio>. Off by default: the "holes" this was made for
               turned out to be real sky over the wall top (user, 2026-11-11), and the fade itself bent near walls. */
            static double r0 = -1, r1;   /* RUGRATS_PERSPMIDR=<full up to>,<2D from> */
            if (r0 < 0) { r0 = 1e9; r1 = 2e9; const char *e = getenv("RUGRATS_PERSPMIDR"); if (e) sscanf(e, "%lf,%lf", &r0, &r1); if (r1 <= r0) r1 = r0 + 0.01; }
            const double r = wa > wb ? wa / wb : wb / wa;
            const double kf = r <= r0 ? 1.0 : r >= r1 ? 0.0 : (r1 - r) / (r1 - r0);
            t = 0.5 + (wa / (double)(wa + wb) - 0.5) * kf;
            pm = kf > 0;
        }
    }
    const int ax = sext11(a), ay = sext11(a >> 16), bx = sext11(b), by = sext11(b >> 16);
    const int mx = sext11(m), my = sext11(m >> 16);
    if (midsub) {
        int fa = 0, ga = 0, fb = 0, gb = 0;
        /* 0 when an end has none; an unsure end: a room point's, else an object point's position (see fxc) */
        if (ea) { fa = ea[0] ? ea[1] : 0; ga = ea[0] ? ea[2] : 0; }
        else if (!subpx_get(a, &fa, &ga) && !subpx_getc(a, 0, -1.0f, &fa, &ga)) subpx_getc(a, 1, -1.0f, &fa, &ga);
        if (eb) { fb = eb[0] ? eb[1] : 0; gb = eb[0] ? eb[2] : 0; }
        else if (!subpx_get(b, &fb, &gb) && !subpx_getc(b, 0, -1.0f, &fb, &gb)) subpx_getc(b, 1, -1.0f, &fb, &gb);
        const int fx = mid_off(ax, fa, bx, fb, mx, t), fy = mid_off(ay, ga, by, gb, my, t);   /* 1/256 px from m */
        /* exact position for the tags at any distance (near the camera split points move hundreds of pixels; a 62 px
           limit dropped them in single frames and the wall texture jumped); the 16-bit pixel table only within MID_OK */
        if (out && fx > -(1 << 22) && fx < (1 << 22) && fy > -(1 << 22) && fy < (1 << 22)) { out[0] = 1; out[1] = fx; out[2] = fy; }
        if (MID_OK(fx, fy)) subpx_put(m, 1, fx, fy, -1.0f, DCLS_NONE);
    }
    /* depth of the point: 1/z is linear in screen space -> the average at the 2D midpoint, the harmonic mean at the
       3D one */
#define MID_Z(p, q) ((float)((p) + t * ((q) - (p))))   /* 1/z is linear in screen space: its value at the point */
    const unsigned keep = subpx_cur_oid;
    const uint32_t ab[2] = { a, b };
    const unsigned oc = subpx_common_oid(ab, 2);
    subpx_cur_oid = oc ? oc : 0xFFFF;   /* split of one model's edge: same model */
    for (int c = DCLS_ROOM; c <= DCLS_OBJ; c++)
        if (subpx_z(a, c, &za) && subpx_z(b, c, &zb)) {
            if (c == DCLS_OBJ && oc && oidz()) { subpx_oid_z(a, oc, &za); subpx_oid_z(b, oc, &zb); }   /* the model's own depths */
            if (c == DCLS_ROOM && za_in > 0 && zb_in > 0) { za = za_in; zb = zb_in; }   /* exact ends (tags) */
            subpx_put(m, 0, 0, 0, MID_Z(za, zb), c);
        }
    if (zout && za_in > 0 && zb_in > 0) *zout = MID_Z(za_in, zb_in);
    /* an end with two room depths (another room point on its pixel): the midpoint gets the near average too, so it
       is just as unsure (exec_poly: write-notest). With only the far one, a split wall piece next to the house stairs
       got a depth far behind the handrail there, and the handrail showed through it (room over object rule). */
    float na, nb;
    if (subpx_zr(a, &za, &na) && subpx_zr(b, &zb, &nb) && (na != za || nb != zb) && midnear())
        subpx_put(m, 0, 0, 0, MID_Z(na, nb), DCLS_ROOM);
    subpx_cur_oid = keep;
    if (subpx_isworld(a) || subpx_isworld(b)) subpx_world(m);
    const int h = subpx_find(m);   /* remember the ends (nearplane: a split piece's corner leads back to the wall) */
    if (h >= 0) {
        subpx_tab[h].mk = 1; subpx_tab[h].ma = a; subpx_tab[h].mb = b;   /* newest wins (checks in exec_poly) */
        if (midsub) for (int c = 0; c < 2; c++) {   /* per-kind position too (see fxc), from the ends' own kind */
            float ta, tb;
            if (!subpx_z(a, c ? DCLS_OBJ : DCLS_ROOM, &ta) || !subpx_z(b, c ? DCLS_OBJ : DCLS_ROOM, &tb)) continue;
            int fa = 0, ga = 0, fb = 0, gb = 0;
            if (!subpx_getc(a, c, -1.0f, &fa, &ga)) subpx_get(a, &fa, &ga);   /* room: the ends' far points (their */
            if (!subpx_getc(b, c, -1.0f, &fb, &gb)) subpx_get(b, &fb, &gb);   /* depth is the one put above) */
            int slot = c;
            if (c == 0) {   /* the room slot whose depth is that one */
                const float mz = MID_Z(ta, tb);
                slot = fabsf(mz - subpx_tab[h].izrn) < fabsf(mz - subpx_tab[h].izr) ? 2 : 0;
            }
            const int fx = mid_off(ax, fa, bx, fb, mx, t), fy = mid_off(ay, ga, by, gb, my, t);
            if (MID_OK(fx, fy)) subpx_putc(h, slot, fx, fy);
            if (c == 1 && oc && subpx_tab[h].oid[0] != 0xFFFF) {   /* split of one model's edge: that model's position */
                int pa = 0, qa = 0, pb = 0, qb = 0;
                if (!subpx_getid(a, oc, &pa, &qa)) subpx_get(a, &pa, &qa);
                if (!subpx_getid(b, oc, &pb, &qb)) subpx_get(b, &pb, &qb);
                const int gx = mid_off(ax, pa, bx, pb, mx, t), gy = mid_off(ay, qa, by, qb, my, t);
                if (MID_OK(gx, gy))
                    for (int i = 0; i < SUBPX_OIDS; i++) if (subpx_tab[h].oid[i] == oc) subpx_oidfx(h, i, gx, gy);
            }
        }
    }
#undef MID_Z
}
void subpx_mid(uint32_t a, uint32_t b, uint32_t m) { subpx_mid_ex(a, b, m, NULL, NULL, NULL, -1.0f, -1.0f, NULL); }
/* Split points carry exact positions too (see subpx_tag): the room renderer's split (hooks.c) leaves its midpoints in
   registers and the caller stores them into a work buffer, then into packets. The hook computes each midpoint from
   its ends' exact positions (register tags of the caller's loads, else earlier midpoints, else the pixel table) and
   tags the registers, so the copies keep it. Two game frames per refresh put slightly different midpoints on one
   pixel; by pixel they were "unsure" and the point snapped back (the perspective split moves them far). */

/* Exact texture coordinates of split points (RUGRATS_SPLITUV=0: off). The game gives a split point the rounded-down
   average texture coordinate of its ends ((a+b)>>1, 80011a28), up to half a texel off its true one, and nested
   splits add up. Up close a texel is ~10 screen pixels, and the game re-splits differently as the camera moves, so
   the wall texture flickered by that much (mini golf hedge wall, 2026-11-11). The texture split's hook keeps the
   exact fraction (1/256 texel) per split point position; exec_poly adds it with perspective textures. */
/* Keyed by position AND the game's rounded uv word (RUGRATS_SPLITQ=1): neighbouring quads split their shared edge
   at the same point with different texture coordinates (mirrored wall tiles), and a position-only key let the second
   overwrite the first, so pieces mixed exact and rounded corners. */
static struct { uint32_t val, gen; int16_t uf, vf; uint16_t word; } uvf_tab[16384];   /* word: the game's rounded uv */
static int splitq(void);
static inline uint32_t uvf_h(uint32_t v, uint32_t word) {
    if (!splitq()) word = 0;
    return ((v ^ (word * 0x9E3779B1u)) * 2654435761u) >> 18;
}
/* RUGRATS_PWTAG=0: perspective weights from the pixel table's depth (old) instead of the tagged exact depth */
static int pwtag(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_PWTAG"); z = e ? atoi(e) : 1; } return z; }
static int splituv(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_SPLITUV"); z = e ? atoi(e) : 1; } return z; }
/* 4-way buckets: two frames of split points share the table, and single slots lost entries to unrelated points */
static int uvf_find(uint32_t pos, uint32_t word) {
    const uint32_t h = uvf_h(pos, word) & ~3u;
    for (int j = 0; j < 4; j++) {
        const uint32_t e = h + j;
        if (uvf_tab[e].val == pos && subpx_gen - uvf_tab[e].gen <= 1 && (!splitq() || uvf_tab[e].word == (uint16_t)word))
            return (int)e;
    }
    return -1;
}
static int uvf_get(uint32_t pos, uint32_t word, int *uf, int *vf) {
    const int e = uvf_find(pos, word);
    if (e < 0) return 0;
    *uf = uvf_tab[e].uf; *vf = uvf_tab[e].vf; return 1;
}
static void uvf_put(uint32_t pos, int uf, int vf, uint32_t word) {
    int e = uvf_find(pos, word);
    if (e < 0) {   /* a free / stale slot of the bucket, else its oldest */
        const uint32_t h = uvf_h(pos, word) & ~3u;
        e = (int)h;
        for (int j = 0; j < 4; j++) {
            if (subpx_gen - uvf_tab[h + j].gen > 1 || !uvf_tab[h + j].val) { e = (int)(h + j); break; }
            if ((int32_t)(uvf_tab[h + j].gen - uvf_tab[e].gen) < 0) e = (int)(h + j);
        }
    }
    uvf_tab[e].val = pos; uvf_tab[e].gen = subpx_gen; uvf_tab[e].uf = (int16_t)uf; uvf_tab[e].vf = (int16_t)vf;
    uvf_tab[e].word = (uint16_t)word;
}
typedef struct { int n, nm, ok; uint32_t pos[4], mpos[5]; int mreg[5]; uint32_t key, seq; } SplitRec;
/* Position splits waiting for their texture split. A near wall is split in two levels (4x4 pieces): the game runs
   ALL position splits first (parent, then its four children) and then all texture splits in the same order. Keeping
   only the last position split paired the parent's texture split with the last child's points and dropped the other
   four: corners kept the game's rounded-down texels and straight wall edges bent near the camera (space station
   panels, 2026-11-13). Both splits of one quad run with r2 = its primitive in the packet buffer (the parent and its
   first child share one), so a texture split takes the OLDEST waiting position split with the same r2; position
   splits that never get a texture split (other paths) just age out. RUGRATS_SPLITQ=0 = old (last only). */
#define SPLITQ_N 64
static SplitRec split_q[SPLITQ_N];
static uint32_t split_seq;
static SplitRec last_split;      /* the one being filled / consumed */
static int splitq(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_SPLITQ"); z = e ? atoi(e) : 1; } return z; }
static int end_pos(uint32_t src, uint32_t val, int e[3]) {
    int fx, fy;
    tag_iz_out = -1.0f; tp_out.ok = 0;
    if (src >= 0xFFFFFF00u) {
        const int r = src & 31;
        if (subpx_rg[r].val == val && subpx_gen - subpx_rg[r].gen <= 1) {
            e[0] = subpx_rg[r].has & 1; e[1] = subpx_rg[r].fx; e[2] = subpx_rg[r].fy; tag_iz_out = subpx_rg[r].iz; tp_out = subpx_rg[r].p3; return 1;
        }
    } else if (src) {
        const int t = subpx_tag_get(src, val, &fx, &fy);
        if (t >= 0) { e[0] = t & 1; e[1] = t ? fx : 0; e[2] = t ? fy : 0; return 1; }
    }
    return mid_cache_get(val, e);
}
/* every split point of this / the last frame by its packed screen value (newest wins). Untagged copies of a split
   point (paths the tag tracking misses) find it here before the pixel table. */
static struct { uint32_t val, gen; int32_t fx, fy; uint8_t has; float iz; TagP p3; } mid_h[8192];
static inline uint32_t mid_hh(uint32_t v) { return (v * 2654435761u) >> 19; }
static int midcache(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_MIDCACHE"); z = e ? atoi(e) : 1; } return z; }
static int mid_cache_get(uint32_t val, int e[3]) {
    if (!midcache()) return 0;
    const uint32_t h = mid_hh(val);
    if (mid_h[h].val != val || subpx_gen - mid_h[h].gen > 1) return 0;
    e[0] = mid_h[h].has; e[1] = mid_h[h].fx; e[2] = mid_h[h].fy; tag_iz_out = mid_h[h].iz; tp_out = mid_h[h].p3; return 1;
}
static void mid_reg(int r, uint32_t val, const int o[3], float iz, const TagP *p3) {
    tag_iz_in = iz; tp_in = *p3;
    subpx_tag_reg(r, val, o[0] | 2, o[1], o[2]);   /* bit 1: a split point (texture fraction, uvf_get) */
    tag_ld[r] = 0xFFFFFF00u | (uint32_t)r; tag_lpc[r] = 0;
    const uint32_t h = mid_hh(val);
    mid_h[h].val = val; mid_h[h].gen = subpx_gen; mid_h[h].has = (uint8_t)o[0];
    mid_h[h].fx = o[1]; mid_h[h].fy = o[2]; mid_h[h].iz = iz; mid_h[h].p3 = *p3;
}
/* v[]: the split's corners (s1..s4 before the call), src[]: their tag_ld; mids: register numbers and their two ends
   (index into v, or 4 + earlier mid index) */
/* texture split of the quad just split (80011a28): uv[4] = corners' texture words (s1..s4); results in t1..t4/t9 */
void subpx_split_uv(const uint32_t *uv) {
    if (splitq()) {   /* the oldest waiting position split of this primitive (r2) */
        int best = -1;
        for (int j = 0; j < SPLITQ_N; j++)
            if (split_q[j].ok && split_q[j].key == cpu.r[2] && split_seq - split_q[j].seq < SPLITQ_N &&
                (best < 0 || split_q[j].seq < split_q[best].seq)) best = j;
        if (best < 0) {   /* r2 changed in between: the position split just before, if unused (one-level path) */
            best = (int)(split_seq % SPLITQ_N);
            if (!split_q[best].ok || split_q[best].seq != split_seq) return;
        }
        last_split = split_q[best]; split_q[best].ok = 0;
    }
    if (!last_split.ok || last_split.n != 4 || last_split.nm != 5 || !splituv()) return;
    last_split.ok = 0;
    int cu[4], cv[4];   /* corners' exact coordinate in 1/256 texel */
    for (int k = 0; k < 4; k++) {
        int fu = 0, fv = 0;
        uvf_get(last_split.pos[k], uv[k] & 0xFFFF, &fu, &fv);   /* an earlier split point: its own fraction */
        cu[k] = (int)(uv[k] & 0xFF) * 256 + fu; cv[k] = (int)((uv[k] >> 8) & 0xFF) * 256 + fv;
    }
    /* position split order: t1 = ab, t4 = ac, t3 = cd, t2 = bd, t9 = centre (of all four) */
    static const int ends[4][2] = { { 0, 1 }, { 0, 2 }, { 2, 3 }, { 1, 3 } };
    for (int i = 0; i < 5; i++) {
        const uint32_t w = cpu.r[last_split.mreg[i]];
        int eu, ev;
        if (i < 4) { eu = (cu[ends[i][0]] + cu[ends[i][1]]) / 2; ev = (cv[ends[i][0]] + cv[ends[i][1]]) / 2; }
        else { eu = (cu[0] + cu[1] + cu[2] + cu[3]) / 4; ev = (cv[0] + cv[1] + cv[2] + cv[3]) / 4; }
        const int du = eu - (int)(w & 0xFF) * 256, dv = ev - (int)((w >> 8) & 0xFF) * 256;
        if (du > -512 && du < 512 && dv > -512 && dv < 512) uvf_put(last_split.mpos[i], du, dv, w & 0xFFFF);
    }
}
void subpx_split(int n, const uint32_t *v, const uint32_t *src, int nm, const int (*mids)[3]) {
    int e[12][3], k = 0;
    uint32_t val[12];
    float ez[12];   /* exact depths (tags), -1 unknown */
    TagP ep[12];    /* camera-space points (tags; split points: average of their ends) */
    const int tg = subpx_tag_on();
    for (k = 0; k < n; k++) {
        val[k] = v[k]; ez[k] = -1.0f; ep[k].ok = 0;
        if (!tg || !end_pos(src[k], v[k], e[k])) e[k][0] = -1;
        else { ez[k] = tag_iz_out; ep[k] = tp_out; }
        if (gpu_cmdlog && (e[k][0] < 0 || ez[k] <= 0))   /* debug: split end without exact position / depth */
            rt_logf("split end %d %08x src %08x: %s\n", k, v[k], src[k], e[k][0] < 0 ? "no tag" : "no depth");
    }
    last_split.n = n; last_split.nm = nm; last_split.ok = 1;
    for (k = 0; k < n; k++) last_split.pos[k] = v[k];
    for (int i = 0; i < nm; i++) {
        const int ia = mids[i][1], ib = mids[i][2], r = mids[i][0];
        last_split.mpos[i] = cpu.r[r]; last_split.mreg[i] = r;
        const uint32_t a = val[ia], b = val[ib], m = cpu.r[r];
        int o[3] = { 0, 0, 0 };
        float zm;
        subpx_mid_ex(a, b, m, e[ia][0] >= 0 ? e[ia] : NULL, e[ib][0] >= 0 ? e[ib] : NULL, o, ez[ia], ez[ib], &zm);
        val[n + i] = m; e[n + i][0] = o[0]; e[n + i][1] = o[1]; e[n + i][2] = o[2]; ez[n + i] = zm;
        ep[n + i].ok = ep[ia].ok && ep[ib].ok;
        for (int c = 0; c < 3; c++) ep[n + i].p[c] = 0.5f * (ep[ia].p[c] + ep[ib].p[c]);
        if (gpu_cmdlog && getenv("RUGRATS_SPLITLOG"))   /* debug: every split point */
            rt_logf("SPLIT m %08x <- %08x [%d %d,%d z%.3f] + %08x [%d %d,%d z%.3f] -> %d,%d z%.3f n%d i%d\n", m, a, e[ia][0], e[ia][1],
                    e[ia][2], ez[ia], b, e[ib][0], e[ib][1], e[ib][2], ez[ib], o[1], o[2], zm, n, i);
        if (tg) mid_reg(r, m, o, zm, &ep[n + i]);
    }
    if (splitq() && n == 4 && nm == 5) {   /* only quad splits get a texture split (80011a28) */
        last_split.key = cpu.r[2]; last_split.seq = ++split_seq;
        split_q[split_seq % SPLITQ_N] = last_split;
    }
}

static int np_clamped(uint32_t key) {
    const int x = ((int32_t)(key << 21)) >> 21, y = ((int32_t)(key << 5)) >> 21;
    return x <= -1024 || x >= 1023 || y <= -1024 || y >= 1023;
}
/* camera-space position of a room vertex the GTE projected onto this pixel (nearplane) */
void subpx_put3(uint32_t sxy, double x, double y, double z) {
    const int h = subpx_find(sxy);
    if (h < 0) return;
    /* newest wins (a table generation can hold 2-3 game frames above 60 fps); c3 = 2 counts a different point on the
       same pixel, which only matters on clamped pixels (-1024 / 1023: many walls' corners land there) */
    if (subpx_tab[h].c3 && (fabs(subpx_tab[h].cx - x) > 0.5 || fabs(subpx_tab[h].cy - y) > 0.5 || fabs(subpx_tab[h].cz - z) > 0.5))
        subpx_tab[h].c3 = 2;
    else if (!subpx_tab[h].c3) subpx_tab[h].c3 = 1;
    subpx_tab[h].cx = (float)x; subpx_tab[h].cy = (float)y; subpx_tab[h].cz = (float)z;
    if (np_clamped(sxy)) {   /* clamped pixel: keep the newest 4 different points (wall corners landing here) */
        for (int i = 0; i < subpx_tab[h].ccn; i++)
            if (fabs(subpx_tab[h].cc[i][0] - x) < 0.5 && fabs(subpx_tab[h].cc[i][1] - y) < 0.5 && fabs(subpx_tab[h].cc[i][2] - z) < 0.5) return;
        if (subpx_tab[h].ccn == 4) {   /* full: drop the oldest (the newest belong to the frame being drawn) */
            memmove(subpx_tab[h].cc[0], subpx_tab[h].cc[1], 3 * sizeof subpx_tab[h].cc[0]);
            subpx_tab[h].ccn = 3;
        }
        float *c = subpx_tab[h].cc[subpx_tab[h].ccn++];
        c[0] = (float)x; c[1] = (float)y; c[2] = (float)z;
    }
}
/* wall points behind a polygon corner: its own camera-space position, else (split midpoint) its two ends' points */
#define NP_MAX 16
/* mode bit 0: clamped pixels never count; bit 1: a split midpoint follows its ends even when some other vertex was
   projected onto the same pixel (its own point may belong to another wall). Tried in order until the set is flat. */
static int np_grp[2], np_ngrp;   /* clamped pixels with several candidate points (tried one by one in exec_poly) */
static int np_collect(uint32_t key, double P[][3], int *n, int depth, int mode) {
    const int h = subpx_find(key);
    if (h < 0) return 0;
    const int cl = np_clamped(key);
    if (cl && (mode & 1)) return 1;   /* clamped pixels left out */
    if (cl && subpx_tab[h].c3 == 2) {   /* several walls' corners here: a group of candidates */
        for (int i = 0; i < np_ngrp; i++) if (np_grp[i] == h) return 1;
        if (np_ngrp < 2) np_grp[np_ngrp++] = h;
        return 1;
    }
    const int mid = subpx_tab[h].mk == 1 && depth < 8;
    if (subpx_tab[h].c3 && !(mid && (mode & 2))) {
        const double q[3] = { subpx_tab[h].cx, subpx_tab[h].cy, subpx_tab[h].cz };
        for (int i = 0; i < *n; i++)
            if (fabs(P[i][0] - q[0]) < 0.5 && fabs(P[i][1] - q[1]) < 0.5 && fabs(P[i][2] - q[2]) < 0.5) return 1;
        if (*n >= NP_MAX) return 1;
        P[*n][0] = q[0]; P[*n][1] = q[1]; P[*n][2] = q[2]; (*n)++;
        return 1;
    }
    if (!mid) return 0;
    const uint32_t a = subpx_tab[h].ma, b = subpx_tab[h].mb;
    return np_collect(a, P, n, depth + 1, mode) && np_collect(b, P, n, depth + 1, mode);
}
/* Exact texture mapping for a wall piece with a clamped corner (RUGRATS_CLAMPUV=0: affine, see clampaff). A clamped
   corner is drawn at the screen limit, but its texture coordinate is its vertex's, which projects elsewhere (or is
   behind the camera); the game's split points of such an edge sit at 2D midpoints of the wrong positions. Each corner
   carries its true camera-space point in the tags (GTE x y z; split points the 3D midpoint of their ends, which is
   where their averaged texture coordinate belongs). Three of them give the wall's plane and its texture layout
   (uv is affine on the plane); each DRAWN corner gets the plane point its pixel shows: uv there and w = 1/z. The
   perspective interpolation of those is exact over the whole piece, so all pieces of the wall agree. Only u / v stay
   the game's (they keep the texel clamp box); the correction goes into uf / vf. Returns 0 (affine) when a corner has no
   point, the points are on a line, or a corner's ray misses the plane in front of the camera. */
void gte_proj_params(double *ox, double *oy, double *h, double *w);
static int clampuv(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_CLAMPUV"); z = e ? atoi(e) : 1; } return z; }
static int clamp_uv(Vtx *v, int nv, const TagP *cp) {
    for (int k = 0; k < nv; k++) if (!cp[k].ok) return 0;
    double P[4][3], U[4], V[4];
    for (int k = 0; k < nv; k++) {
        for (int c = 0; c < 3; c++) P[k][c] = cp[k].p[c];
        U[k] = v[k].u + v[k].uf; V[k] = v[k].v + v[k].vf;
    }
    int bi = -1, bj = 0, bk = 0;
    double best = 0, nrm[3] = { 0, 0, 0 };
    for (int i = 0; i < nv; i++) for (int j = i + 1; j < nv; j++) for (int k = j + 1; k < nv; k++) {
        const double a[3] = { P[j][0] - P[i][0], P[j][1] - P[i][1], P[j][2] - P[i][2] };
        const double b[3] = { P[k][0] - P[i][0], P[k][1] - P[i][1], P[k][2] - P[i][2] };
        const double c[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
        const double l = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (l > best) { best = l; bi = i; bj = j; bk = k; nrm[0] = c[0] / l; nrm[1] = c[1] / l; nrm[2] = c[2] / l; }
    }
    if (bi < 0 || best < 64.0) return 0;
    const double e1[3] = { P[bj][0] - P[bi][0], P[bj][1] - P[bi][1], P[bj][2] - P[bi][2] };
    const double e2[3] = { P[bk][0] - P[bi][0], P[bk][1] - P[bi][1], P[bk][2] - P[bi][2] };
    const double a11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], a12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
    const double a22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2], det = a11 * a22 - a12 * a12;
    if (fabs(det) < 1e-6) return 0;
    const double d = nrm[0] * P[bi][0] + nrm[1] * P[bi][1] + nrm[2] * P[bi][2];
    double ox, oy, hh, ww;
    gte_proj_params(&ox, &oy, &hh, &ww);
    double nu[4], nvv[4], nw[4], lo = 1e30, hi = 0;
    for (int m = 0; m < nv; m++) {
        const double sx = v[m].x - g.offx + v[m].fx / 256.0, sy = v[m].y - g.offy + v[m].fy / 256.0;
        const double dir[3] = { (sx - ox) / (hh * ww), (sy - oy) / hh, 1.0 };
        const double den = nrm[0] * dir[0] + nrm[1] * dir[1] + nrm[2] * dir[2];
        if (fabs(den) < 1e-9) return 0;
        const double t = d / den;   /* = camera-space z of the plane point */
        if (t < 8.0) return 0;
        const double r[3] = { t * dir[0] - P[bi][0], t * dir[1] - P[bi][1], t - P[bi][2] };
        const double b1 = r[0] * e1[0] + r[1] * e1[1] + r[2] * e1[2], b2 = r[0] * e2[0] + r[1] * e2[1] + r[2] * e2[2];
        const double s1 = (a22 * b1 - a12 * b2) / det, s2 = (a11 * b2 - a12 * b1) / det;
        nu[m] = U[bi] + s1 * (U[bj] - U[bi]) + s2 * (U[bk] - U[bi]);
        nvv[m] = V[bi] + s1 * (V[bj] - V[bi]) + s2 * (V[bk] - V[bi]);
        if (fabs(nu[m]) > 4096 || fabs(nvv[m]) > 4096) return 0;
        nw[m] = 4096.0 / t;
        if (nw[m] < lo) lo = nw[m];
        if (nw[m] > hi) hi = nw[m];
    }
    if (hi > 64.0 * lo) return 0;
    for (int m = 0; m < nv; m++) {
        v[m].uf = (float)(nu[m] - v[m].u); v[m].vf = (float)(nvv[m] - v[m].v); v[m].pw = (float)nw[m];
    }
    if (gpu_cmdlog) {
        rt_logf("   clamp_uv:");
        for (int m = 0; m < nv; m++) rt_logf(" %.2f,%.2f w%.3f", nu[m], nvv[m], nw[m]);
        rt_logf("\n");
    }
    return 1;
}
/* a corner at the GTE screen limit, or a split point with such a corner among its ends (any depth of nesting) */
static int np_fromclamp(uint32_t key, int depth) {
    if (np_clamped(key)) return 1;
    const int h = subpx_find(key);
    if (h < 0 || subpx_tab[h].mk != 1 || depth >= 8) return 0;
    return np_fromclamp(subpx_tab[h].ma, depth + 1) || np_fromclamp(subpx_tab[h].mb, depth + 1);
}
/* RUGRATS_CLAMPAFFINE=0: perspective textures also on pieces with clamped corners (old, see exec_poly) */
static int clampaff(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_CLAMPAFFINE"); z = e ? atoi(e) : 1; } return z; }
/* plane n.P = d through the points (largest triangle), all points on it within 4 units + 0.1% of distance */
static int np_plane(double P[][3], int np, double nrm[3], double *d) {
    double best = 0;
    for (int i = 0; i < np; i++) for (int j = i + 1; j < np; j++) for (int k = j + 1; k < np; k++) {
        const double u[3] = { P[j][0] - P[i][0], P[j][1] - P[i][1], P[j][2] - P[i][2] };
        const double w[3] = { P[k][0] - P[i][0], P[k][1] - P[i][1], P[k][2] - P[i][2] };
        const double c[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
        const double l = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (l > best) { best = l; nrm[0] = c[0] / l; nrm[1] = c[1] / l; nrm[2] = c[2] / l; *d = nrm[0] * P[i][0] + nrm[1] * P[i][1] + nrm[2] * P[i][2]; }
    }
    if (best < 64.0) return 0;   /* points (nearly) on one line */
    for (int i = 0; i < np; i++) {
        const double e = fabs(nrm[0] * P[i][0] + nrm[1] * P[i][1] + nrm[2] * P[i][2] - *d);
        const double r = sqrt(P[i][0] * P[i][0] + P[i][1] * P[i][1] + P[i][2] * P[i][2]);
        if (e > 4.0 + 0.001 * r) return 0;   /* GTE camera-space points are exact to ~1 unit; mixed frames fail */
    }
    return fabs(*d) > 1e-6;
}
/* 1/z of the plane at GTE screen position sx, sy, as the depth table stores it (4096 / z) */
void gte_proj_params(double *ox, double *oy, double *h, double *w);
/* RUGRATS_NEARPOS=1: experimental, OFF by default (split pieces of a wall at the lens get their corners re-projected,
   see np_point3). It never showed a gain, and on the mini golf hedge next to Angelica it moved pieces in single
   frames: the hedge opened a hole (pool behind it showed) and the wall cap bent while the camera turned (2026-11-11). */
static int nearpos(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_NEARPOS"); z = e ? atoi(e) : 0; } return z; }
static int np_onplane(const double p[3], const double nrm[3], double d) {
    const double r = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    return fabs(nrm[0] * p[0] + nrm[1] * p[1] + nrm[2] * p[2] - d) <= 4.0 + 0.001 * r;
}
/* camera-space point behind a polygon corner on the wall plane: its own GTE point, a clamped pixel's candidate on the
   plane, or (split midpoint) the 3D midpoint of its two ends. *cl is set when the chain went through a clamped pixel:
   then the game's 2D position is wrong (midpoint of a clamped corner) and only the 3D point gives the right one. */
static int np_point3(uint32_t key, const double nrm[3], double d, double p[3], int *cl, int depth) {
    const int h = subpx_find(key);
    if (h < 0 || depth > 10) return 0;
    if (np_clamped(key)) {
        for (int i = 0; i < subpx_tab[h].ccn; i++) {
            const double q[3] = { subpx_tab[h].cc[i][0], subpx_tab[h].cc[i][1], subpx_tab[h].cc[i][2] };
            if (np_onplane(q, nrm, d)) { p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; *cl = 1; return 1; }
        }
        return 0;
    }
    if (subpx_tab[h].mk == 1) {
        double a[3], b[3];
        if (np_point3(subpx_tab[h].ma, nrm, d, a, cl, depth + 1) && np_point3(subpx_tab[h].mb, nrm, d, b, cl, depth + 1)) {
            p[0] = (a[0] + b[0]) * 0.5; p[1] = (a[1] + b[1]) * 0.5; p[2] = (a[2] + b[2]) * 0.5;
            return 1;
        }
    }
    if (subpx_tab[h].c3 == 1) {
        const double q[3] = { subpx_tab[h].cx, subpx_tab[h].cy, subpx_tab[h].cz };
        if (np_onplane(q, nrm, d)) { p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; return 1; }
    }
    return 0;
}
static double np_iz(const double nrm[3], double d, int sx, int sy) {
    double ox, oy, h, w;
    gte_proj_params(&ox, &oy, &h, &w);
    return 4096.0 * (nrm[0] * (sx - ox) / (h * w) + nrm[1] * (sy - oy) / h + nrm[2]) / d;
}

static uint32_t poly_seq;   /* polygon number in the frame (debug: GP0 log "#n", RUGRATS_POLYID colours) */
static int poly_id_vis = -1;
static void tri_t(const St *st, const Tgt *T, int band, int nb, Vtx a, Vtx b, Vtx c, int textured, int gouraud,
                  int semi_on, int raw, int clutx, int cluty, int tpx, int tpy, int depth) {
    int64_t area = (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(c.x - a.x) * (b.y - a.y);
    /* zero area at whole native pixels: the PS1 draws nothing. Not for hi-res-only pieces (grown / reprojected, zs > 0):
       thin pieces of those land on collinear native pixels while they cover hi-res pixels (1-pixel slits in the grass,
       CPU renderer only; the GPU renderer has no such check) */
    if (area == 0 && !(a.zs > 0 && T->s > 1)) return;
    if (area < 0) { Vtx t = b; b = c; c = t; area = -area; }
    int minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    const int s = T->s;
    if (s > 1) { /* same triangle in shadow pixels, with the GTE's sub-pixel positions when known */
        #define SUB(f) (((f) * s + 128) >> 8)
        a.x = a.x * s + SUB(a.fx); a.y = a.y * s + SUB(a.fy);
        b.x = b.x * s + SUB(b.fx); b.y = b.y * s + SUB(b.fy);
        c.x = c.x * s + SUB(c.fx); c.y = c.y * s + SUB(c.fy);
        #undef SUB
        area = (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(c.x - a.x) * (b.y - a.y);
        if (area == 0) return;
        if (area < 0) { Vtx t = b; b = c; c = t; area = -area; }
        minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
        maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
        miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
        maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    }
    static int dbx = -2, dby;   /* debug: RUGRATS_TRIDBG=<hi-res x>,<y>: triangles touching that shadow pixel */
    if (dbx == -2) { const char *e = getenv("RUGRATS_TRIDBG"); dbx = -1; if (e) sscanf(e, "%d,%d", &dbx, &dby); }
    const int dbg = dbx >= 0 && T == &hr && dbx >= minx && dbx <= maxx && dby >= miny && dby <= maxy;
    if (dbg) rt_logf("tri_t dbg seq %u a %d,%d b %d,%d c %d,%d uv %d,%d %d,%d %d,%d\n", a.seq, a.x, a.y, b.x, b.y, c.x, c.y, a.u, a.v, b.u, b.v, c.u, c.v);
    if (minx < st->dx1 * s) minx = st->dx1 * s;
    if (miny < st->dy1 * s) miny = st->dy1 * s;
    if (maxx > st->dx2 * s + s - 1) maxx = st->dx2 * s + s - 1;
    if (maxy > st->dy2 * s + s - 1) maxy = st->dy2 * s + s - 1;
    if (minx > maxx || miny > maxy) return;
    int dith = st->dither && (gouraud || (textured && !raw));
    if (T == &nat && !nat_nocost) cpu.cycles += (uint32_t)((maxx - minx + 1) * (maxy - miny + 1)) / 8;

    /* edge functions, stepped incrementally; top-left rule: skip pixels exactly on edges that are not top/left */
    #define EDGE(p, q, X, Y) ((int64_t)((q).x - (p).x) * ((Y) - (p).y) - (int64_t)((q).y - (p).y) * ((X) - (p).x))
    int bias0 = (c.y - b.y < 0 || (c.y == b.y && c.x - b.x > 0)) ? 0 : -1;
    int bias1 = (a.y - c.y < 0 || (a.y == c.y && a.x - c.x > 0)) ? 0 : -1;
    int bias2 = (b.y - a.y < 0 || (b.y == a.y && b.x - a.x > 0)) ? 0 : -1;
    const int64_t sx0 = -(int64_t)(c.y - b.y), sx1 = -(int64_t)(a.y - c.y), sx2 = -(int64_t)(b.y - a.y);
    const int64_t sy0 = (int64_t)(c.x - b.x), sy1 = (int64_t)(a.x - c.x), sy2 = (int64_t)(b.x - a.x);
    /* texel rounding: native rounds to nearest; at s x it must still pick one texel per native pixel step */
    const double ub = 0.5 / s;
    double inv = 1.0 / (double)area;
    /* attributes are linear in x: per-pixel steps, row start computed exactly from the edge values */
    #define GRAD(A0, A1, A2) (((double)sx0 * (A0) + (double)sx1 * (A1) + (double)sx2 * (A2)) * inv)
    const double dr = GRAD(a.r, b.r, c.r), dg = GRAD(a.g, b.g, c.g), dbb = GRAD(a.b, b.b, c.b);
    const double du = GRAD(a.u, b.u, c.u), dv = GRAD(a.v, b.v, c.v);
    const int persp = textured && T == &hr && a.pw > 0 && b.pw > 0 && c.pw > 0;
    const double dw = persp ? GRAD(a.pw, b.pw, c.pw) : 0;
#define PU(q) ((q).u + (q).uf)
#define PV(q) ((q).v + (q).vf)
    const double duw = persp ? GRAD(PU(a) * a.pw, PU(b) * b.pw, PU(c) * c.pw) : 0, dvw = persp ? GRAD(PV(a) * a.pw, PV(b) * b.pw, PV(c) * c.pw) : 0;
    const int umn = a.u < b.u ? (a.u < c.u ? a.u : c.u) : (b.u < c.u ? b.u : c.u), umx = a.u > b.u ? (a.u > c.u ? a.u : c.u) : (b.u > c.u ? b.u : c.u);
    const int vmn = a.v < b.v ? (a.v < c.v ? a.v : c.v) : (b.v < c.v ? b.v : c.v), vmx = a.v > b.v ? (a.v > c.v ? a.v : c.v) : (b.v > c.v ? b.v : c.v);
    const int dm = (T == &hr && hr_z) ? a.dm : DM_NONE;
    const double dz = dm ? GRAD(a.iz, b.iz, c.iz) : 0;
    const int nearz = dm == DM_WNT && gpu_room_obj_wnt() == 2;   /* room over object with the near candidates */
    const double dzn = nearz ? GRAD(a.izn, b.izn, c.izn) : 0;
    /* room writes are pushed back by the slope bias (see gpu_depth_slope): K native pixels of their own depth change */
    double zbias = 0;
    if (dm == DM_WRITE || dm == DM_WNT) {
        const double dzy = ((double)sy0 * a.iz + (double)sy1 * b.iz + (double)sy2 * c.iz) * inv;
        zbias = (fabs(dz) > fabs(dzy) ? fabs(dz) : fabs(dzy)) * s * gpu_depth_slope();
        /* pieces of a grown / reprojected polygon carry their source triangle's slope (per native pixel, from exact
           positions): thin bevel pieces at whole hi-res pixels gave huge gradients, the room depth was pushed far back
           and the pool drawn later showed through the ground (CPU renderer only; the GPU's bias uses float positions) */
        if (a.zs > 0) zbias = a.zs * gpu_depth_slope();
    }
    #undef GRAD
    int64_t r0 = EDGE(b, c, minx, miny), r1 = EDGE(c, a, minx, miny), r2 = EDGE(a, b, minx, miny);
    for (int y = miny; y <= maxy; y++, r0 += sy0, r1 += sy1, r2 += sy2) {
        if (!OWNS(y, band, nb)) continue;
        /* covered span of this row: solve w_i + sx_i * k >= 0 for each edge */
        int64_t klo = 0, khi = maxx - minx;
        {
            const int64_t ws[3] = { r0, r1, r2 }, ss[3] = { sx0, sx1, sx2 };
            for (int e = 0; e < 3 && klo <= khi; e++) {
                int64_t w = ws[e], st_ = ss[e];
                if (st_ == 0) { if (w < 0) khi = -1; }
                else if (st_ > 0) { if (w < 0) { int64_t k = (-w + st_ - 1) / st_; if (k > klo) klo = k; } }
                else { if (w < 0) khi = -1; else { int64_t k = w / -st_; if (k < khi) khi = k; } }
            }
        }
        if (klo > khi) continue;
        int64_t w0 = r0 + sx0 * klo, w1 = r1 + sx1 * klo, w2 = r2 + sx2 * klo;
        double l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
        double fr = l0 * a.r + l1 * b.r + l2 * c.r + 0.5, fg = l0 * a.g + l1 * b.g + l2 * c.g + 0.5;
        double fb = l0 * a.b + l1 * b.b + l2 * c.b + 0.5;
        double fu = l0 * a.u + l1 * b.u + l2 * c.u + ub, fv = l0 * a.v + l1 * b.v + l2 * c.v + ub;
        double fw = 0, fuw = 0, fvw = 0;   /* perspective: uv * w and w (see exec_poly) */
        if (persp) { fw = l0 * a.pw + l1 * b.pw + l2 * c.pw; fuw = l0 * PU(a) * a.pw + l1 * PU(b) * b.pw + l2 * PU(c) * c.pw;
                     fvw = l0 * PV(a) * a.pw + l1 * PV(b) * b.pw + l2 * PV(c) * c.pw; }
        double fz = dm ? l0 * a.iz + l1 * b.iz + l2 * c.iz : 0;
        double fzn = nearz ? l0 * a.izn + l1 * b.izn + l2 * c.izn : 0;
        uint16_t *row = &T->buf[y * T->w];
        float *zrow = dm ? &hr_z[(size_t)y * T->w] : NULL;
        float *ozrow = dm ? &hr_oz[(size_t)y * T->w] : NULL;
        const int ny = s > 1 ? y / s : y; /* dither pattern stays on native pixels */
        for (int x = minx + (int)klo, xe = minx + (int)khi; x <= xe;
             x++, w0 += sx0, w1 += sx1, w2 += sx2, fr += dr, fg += dg, fb += dbb, fu += du, fv += dv, fz += dz, fzn += dzn,
             fw += dw, fuw += duw, fvw += dvw) {
            if (dbg && x == dbx && y == dby) rt_logf("  px w %lld %lld %lld bias %d %d %d fu %.2f fv %.2f fw %.4f u %.2f v %.2f\n", (long long)w0, (long long)w1, (long long)w2, bias0, bias1, bias2, fu, fv, fw, fw > 0 ? fuw / fw : 0.0, fw > 0 ? fvw / fw : 0.0);
            if ((w0 == 0 && bias0) || (w1 == 0 && bias1) || (w2 == 0 && bias2)) continue;
            int r = a.r, gg = a.g, bb = a.b;
            if (gouraud) { r = (int)fr; gg = (int)fg; bb = (int)fb; }
            int nx = s > 1 ? x / s : x;
            uint16_t px;
            int so = semi_on;
            if (textured) {
                int u = (int)fu;
                int v = (int)fv;
                if (persp && fw > 0) {   /* stays inside the polygon's uv box, as the affine values do */
                    u = (int)floor(fuw / fw + ub); v = (int)floor(fvw / fw + ub);
                    u = u < umn ? umn : u > umx ? umx : u; v = v < vmn ? vmn : v > vmx ? vmx : v;
                }
                uint16_t t = texel(st, u, v, clutx, cluty, tpx, tpy, depth);
                if (dbg && x == dbx && y == dby) rt_logf("  texel u %d v %d -> %04x (box %d-%d %d-%d)\n", u, v, t, umn, umx, vmn, vmx);
                if (t == 0) continue;
                so = semi_on && (t & 0x8000);
                px = raw ? t : shade(t, r, gg, bb, nx, ny, dith);
            } else {
                px = rgb15(r, gg, bb, nx, ny, dith);
            }
            uint16_t *p = &row[x];
            if (st->mask_check && (*p & 0x8000)) continue;
            if (dm == DM_TEST || dm == DM_TW) {
                if (zrow[x] > 0 && depth_reach((float)fz) < zrow[x]) continue;
                /* behind a nearer face of the same model (objsort) */
                const unsigned myid = (unsigned)a.oid & OID_MASK;
                if (ozrow[x] > 0 && depth_reach((float)fz) < ozrow[x]) {
                    const unsigned oi = oz_id(ozrow[x]);
                    if (myid && (oi == myid || (oi == (myid | OID_ACTOR) && gpu_propsort()))) continue;   /* own model (a still
                                                                                   prop stores its pixels with OID_ACTOR) */
                    if (oi && ((unsigned)a.oid & OID_GLASS)) continue;   /* glass drawn last: behind any model (glass_q) */
                    if ((oi & OID_ACTOR) && ((unsigned)a.oid & OID_STATIC)) {   /* still prop behind a character / another still prop */
                        if (!((unsigned)a.oid & OID_FLOOR)) continue;
                        const float kf = (float)fz * (gpu_depth_k() + gpu_floor_k());   /* floor-like: only well behind */
                        if (kf < 0.999f && (float)fz / (1.0f - kf) < ozrow[x]) continue;
                    }
                }
                if (dm == DM_TW) {
                    const unsigned st_bit = myid && ((unsigned)a.oid & OID_STATIC) && gpu_propsort() ? OID_ACTOR : 0;
                    float zs = oz_pack(objsort_store((float)fz), myid | st_bit);
                    if (zs > ozrow[x]) ozrow[x] = zs;
                }
            }
            if ((dm == DM_HTEST || dm == DM_WNT) && zrow[x] >= HUD_Z) continue;   /* HUD stays on top */
            if (dm == DM_HUD) zrow[x] = HUD_Z;
            if (dm == DM_WRITE && room_test() && zrow[x] > 0 &&
                (room_test() == 2 ? (float)(fz - zbias) < zrow[x] : depth_reach((float)fz) < zrow[x])) continue;
            if (dm == DM_WRITE || dm == DM_WNT) {
                const float rz = (float)(fz - zbias);
                zrow[x] = rz;
                if (!gpu_room_obj() || (dm == DM_WNT && !gpu_room_obj_wnt())) ozrow[x] = 0;   /* old rule: room paints, no object depth */
                else {
                    const float rzo = nearz ? (float)(fzn - zbias) : rz;
                    if (ozrow[x] > 0 && oz_id(ozrow[x])) {   /* object pixel in front, not nearer than the whole polygon: stays */
                        const float ot = oz_reach(ozrow[x]);
                        if (ot >= rzo && (!gpu_room_obj_span() || ot <= a.zl)) continue;
                    }
                    const float mk = oz_pack(oz_mark(rzo), 0), mk2 = oz_pack(oz_mark(a.zl), 0);   /* room marks, as D3D11 (ps_om) */
                    if (mk > ozrow[x]) ozrow[x] = mk;
                    if (gpu_room_obj_span() && mk2 < ozrow[x]) ozrow[x] = mk2;
                }
            }
            if (so) px = blend(*p, px, st->semi);
            if (poly_id_vis && T == &hr) {   /* debug (RUGRATS_POLYID=1): each polygon one flat colour from its number */
                const uint32_t q = a.seq * 2654435761u;
                px = (uint16_t)((q >> 8) & 0x7FFF);
            }
            if (depth_vis > 0 && T == &hr && hr_z) {   /* debug: no-depth polygons red, objects over no depth magenta */
                if (dm == DM_NONE) px = (px & 0x7BDE) >> 1 | 0x0010;
                else if ((dm == DM_TEST || dm == DM_TW) && zrow[x] == 0) px = 0x7C1F;
            }
            *p = px | (st->mask_set ? 0x8000 : 0);
            if (dbg && x == dbx && y == dby) rt_logf("  written %04x dm %d iz %.4f zrow %.4f\n", px, dm, fz, zrow ? zrow[x] : 0.0f);
        }
    }
    #undef EDGE
}

/* ---- rectangles ----------------------------------------------------- */
static void rect_t(const St *st, const Tgt *T, int band, int nb, int x0, int y0, int w, int h, int r, int gg,
                   int b, int textured, int semi_on, int raw, int u0, int v0, int clutx, int cluty) {
    const int s = T->s;
    /* shadow pixel X belongs to native column x0 + (X - x0*s)/s; sprites keep their texels, just finer edges */
    int ya = y0 * s, yb = (y0 + h) * s - 1, xa = x0 * s, xb = (x0 + w) * s - 1;
    if (ya < st->dy1 * s) ya = st->dy1 * s;
    if (yb > st->dy2 * s + s - 1) yb = st->dy2 * s + s - 1;
    if (xa < st->dx1 * s) xa = st->dx1 * s;
    if (xb > st->dx2 * s + s - 1) xb = st->dx2 * s + s - 1;
    for (int y = ya; y <= yb; y++) {
        if (!OWNS(y, band, nb)) continue;
        int yy = (y - y0 * s) / s;
        for (int x = xa; x <= xb; x++) {
            int xx = (x - x0 * s) / s;
            if (textured) {
                int u = st->rect_flipx ? u0 - xx : u0 + xx;
                int v = st->rect_flipy ? v0 - yy : v0 + yy;
                uint16_t t = texel(st, u, v, clutx, cluty, st->tpx, st->tpy, st->tdepth);
                if (t == 0) continue;
                plot(st, T, x, y, raw ? t : shade(t, r, gg, b, 0, 0, 0), semi_on && (t & 0x8000));
            } else {
                plot(st, T, x, y, rgb15(r, gg, b, 0, 0, 0), semi_on);
            }
        }
    }
    if (T == &nat && !nat_nocost) cpu.cycles += (uint32_t)(w * h) / 8;
}

/* ---- lines ---------------------------------------------------------- */
static void line_t(const St *st, const Tgt *T, int band, int nb, Vtx a, Vtx b, int gouraud, int semi_on) {
    int dx = b.x - a.x, dy = b.y - a.y;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (abs(dx) > 1023 || abs(dy) > 511) return;
    const int s = T->s;
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + dx * i / n : a.x, y = n ? a.y + dy * i / n : a.y;
        if (x < st->dx1 || x > st->dx2 || y < st->dy1 || y > st->dy2) continue;
        int r = a.r, gg = a.g, bb = a.b;
        if (gouraud && n) { r += (b.r - a.r) * i / n; gg += (b.g - a.g) * i / n; bb += (b.b - a.b) * i / n; }
        uint16_t c = rgb15(r, gg, bb, x, y, st->dither && gouraud);
        for (int j = 0; j < s; j++) /* a native pixel is an s x s block in the shadow */
            if (OWNS(y * s + j, band, nb))
                for (int k = 0; k < s; k++) plot(st, T, x * s + k, y * s + j, c, semi_on);
    }
}

/* ---- shadow command queue + workers ----------------------------------- */
enum { HC_TRI, HC_RECT, HC_LINE, HC_FILL };
typedef struct {
    int type, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth;
    Vtx v[3];
    int w, h, r, gg, b; /* rect */
    St st;
} HCmd;

#define HQ_SIZE 32768
static HCmd *hq;
static int64_t hq_wr, hq_pub, hq_since_drain;
static int64_t hq_rd[16];
static int hq_nw;
static SRWLOCK hq_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE hq_work = CONDITION_VARIABLE_INIT, hq_done = CONDITION_VARIABLE_INIT;
/* native area that queued commands still read as texture/CLUT (empty when x0 > x1) */
static int tex_x0 = 1 << 30, tex_y0, tex_x1 = -1, tex_y1;

/* VRAM fill in the shadow (no mask, no clipping, wraps like the native one) */
static void fill_hr(const HCmd *c, int band, int nb) {
    const int s = hr.s, x0 = c->v[0].x * s, w = c->w * s, y0 = c->v[0].y * s, h = c->h * s;
    const uint16_t col = (uint16_t)c->r;
    for (int yy = 0; yy < h; yy++) {
        int y = (y0 + yy) % hr.h;
        if (!OWNS(y, band, nb)) continue;
        uint16_t *row = &hr.buf[(size_t)y * hr.w];
        if (x0 + w <= hr.w) for (int i = 0; i < w; i++) row[x0 + i] = col;
        else for (int i = 0; i < w; i++) row[(x0 + i) % hr.w] = col;
        if (hr_z) { /* a fill starts a new picture there: no depth */
            float *zr = &hr_z[(size_t)y * hr.w];
            for (int i = 0; i < w; i++) zr[(x0 + i) % hr.w] = 0;
            float *ozr = &hr_oz[(size_t)y * hr.w];
            for (int i = 0; i < w; i++) ozr[(x0 + i) % hr.w] = 0;
        }
    }
}

static void hcmd_run(const HCmd *c, int band, int nb) {
    switch (c->type) {
    case HC_FILL: fill_hr(c, band, nb); break;
    case HC_TRI:
        tri_t(&c->st, &hr, band, nb, c->v[0], c->v[1], c->v[2], c->textured, c->gouraud, c->semi_on, c->raw,
              c->clutx, c->cluty, c->tpx, c->tpy, c->depth);
        break;
    case HC_RECT:
        rect_t(&c->st, &hr, band, nb, c->v[0].x, c->v[0].y, c->w, c->h, c->r, c->gg, c->b, c->textured, c->semi_on,
               c->raw, c->v[0].u, c->v[0].v, c->clutx, c->cluty);
        break;
    case HC_LINE:
        line_t(&c->st, &hr, band, nb, c->v[0], c->v[1], c->gouraud, c->semi_on);
        break;
    }
}

static DWORD WINAPI hq_worker(LPVOID arg) {
    int band = (int)(intptr_t)arg;
    for (;;) {
        AcquireSRWLockExclusive(&hq_lock);
        while (hq_rd[band] == hq_pub) SleepConditionVariableSRW(&hq_work, &hq_lock, INFINITE, 0);
        int64_t from = hq_rd[band], to = hq_pub;
        ReleaseSRWLockExclusive(&hq_lock);
        for (int64_t i = from; i < to; i++) hcmd_run(&hq[i & (HQ_SIZE - 1)], band, hq_nw);
        AcquireSRWLockExclusive(&hq_lock);
        hq_rd[band] = to;
        WakeAllConditionVariable(&hq_done);
        ReleaseSRWLockExclusive(&hq_lock);
    }
    return 0;
}

static void hq_publish(void) {
    if (hq_pub == hq_wr) return;
    AcquireSRWLockExclusive(&hq_lock);
    hq_pub = hq_wr;
    WakeAllConditionVariable(&hq_work);
    ReleaseSRWLockExclusive(&hq_lock);
}

/* wait until the shadow is complete (before VRAM uploads/copies/fills, display readout, resize) */
uint64_t gpu_prof_sync_n, gpu_prof_sync_t, gpu_prof_late_n; /* RUGRATS_PROF: syncs and QPC ticks spent waiting */
void gpu_hr_sync(void) {
    if (!hq_nw) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    gpu_prof_sync_n++;
    hq_publish();
    AcquireSRWLockExclusive(&hq_lock);
    for (;;) {
        int busy = 0;
        for (int k = 0; k < hq_nw; k++) busy |= hq_rd[k] != hq_pub;
        if (!busy) break;
        SleepConditionVariableSRW(&hq_done, &hq_lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&hq_lock);
    hq_since_drain = 0;
    tex_x0 = 1 << 30; tex_x1 = -1;
    QueryPerformanceCounter(&t1);
    gpu_prof_sync_t += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

static void hq_start(void) {
    if (hq_nw || !hr.buf) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    /* all but two hardware threads (the game thread and the OS); workers run below normal priority.
       8 was the old cap; 14 on a 16-thread CPU is ~30% faster at 9x */
    int n = (int)si.dwNumberOfProcessors - 2;
    const char *e = getenv("RUGRATS_THREADS");
    if (e) n = atoi(e);
    if (n > 16) n = 16;
    if (n < 1) n = 1;
    hq = calloc(HQ_SIZE, sizeof *hq);
    if (!hq) return;
    for (int k = 0; k < n; k++) {
        HANDLE t = CreateThread(NULL, 0, hq_worker, (LPVOID)(intptr_t)k, 0, NULL);
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); /* the game thread comes first */
        CloseHandle(t);
    }
    hq_nw = n;
    rt_logf("gpu: %d shadow raster threads\n", n);
}

/* native texture/CLUT area a command reads */
static void tex_note(int tpx, int tpy, int depth, int clutx, int cluty) {
    int w = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    int x0 = tpx, x1 = tpx + w - 1, y0 = tpy, y1 = tpy + 255;
    if (depth < 2) { /* CLUT row */
        int cw = depth == 0 ? 16 : 256;
        if (clutx < x0) x0 = clutx;
        if (clutx + cw - 1 > x1) x1 = clutx + cw - 1;
        if (cluty < y0) y0 = cluty;
        if (cluty > y1) y1 = cluty;
    }
    if (tex_x0 > tex_x1) { tex_x0 = x0; tex_x1 = x1; tex_y0 = y0; tex_y1 = y1; return; }
    if (x0 < tex_x0) tex_x0 = x0;
    if (x1 > tex_x1) tex_x1 = x1;
    if (y0 < tex_y0) tex_y0 = y0;
    if (y1 > tex_y1) tex_y1 = y1;
}

/* a native write to this VRAM area must not change texels that queued commands still read */
static void guard_rect(int x0, int y0, int x1, int y1) {
    if (tex_x0 > tex_x1) return;
    if (x1 < tex_x0 || x0 > tex_x1 || y1 < tex_y0 || y0 > tex_y1) return;
    gpu_hr_sync();
}
static void native_guard(void) { guard_rect(g.dx1, g.dy1, g.dx2, g.dy2); }

const uint16_t *gpu_vram_hr(void);

/* ---- one-frame-late presentation ----------------------------------------
   At a present, the frame from the previous present is shown instead of draining the queue: the workers
   finish it while the guest runs on. Valid only if nothing queued or written since then touched its area
   (the game draws the next frame into the other VRAM buffer); otherwise the caller drains and shows now. */
static int64_t late_mark = -1;      /* queue position that completes the pending frame */
static int late_r[4], late_dirty;   /* its native display rect x0,y0,x1,y1 (inclusive) */
static void late_touch(int x0, int y0, int x1, int y1) {
    if (late_mark < 0 || late_dirty) return;
    if (x1 < late_r[0] || x0 > late_r[2] || y1 < late_r[1] || y0 > late_r[3]) return;
    late_dirty = 1;
}
static void late_touch_cmd(const HCmd *c) {
    if (late_mark < 0 || late_dirty) return;
    if (c->type == HC_FILL) {
        if (c->v[0].x + c->w > 1024 || c->v[0].y + c->h > 512) { late_dirty = 1; return; }
        late_touch(c->v[0].x, c->v[0].y, c->v[0].x + c->w - 1, c->v[0].y + c->h - 1);
    } else {
        late_touch(c->st.dx1, c->st.dy1, c->st.dx2, c->st.dy2); /* drawing area bounds every primitive */
    }
}
static void hq_wait_until(int64_t mark) {
    if (!hq_nw) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (hq_pub < mark) hq_publish();
    AcquireSRWLockExclusive(&hq_lock);
    for (;;) {
        int busy = 0;
        for (int k = 0; k < hq_nw; k++) busy |= hq_rd[k] < mark;
        if (!busy) break;
        SleepConditionVariableSRW(&hq_done, &hq_lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&hq_lock);
    QueryPerformanceCounter(&t1);
    gpu_prof_sync_t += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

/* the display rect (native) to show now; returns the shadow buffer. *late = 1 when the previous frame is shown */
const uint16_t *gpu_present_hr(int *x, int *y, int *w, int *h, int *late) {
    *late = 0;
    if (hw_s) { hw_flush(); late_mark = -1; return NULL; } /* the host shows the GPU target (hw_present) */
    if (!hr.buf || !hq_nw) { late_mark = -1; return gpu_vram_hr(); }
    int cur[4] = { *x, *y, *x + *w - 1, *y + *h - 1 };
    int same_size = late_mark >= 0 && late_r[2] - late_r[0] == cur[2] - cur[0] && late_r[3] - late_r[1] == cur[3] - cur[1];
    int ok = late_mark >= 0 && !late_dirty && same_size && opt.late_present;
    if (ok) {
        hq_wait_until(late_mark);
        gpu_prof_late_n++;
        *x = late_r[0]; *y = late_r[1];
        *late = 1;
    } else {
        gpu_hr_sync();
    }
    /* the current frame becomes the pending one */
    hq_publish();
    late_mark = opt.late_present ? hq_wr : -1;
    memcpy(late_r, cur, sizeof cur);
    late_dirty = 0;
    return hr.buf;
}

static void hq_push(const HCmd *c) {
    late_touch_cmd(c);
    if (!hq_nw) { hcmd_run(c, 0, 1); return; }
    if (hq_since_drain >= HQ_SIZE - 1) gpu_hr_sync();
    hq[hq_wr & (HQ_SIZE - 1)] = *c;
    hq_wr++;
    hq_since_drain++;
    if (hq_wr - hq_pub >= 32) hq_publish();
}

int gpu_scale(void) { return hr.buf ? hr.s : hw_s ? hw_s : 1; }
int gpu_hw_on(void) { return hw_s != 0; }
const uint16_t *gpu_vram_hr(void) {
    if (hw_s) { const uint16_t *p = hw_readback(); if (p) return p; }
    gpu_hr_sync();
    return hr.buf ? hr.buf : vram;
}

void gpu_native_sync(void);
/* change the internal resolution / renderer; the shadow (CPU renderer, s > 1) starts as a nearest-neighbour copy of
   VRAM, the D3D11 target (any s) as a blit of it */
void gpu_set_scale(int s) {
    if (s < 1) s = 1;
    if (s > MAX_INTERNAL) s = MAX_INTERNAL;
    gpu_hr_sync();
    late_mark = -1;
    int want_hw = opt.renderer; /* the D3D11 renderer at any scale, 1x too (2026-11-07: 1x CPU raster was 2.5x slower) */
    if (s == gpu_scale() && (hw_s != 0) == want_hw && (!hw_s || hw_samples_req() == gpu_msaa())) return;   /* MSAA change: new target */
    gpu_native_sync(); /* the next renderer starts from native VRAM */
    free(hr.buf);
    free(hr_z); free(hr_oz);
    hr.buf = NULL; hr_z = hr_oz = NULL; hr.s = 1; hr.w = 1024; hr.h = 512;
    if (hw_s) { hw_end(); hw_s = 0; }
    if (s == 1 && !want_hw) return;
    if (want_hw && hw_begin(s)) { hw_s = s; return; }
    if (want_hw) rt_logf("gpu: D3D11 renderer not available, using the CPU renderer\n");
    uint16_t *b = malloc((size_t)1024 * s * 512 * s * 2);
    if (!b) return;
    for (int y = 0; y < 512 * s; y++)
        for (int x = 0; x < 1024 * s; x++) b[(size_t)y * 1024 * s + x] = vram[(y / s) * 1024 + x / s];
    hr.buf = b; hr.s = s; hr.w = 1024 * s; hr.h = 512 * s;
    hr_z = calloc((size_t)hr.w * hr.h, sizeof *hr_z); /* NULL = no depth check (still draws) */
    hr_oz = hr_z ? calloc((size_t)hr.w * hr.h, sizeof *hr_oz) : NULL;
    if (!hr_oz) { free(hr_z); hr_z = NULL; }
    hq_start();
}

/* ---- D3D11 hardware renderer (gpu_d3d11.c): build its vertices ---------- */

/* Stretch 2D Off (ini [video] stretch2d=0, widescreen, GPU renderer): the 320-wide PS1 picture is shown stretched to
   16:9 and the GTE squeezes the 3D world by 3/4 to make up for it, so 2D (HUD, menus, text, full 2D screens) came out
   4/3 too wide. Here the D3D11 vertices of everything in the game's HUD layers (gpu_layer 2 and 3, hw.c) are pulled
   toward the centre of the drawing area by the same 3/4: 2D keeps its 4:3 shape, centred. The sky (layer 4) and the
   3D world (layer 1) still fill the screen; on frames without the world (title, menus, loading screens: layers 1 and 4
   hold the logo, speech bubble and background) every layer is squeezed and the sides painted black: plain 4:3. Native VRAM is untouched (the game reads it). The menu
   button icons (3D models in layer 3, 8003f934) are then projected without the GTE squeeze (hooks.c), like the golf
   meter frame already was. RUGRATS_STRETCH2D=0/1 overrides the ini. */
static int sq_on;                 /* this command's vertices are squeezed */
int gpu_sq_mode(void) { return hw_s && opt.widescreen && !opt.stretch2d; }
static void hw_area(int x, int y, int w, int h, uint32_t kind, uint16_t c);
static void sq_set(void) {
    extern int gpu_layer;
    extern uint32_t gpu_frame_tok;
    sq_on = gpu_sq_mode() && gpu_src_addr != 0xFFFFFFFF && gpu_layer >= 0 &&
            (gpu_layer == 2 || gpu_layer == 3 || !gpu_world_frame);
    static uint32_t bars_tok;
    if (sq_on && !gpu_world_frame && bars_tok != gpu_frame_tok) {
        /* whole frame squeezed: black side bars in the hi-res picture (else old pixels show there) */
        bars_tok = gpu_frame_tok;
        int64_t ws_hud_num(void), ws_hud_den(void);
        const int aw = g.dx2 - g.dx1 + 1, ah = g.dy2 - g.dy1 + 1;
        const int bw = (int)((aw - (int64_t)aw * ws_hud_den() / ws_hud_num()) / 2) + 1;
        if (aw > 2 * bw && ah > 0) {
            hw_area(g.dx1, g.dy1, bw, ah, HWF_FILL, 0);
            hw_area(g.dx2 + 1 - bw, g.dy1, bw, ah, HWF_FILL, 0);
        }
    }
}
static float sq_x(const St *st, float x) {
    if (!sq_on) return x;
    int64_t ws_hud_num(void), ws_hud_den(void);
    const float k = (float)ws_hud_den() / (float)ws_hud_num(), cx = (st->dx1 + st->dx2 + 1) * 0.5f;
    return cx + (x - cx) * k;
}


static uint32_t hw_flags(const St *st, int textured, int raw, int semi_on, int dither, int depth) {
    static int warned;
    if (st->mask_check && !warned) { warned = 1; rt_logf("hwgpu: game uses the mask check bit (not emulated)\n"); }
    return (uint32_t)depth | (textured ? HWF_TEX : 0) | (raw ? HWF_RAW : 0) | (semi_on ? HWF_SEMI : 0) |
           ((uint32_t)st->semi << 5) | (dither ? HWF_DITHER : 0) | (st->mask_set ? HWF_MASK : 0) |
           (opt.dither == 2 && !raw ? HWF_FULL : 0);
}
static uint32_t hw_tpage(const St *st, int tpx, int tpy) {
    return (uint32_t)(tpx / 64) | (uint32_t)(tpy / 256) << 4 | (uint32_t)st->twmx << 8 | (uint32_t)st->twmy << 13 |
           (uint32_t)st->twox << 18 | (uint32_t)st->twoy << 23;
}
static void hw_tex_read(int tpx, int tpy, int depth, int clutx, int cluty) {
    hw_note_read(tpx, tpy, tpx + (depth == 0 ? 64 : depth == 1 ? 128 : 256) - 1, tpy + 255);
    if (depth < 2) hw_note_read(clutx, cluty, clutx + (depth == 0 ? 16 : 256) - 1, cluty);
}

/* Occlusion boxes (hooks.c low-wall rule, gpu_d3d11.c hw_occl_add): room boxes projected by the hook while the game
   picks the frame's rooms, as triangles {GTE screen x, y, 4096 / z}. They wait here until the frame's first room
   piece is drawn, which gives the drawing offset / area of the buffer this frame goes to. */
enum { OCCL_MAX = 256 };
static struct { uint32_t id; int n; float v[72][3]; } occl_q[OCCL_MAX];
static int occl_n;
void gpu_occl_box(uint32_t id, const float (*v)[3], int n) {
    if (!hw_s || n <= 0 || n > 72 || occl_n >= OCCL_MAX) return;
    occl_q[occl_n].id = id; occl_q[occl_n].n = n;
    memcpy(occl_q[occl_n].v, v, (size_t)n * sizeof *v);
    occl_n++;
}
static void occl_emit(const St *st) {
    const float s = (float)hw_s;
    for (int i = 0; i < occl_n; i++) {
        HwVert o[72];
        memset(o, 0, sizeof o);
        for (int k = 0; k < occl_q[i].n; k++) {
            float z = occl_q[i].v[k][2] * (1.0f / 64);
            o[k].x = (g.offx + occl_q[i].v[k][0]) * s; o[k].y = (g.offy + occl_q[i].v[k][1]) * s;
            o[k].z = z > 1.0f ? 1.0f : z;
        }
        hw_occl_add(occl_q[i].id, o, occl_q[i].n, st->dx1, st->dy1, st->dx2, st->dy2);
    }
    occl_n = 0;
}
static void hw_tri(const St *st, Vtx a, Vtx b, Vtx c, int textured, int gouraud, int semi_on, int raw, int clutx,
                   int cluty, int tpx, int tpy, int depth) {
    const Vtx *p[3] = { &a, &b, &c };
    int minx = a.x, maxx = a.x, miny = a.y, maxy = a.y, umin = a.u, umax = a.u, vmin = a.v, vmax = a.v;
    for (int k = 1; k < 3; k++) {
        if (p[k]->x < minx) minx = p[k]->x;
        if (p[k]->x > maxx) maxx = p[k]->x;
        if (p[k]->y < miny) miny = p[k]->y;
        if (p[k]->y > maxy) maxy = p[k]->y;
        if (p[k]->u < umin) umin = p[k]->u;
        if (p[k]->u > umax) umax = p[k]->u;
        if (p[k]->v < vmin) vmin = p[k]->v;
        if (p[k]->v > vmax) vmax = p[k]->v;
    }
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    int dith = st->dither && (gouraud || (textured && !raw));
    uint32_t fl = hw_flags(st, textured, raw, semi_on, dith, depth), tp = hw_tpage(st, tpx, tpy);
    if (a.dm == DM_TEST || a.dm == DM_TW) fl |= (uint32_t)(a.oid & (OID_MASK | OID_STATIC | OID_FLOOR | OID_GLASS)) << 16;   /* model id + "behind characters" bit 29 + floor-like bit 30 + glass bit 31 (objsort, ps_to / ps_ow) */
    if (a.dm == DM_WRITE || a.dm == DM_WNT) {   /* zl for the shader's zlim(): 512 steps per octave from 1/64, rounded up */
        int q = a.zl > 0 ? (int)ceil((log2((double)a.zl) + 6.0) * 512.0) : 0;
        fl |= (uint32_t)(q < 0 ? 0 : q > 8191 ? 8191 : q) << 16;
    }
    uint32_t lim = (uint32_t)umin | (uint32_t)vmin << 8 | (uint32_t)umax << 16 | (uint32_t)vmax << 24;
    const float s = (float)hw_s;
    HwVert v[3];
    for (int k = 0; k < 3; k++) {
        const Vtx *q = gouraud ? p[k] : &a;
        v[k].x = sq_x(st, p[k]->x + p[k]->fx / 256.0f) * s;
        v[k].y = p[k]->y * s + p[k]->fy * s / 256.0f;
        v[k].r = (float)q->r; v[k].g = (float)q->g; v[k].b = (float)q->b;
        v[k].u = (float)p[k]->u; v[k].v = (float)p[k]->v;
        v[k].clut = (uint32_t)clutx | (uint32_t)cluty << 16;
        v[k].tpage = tp; v[k].flags = fl; v[k].uvlim = lim;
        /* depth check value: 4096/z scaled into 0..1 (stays linear in screen space); objects carry the tolerance */
        v[k].z = a.dm == DM_WRITE || a.dm == DM_WNT ? p[k]->iz * (1.0f / 64) : a.dm == DM_TEST || a.dm == DM_TW ? p[k]->iz * (1.0f / 64) :
                 a.dm == DM_HUD ? 1.0f : 0;   /* testers: the gap is applied per pixel (ps_test); HTEST: ps_h */
        v[k].zn = a.dm == DM_WNT && gpu_room_obj_wnt() == 2 ? p[k]->izn * (1.0f / 64) : 0;
        v[k].pw = textured && a.pw > 0 && b.pw > 0 && c.pw > 0 ? p[k]->pw : 0;   /* 0 = affine (shader uses 1) */        if (v[k].pw > 0) { v[k].u += p[k]->uf; v[k].v += p[k]->vf; }   /* split point: exact texture fraction */
    }
    if (occl_n && a.dm == DM_WRITE) occl_emit(st);   /* with the frame's first room piece: same batch, same drawing area */
    hw_add(v, 3, semi_on && st->semi == 2, a.dm, st->dx1, st->dy1, st->dx2, st->dy2);
    if (textured) hw_tex_read(tpx, tpy, depth, clutx, cluty);
}

static void hw_quad(float x0, float y0, float x1, float y1, float u1, float v1, int r, int gg, int b,
                    uint32_t clut, uint32_t tp, uint32_t fl, uint32_t lim, int sub, int cx0, int cy0, int cx1, int cy1) {
    HwVert c[4], o[6];
    for (int k = 0; k < 4; k++) {
        c[k].x = (k & 1) ? x1 : x0; c[k].y = (k & 2) ? y1 : y0;
        c[k].r = (float)r; c[k].g = (float)gg; c[k].b = (float)b;
        c[k].u = (k & 1) ? u1 : 0; c[k].v = (k & 2) ? v1 : 0;
        c[k].clut = clut; c[k].tpage = tp; c[k].flags = fl; c[k].uvlim = lim;
        c[k].z = 0; c[k].zn = 0; c[k].pw = 0;
    }
    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[1]; o[4] = c[3]; o[5] = c[2];
    /* a fill starts a new picture there: depth 0 (nothing) */
    hw_add(o, 6, sub, ((fl >> 12) & 3) == 1 && opt.depthfix ? HW_DM_CLEAR : HW_DM_NONE, cx0, cy0, cx1, cy1);
}

static void hw_rect(const St *st, int x0, int y0, int w, int h, int r, int gg, int b, int textured, int semi_on,
                    int raw, int u0, int v0, int clutx, int cluty) {
    const int s = hw_s;
    uint32_t fl = hw_flags(st, textured, raw, semi_on, 0, st->tdepth) | HWF_RECT |
                  (st->rect_flipx ? HWF_FLIPX : 0) | (st->rect_flipy ? HWF_FLIPY : 0);
    hw_quad(sq_x(st, (float)x0) * s, (float)(y0 * s), sq_x(st, (float)(x0 + w)) * s, (float)((y0 + h) * s), (float)w, (float)h,
            r, gg, b, (uint32_t)clutx | (uint32_t)cluty << 16, hw_tpage(st, st->tpx, st->tpy), fl,
            (uint32_t)(u0 & 255) | (uint32_t)(v0 & 255) << 8, semi_on && st->semi == 2, st->dx1, st->dy1, st->dx2, st->dy2);
    if (textured) hw_tex_read(st->tpx, st->tpy, st->tdepth, clutx, cluty);
}

/* a line is drawn as native pixels (s x s blocks), like the software renderer */
static void hw_line(const St *st, Vtx a, Vtx b, int gouraud, int semi_on) {
    int dx = b.x - a.x, dy = b.y - a.y;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (abs(dx) > 1023 || abs(dy) > 511) return;
    const int s = hw_s;
    uint32_t fl = hw_flags(st, 0, 0, semi_on, st->dither && gouraud, 0);
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + dx * i / n : a.x, y = n ? a.y + dy * i / n : a.y;
        if (x < st->dx1 || x > st->dx2 || y < st->dy1 || y > st->dy2) continue;
        int r = a.r, gg = a.g, bb = a.b;
        if (gouraud && n) { r += (b.r - a.r) * i / n; gg += (b.g - a.g) * i / n; bb += (b.b - a.b) * i / n; }
        hw_quad(sq_x(st, (float)x) * s, (float)(y * s), sq_x(st, (float)(x + 1)) * s, (float)(y * s + s), 0, 0, r, gg, bb, 0, 0, fl, 0,
                semi_on && st->semi == 2, st->dx1, st->dy1, st->dx2, st->dy2);
    }
}

/* fill (colour c) or blit from native VRAM of a native rect that may wrap */
static void hw_area(int x, int y, int w, int h, uint32_t kind, uint16_t c) {
    const int s = hw_s;
    for (int py = 0; py < h;) {
        int yy = (y + py) & 511, hh = h - py;
        if (yy + hh > 512) hh = 512 - yy;
        for (int px = 0; px < w;) {
            int xx = (x + px) & 1023, ww = w - px;
            if (xx + ww > 1024) ww = 1024 - xx;
            hw_quad((float)(xx * s), (float)(yy * s), (float)((xx + ww) * s), (float)((yy + hh) * s), 0, 0, 0, 0, 0,
                    c, 0, kind, 0, 0, 0, 0, 1023, 511);
            if (kind == HWF_BLIT) hw_note_read(xx, yy, xx + ww - 1, yy + hh - 1);
            px += ww;
        }
        py += hh;
    }
}

/* ---- drawing entry points: native now, shadow queued ------------------ */

/* ---- native copy only where needed (D3D11 renderer) --------------------------------------------------------
   With the D3D11 renderer the picture is drawn on the GPU; the native 1x VRAM (CPU) used to get every tri/rect/line
   too, which cost about 2/3 of the frame. Native VRAM is only READ back by the game when it samples drawn pixels as
   a texture/CLUT, copies them, or reads VRAM to the CPU. Rugrats only does the first, on a small patch at VRAM 0,0
   (levels 7, 14-16, 18; checked over all levels with RUGRATS_NATIVELOG=1). So:
   - native_mode 2 (default): prims are drawn on the CPU only inside "hot" rects (seeded with 0,0-15,31, plus any
     area the game is caught reading at run time); everything else is GPU-only and its tiles are marked in nl_map.
   - A read of a GPU-only tile (texture, CLUT, copy source, VRAM->CPU) first copies those pixels back from the GPU
     target (hw_read_native) and, for textures/CLUTs, makes the area hot. Save states and scale/renderer changes
     copy back everything first, so native VRAM is exact whenever something outside the GPU path looks at it.
   - The emulated GPU drawing time (cpu.cycles) is charged for every prim exactly as the full CPU raster did, so
     game timing does not depend on this mode.
   RUGRATS_NONATIVE=0 = full CPU copy (old behaviour), 1 = no CPU copy at all (debug), else auto.
   RUGRATS_NATIVELOG=1 logs every read of GPU-only pixels. */
static int nat_mode = -1;      /* 0 full, 1 none, 2 where needed */
#define HOT_MAX 16
static int hot_n, hot_r[HOT_MAX][4];   /* x0, y0, x1, y1 (tile aligned, inclusive) */
static uint8_t hot_t[32][64];          /* 16x16 tiles inside a hot rect */
static uint8_t nl_map[32][64];         /* 16x16 tiles drawn on the GPU only (native copy stale) */
static int nl_any;                     /* some nl_map tile is set */
static int nl_log = -1;
static unsigned nl_count[4];

static void hot_add(int x0, int y0, int x1, int y1) {
    x0 &= ~15; y0 &= ~15; x1 |= 15; y1 |= 15;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 1023) x1 = 1023;
    if (y1 > 511) y1 = 511;
    for (int i = 0; i < hot_n; i++)
        if (x0 >= hot_r[i][0] && y0 >= hot_r[i][1] && x1 <= hot_r[i][2] && y1 <= hot_r[i][3]) return;
    if (hot_n == HOT_MAX) {
        rt_logf("gpu: native copy needed in too many places, drawing all of it on the CPU from now on\n");
        nat_mode = 0;
        return;
    }
    hot_r[hot_n][0] = x0; hot_r[hot_n][1] = y0; hot_r[hot_n][2] = x1; hot_r[hot_n][3] = y1;
    hot_n++;
    for (int ty = y0 >> 4; ty <= y1 >> 4; ty++)
        for (int tx = x0 >> 4; tx <= x1 >> 4; tx++) hot_t[ty][tx] = 1;
    rt_logf("gpu: native copy drawn on the CPU in %d,%d-%d,%d (%d areas)\n", x0, y0, x1, y1, hot_n);
}

static int native_mode(void) {
    if (nat_mode < 0) {
        const char *e = getenv("RUGRATS_NONATIVE");
        nat_mode = e && *e == '0' ? 0 : e && *e == '1' ? 1 : 2;
        const char *l = getenv("RUGRATS_NATIVELOG");
        nl_log = l && atoi(l);
        if (nat_mode == 2) hot_add(0, 0, 15, 31); /* seen read as a texture in 5 levels */
    }
    return nat_mode;
}
static int nl_tracking(void) { return native_mode() != 0 || nl_log; }

static int nl_clip(int *x0, int *y0, int *x1, int *y1) {
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > 1023) *x1 = 1023;
    if (*y1 > 511) *y1 = 511;
    return *x0 <= *x1 && *y0 <= *y1;
}
static void nl_set(int x0, int y0, int x1, int y1, int v) {
    if (!nl_tracking() || !nl_clip(&x0, &y0, &x1, &y1)) return;
    for (int ty = y0 >> 4; ty <= y1 >> 4; ty++)
        for (int tx = x0 >> 4; tx <= x1 >> 4; tx++) nl_map[ty][tx] = (uint8_t)v;
    if (v) nl_any = 1;
}
/* is any GPU-only (not hot) tile inside the rect? */
static int nl_stale(int x0, int y0, int x1, int y1) {
    if (!nl_any || !nl_clip(&x0, &y0, &x1, &y1)) return 0;
    for (int ty = y0 >> 4; ty <= y1 >> 4; ty++)
        for (int tx = x0 >> 4; tx <= x1 >> 4; tx++)
            if (nl_map[ty][tx] && !hot_t[ty][tx]) return 1;
    return 0;
}
/* copy the GPU-only tiles inside the rect back into native VRAM (from the GPU target) */
static void nl_refresh(int x0, int y0, int x1, int y1) {
    if (!hw_s || !nl_any || !nl_clip(&x0, &y0, &x1, &y1)) return;
    int bx0 = 1024, by0 = 512, bx1 = -1, by1 = -1;
    for (int ty = y0 >> 4; ty <= y1 >> 4; ty++)
        for (int tx = x0 >> 4; tx <= x1 >> 4; tx++)
            if (nl_map[ty][tx] && !hot_t[ty][tx]) {
                if (tx * 16 < bx0) bx0 = tx * 16;
                if (ty * 16 < by0) by0 = ty * 16;
                if (tx * 16 + 15 > bx1) bx1 = tx * 16 + 15;
                if (ty * 16 + 15 > by1) by1 = ty * 16 + 15;
            }
    if (bx1 < 0) return;
    static uint16_t *tmp;
    if (!tmp) tmp = malloc(1024 * 512 * 2);
    if (!tmp || !hw_read_native(bx0, by0, bx1, by1, tmp)) return;
    for (int ty = by0 >> 4; ty <= by1 >> 4; ty++)
        for (int tx = bx0 >> 4; tx <= bx1 >> 4; tx++) {
            if (!nl_map[ty][tx] || hot_t[ty][tx]) continue;
            for (int y = ty * 16; y < ty * 16 + 16; y++)
                memcpy(&vram[y * 1024 + tx * 16], &tmp[y * 1024 + tx * 16], 32);
            nl_map[ty][tx] = 0;
        }
    hw_mark(by0, by1); /* the GPU's copy of native VRAM (texture source) needs these rows again */
}
/* everything native VRAM is missing (save states, renderer/scale changes) */
void gpu_native_sync(void) {
    if (hw_s && nl_any) nl_refresh(0, 0, 1023, 511);
    if (hw_s) { memset(nl_map, 0, sizeof nl_map); nl_any = 0; }
}
/* the game is about to read native VRAM in this rect: kind 0 texture, 1 CLUT, 2 copy source, 3 VRAM->CPU */
static void nl_read(int x0, int y0, int x1, int y1, int kind) {
    static const char *names[4] = { "texture", "clut", "copy source", "vram->cpu read" };
    if (!nl_tracking() || !nl_stale(x0, y0, x1, y1)) return;
    unsigned n = ++nl_count[kind];
    if (nl_log && (n <= 10 || (n & (n - 1)) == 0))
        rt_logf("nativelog: %s reads GPU-only VRAM %d,%d-%d,%d (#%u, totals tex %u clut %u copy %u read %u)\n",
                names[kind], x0, y0, x1, y1, n, nl_count[0], nl_count[1], nl_count[2], nl_count[3]);
    if (native_mode() == 1) return; /* debug: stays stale */
    if (native_mode() == 2 && kind <= 1) hot_add(x0, y0, x1, y1);
    nl_refresh(x0, y0, x1, y1);
}
/* texture area actually sampled: uv box -> VRAM pixels for the colour depth */
static void nl_tex(int tpx, int tpy, int depth, int umin, int vmin, int umax, int vmax, int clutx, int cluty) {
    if (!nl_tracking() || !nl_any) return;
    int sh = depth == 0 ? 2 : depth == 1 ? 1 : 0;
    nl_read(tpx + (umin >> sh), tpy + vmin, tpx + (umax >> sh), tpy + vmax, 0);
    if (depth < 2) nl_read(clutx, cluty, clutx + (depth == 0 ? 15 : 255), cluty, 1);
}

/* the emulated GPU time tri_t charges for this triangle at 1x (same rules) */
static void tri_cost(const St *st, Vtx a, Vtx b, Vtx c) {
    int64_t area = (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(c.x - a.x) * (b.y - a.y);
    if (area == 0) return;
    int minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    if (minx < st->dx1) minx = st->dx1;
    if (miny < st->dy1) miny = st->dy1;
    if (maxx > st->dx2) maxx = st->dx2;
    if (maxy > st->dy2) maxy = st->dy2;
    if (minx > maxx || miny > maxy) return;
    cpu.cycles += (uint32_t)((maxx - minx + 1) * (maxy - miny + 1)) / 8;
}

/* GPU path: the native copy of one prim with bounding box x0..x1, y0..y1. Returns the number of hot pieces to draw
   (their clipped states in out[]); 0 = GPU only. Mode 0 returns the whole prim as one piece. */
static int nat_pieces(const St *st, int x0, int y0, int x1, int y1, St out[HOT_MAX]) {
    int m = native_mode();
    if (m == 0) { out[0] = *st; return 1; }
    if (x0 < st->dx1) x0 = st->dx1;
    if (y0 < st->dy1) y0 = st->dy1;
    if (x1 > st->dx2) x1 = st->dx2;
    if (y1 > st->dy2) y1 = st->dy2;
    if (x0 > x1 || y0 > y1) return 0;
    nl_set(x0, y0, x1, y1, 1);
    if (m == 1) return 0;
    int n = 0;
    for (int i = 0; i < hot_n; i++) {
        const int *r = hot_r[i];
        if (x1 < r[0] || x0 > r[2] || y1 < r[1] || y0 > r[3]) continue;
        St p = *st;
        if (p.dx1 < r[0]) p.dx1 = r[0];
        if (p.dy1 < r[1]) p.dy1 = r[1];
        if (p.dx2 > r[2]) p.dx2 = r[2];
        if (p.dy2 > r[3]) p.dy2 = r[3];
        out[n++] = p;
    }
    return n;
}

/* Repeated triangles (opt.dupfix). Some models draw a face twice in one frame (a toy store stand: its side face as a
   triangle and again as half of a quad). The game's sort can put other polygons between the two copies (the
   stand's books), and when the camera angle tips the sort the second copy paints over them for single frames.
   An exact repeat (same screen corners, texture coordinates, colours, CLUT and texture page) of an opaque triangle
   already drawn since the drawing area was last set is left out of the hi-res picture; native VRAM stays exact. */
static int tri_hr_skip;          /* set by exec_poly for one tri(): native only */
static uint32_t dup_gen = 1;     /* new frame: drawing area set (GP0 E3) */
static int hud_3d;               /* polygons with world corners since the drawing area was set (HUD protection) */
static struct { uint64_t key; uint32_t gen; } dup_tab[16384];
static int dup_seen(uint64_t key) {     /* 1 = drawn before in this frame; else remembers it */
    uint32_t h = (uint32_t)(key ^ key >> 29) * 2654435761u >> 18;
    for (int p = 0; p < 8; p++) {
        uint32_t i = (h + p) & 16383;
        if (dup_tab[i].gen != dup_gen) { dup_tab[i].key = key; dup_tab[i].gen = dup_gen; return 0; }
        if (dup_tab[i].key == key) return 1;
    }
    return 0;   /* table crowded: draw it */
}
static uint64_t dup_mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0xBF58476D1CE4E5B9ull; }
/* key of one triangle of a polygon packet: its three corners (xy word, uv, colour) in any order + texture state */
static uint64_t dup_key(const uint32_t *f, const int *vi, const uint32_t *vc, int i0, int i1, int i2, int tex, uint32_t tstate) {
    uint64_t c[3];
    const int ix[3] = { i0, i1, i2 };
    for (int k = 0; k < 3; k++) {
        int j = ix[k];
        c[k] = (uint64_t)f[vi[j]] << 32 ^ (uint64_t)(tex ? (f[vi[j] + 1] & 0xFFFF) : 0) << 24 ^ (vc[j] & 0xFFFFFF);
    }
    if (c[0] > c[1]) { uint64_t t = c[0]; c[0] = c[1]; c[1] = t; }
    if (c[1] > c[2]) { uint64_t t = c[1]; c[1] = c[2]; c[2] = t; }
    if (c[0] > c[1]) { uint64_t t = c[0]; c[0] = c[1]; c[1] = t; }
    uint64_t h = dup_mix(dup_mix(dup_mix(0x1234567ull, c[0]), c[1]), c[2]);
    return dup_mix(h, tstate);
}

/* HUD overlay candidates (2D polygons after the 3D scene) must also look like HUD, not like world sprites (falling
   flower petals, a spinning cookie pickup: flat quads around a projected point): drawn at exactly the same screen
   corners (and texture) for the last HUD_FRAMES frames (frames = display flips, subpx_gen; the game sets the drawing
   area several times per frame), and the centre is not a projected point. */
static struct { uint64_t key; uint32_t gen; int ok; } hud_tab[4096];   /* ok: frames in a row it was drawn before */
#define HUD_FRAMES 30   /* a falling petal can hold still for a few frames; HUD stays for seconds */
static int hud_like(const uint32_t *f, const int *vi, int nv, int tex) {
    /* HUD pieces are untinted textured rectangles on the screen axes (the cookie box: 2c808080, corners on two x and
       two y values). World sprites are tinted by distance and skewed by perspective (the pie gallery's pies:
       2cebebeb / 2c727272, corners a pixel apart): those stay ordinary 2D. */
    if (!tex || nv != 4) return 0;
    if ((f[0] & 0x01000000) == 0 && (f[0] & 0xFFFFFF) != 0x808080) {   /* not raw texture: must be the neutral tint */
        if (gpu_cmdlog) rt_logf("   hud? no: tinted %06x\n", f[0] & 0xFFFFFF);
        return 0;
    }
    {
        int x[4], y[4];
        for (int k = 0; k < 4; k++) { x[k] = (int16_t)(f[vi[k]] & 0xFFFF); y[k] = (int16_t)(f[vi[k]] >> 16); }
        int rect = (x[0] == x[2] && x[1] == x[3] && y[0] == y[1] && y[2] == y[3]) ||
                   (x[0] == x[1] && x[2] == x[3] && y[0] == y[2] && y[1] == y[3]);
        if (!rect) {
            if (gpu_cmdlog) rt_logf("   hud? no: not a screen-aligned rectangle\n");
            return 0;
        }
    }
    uint64_t key = 0x5bd1e995ull;
    int sx = 0, sy = 0;
    for (int k = 0; k < nv; k++) {
        key = dup_mix(key, (uint64_t)f[vi[k]] << 16 ^ (tex ? f[vi[k] + 1] : 0));
        sx += (int16_t)(f[vi[k]] & 0xFFFF); sy += (int16_t)(f[vi[k]] >> 16);
    }
    {   /* corners projected by the GTE: a world sprite (the Reptar bars: camera-facing squares whose corners the game
           projects one by one, untinted and screen-aligned, so they passed every other test and showed through walls).
           Two or more, since one HUD corner once shared a pixel with a world point by chance (the cookie box). */
        int np = 0;
        for (int k = 0; k < nv; k++) np += subpx_find(f[vi[k]]) >= 0;
        if (np >= 2) {
            if (gpu_cmdlog) rt_logf("   hud? no: %d corners are projected points\n", np);
            return 0;
        }
    }
    for (int dy = 0; dy < 2; dy++)   /* centre (both roundings) on a projected point: a world sprite */
        for (int dx = 0; dx < 2; dx++) {
            int cx = (sx + dx * (nv - 1)) / nv, cy = (sy + dy * (nv - 1)) / nv;
            if (subpx_find((uint32_t)(uint16_t)cy << 16 | (uint16_t)cx) >= 0) {
                if (gpu_cmdlog) rt_logf("   hud? no: centre %d,%d is a projected point\n", cx, cy);
                return 0;
            }
        }
    uint32_t h = (uint32_t)(key ^ key >> 31) * 2654435761u >> 20;
    int found = -1, fr = -1;
    for (int p = 0; p < 4; p++) {
        int i = (int)((h + p) & 4095);
        if (hud_tab[i].key == key && subpx_gen - hud_tab[i].gen <= 1) { found = i; break; }
        if (fr < 0 && subpx_gen - hud_tab[i].gen > 1) fr = i;
    }
    if (found >= 0) {
        if (hud_tab[found].gen != subpx_gen) { hud_tab[found].ok++; hud_tab[found].gen = subpx_gen; }   /* last frame too */
        if (gpu_cmdlog && hud_tab[found].ok < HUD_FRAMES) rt_logf("   hud? no: same place only %d frames\n", hud_tab[found].ok + 1);
        return hud_tab[found].ok >= HUD_FRAMES;
    }
    if (gpu_cmdlog) rt_logf("   hud? no: new this frame (gen %u)\n", subpx_gen);
    if (fr < 0) fr = (int)(h & 4095);   /* crowded: reuse */
    hud_tab[fr].key = key; hud_tab[fr].gen = subpx_gen; hud_tab[fr].ok = 0;
    return 0;
}

/* Glass (objsort + RUGRATS_GLASSFIX, default 1): see-through object faces (semi-transparent, DM_TEST: the toy store's
   glass cases) are drawn into the hi-res picture at the END of the drawing list instead of where the game's sort put
   them. A case's glass wall and the room floor piece it stands on sort as a tie (same far corner); when the stick
   camera moved, the floor came after the glass in the list and painted over it, so glass sheets went missing / popped
   back in. Native VRAM is drawn in the game's order as always. At the end every room piece has stored its depth, so the
   glass is still hidden behind walls (room depth test), and where ANY model stored a nearer depth (oz: characters,
   props in front of it) it is left out too (polygon bit OID_GLASS, ps_to / tri_t). */
static int glass_cur;             /* exec_poly: the current polygon's hi-res triangles go to glass_q */
typedef struct { St st; Vtx a, b, c; int textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth; } GlassTri;
static GlassTri glass_q[4096];
static int glass_n;
static int glass_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_GLASSFIX"); on = e ? atoi(e) : 1; }
    return on && opt.objsort;
}
static void hr_tri_emit(const St *st, Vtx a, Vtx b, Vtx c, int textured, int gouraud, int semi_on, int raw, int clutx,
                        int cluty, int tpx, int tpy, int depth) {   /* the hi-res part of tri() */
    if (hw_s) { hw_tri(st, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth); return; }
    if (!hr.buf) return;
    HCmd h = { HC_TRI, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth, { a, b, c } };
    h.st = *st;
    if (textured) tex_note(tpx, tpy, depth, clutx, cluty);
    hq_push(&h);
}
void gpu_glass_flush(void) {   /* end of a DMA list, and before fills / VRAM transfers / a new drawing area / a flip */
    const int n = glass_n;
    const int sq = sq_on;
    sq_on = 0;                    /* held-back world glass: never squeezed (Stretch 2D) */
    glass_n = 0;
    for (int i = 0; i < n; i++) {
        const GlassTri *q = &glass_q[i];
        hr_tri_emit(&q->st, q->a, q->b, q->c, q->textured, q->gouraud, q->semi_on, q->raw, q->clutx, q->cluty, q->tpx,
                    q->tpy, q->depth);
    }
    sq_on = sq;
}
static void glass_push(const St *st, Vtx a, Vtx b, Vtx c, int textured, int gouraud, int semi_on, int raw, int clutx,
                       int cluty, int tpx, int tpy, int depth) {
    if (glass_n == (int)(sizeof glass_q / sizeof glass_q[0])) gpu_glass_flush();
    GlassTri *q = &glass_q[glass_n++];
    q->st = *st; q->a = a; q->b = b; q->c = c; q->textured = textured; q->gouraud = gouraud; q->semi_on = semi_on;
    q->raw = raw; q->clutx = clutx; q->cluty = cluty; q->tpx = tpx; q->tpy = tpy; q->depth = depth;
}
/* 2D overlap of two convex polygons (separating axis), native coordinates. mpx = +1: touching within 1 px counts as
   overlap; -1: must overlap by more than ~1 px (glass sheets sharing an edge do not count) */
static int glass_sep(const int (*p)[2], int np, const int (*q)[2], int nq, int mpx) {
    for (int i = 0; i < np; i++) {
        const int *a = p[i], *b = p[(i + 1) % np];
        const int64_t nx = b[1] - a[1], ny = a[0] - b[0];
        if (!nx && !ny) continue;
        int64_t pmin = INT64_MAX, pmax = INT64_MIN, qmin = INT64_MAX, qmax = INT64_MIN;
        for (int k = 0; k < np; k++) { const int64_t d = nx * p[k][0] + ny * p[k][1]; if (d < pmin) pmin = d; if (d > pmax) pmax = d; }
        for (int k = 0; k < nq; k++) { const int64_t d = nx * q[k][0] + ny * q[k][1]; if (d < qmin) qmin = d; if (d > qmax) qmax = d; }
        const int64_t m = ((nx < 0 ? -nx : nx) + (ny < 0 ? -ny : ny)) * mpx;   /* ~1 px */
        if (qmax < pmin - m || qmin > pmax + m) return 1;
    }
    return 0;
}
static int glass_hit(const int (*p)[2], int np, const int (*q)[2], int nq, int mpx) {
    return !glass_sep(p, np, q, nq, mpx) && !glass_sep(q, nq, p, np, mpx);
}
/* exec_poly, a polygon that must keep the game's order against held glass: draw the held glass it touches on screen
   first, plus any glass held before one of those that really overlaps it (so overlapping sheets keep their order).
   Glass elsewhere stays held: a prop triangle inside a case (other side of the screen) used to release ALL glass
   (later: all glass held before the one it touched), and the case floor drawn after it painted over the glass again
   (red case floor showing as bare triangles, 2026-11-12). RUGRATS_GLASSFIX=2: release all (first rule).
   A polygon with real depth (DM_TEST) lying wholly behind a glass sheet's plane does not release that sheet: the
   glass drawn later just tints it (a pedestal inside the case released the sheet, then the case floor covered it;
   RUGRATS_GLASSBEHIND=0 = off). */
static int glass_side(const Vtx *v, int nv, const GlassTri *g, int front) {   /* every corner of v farther (front=0)
                                                                                 / nearer (front=1) than g's plane */
    const double x1 = g->b.x - g->a.x, y1 = g->b.y - g->a.y, x2 = g->c.x - g->a.x, y2 = g->c.y - g->a.y;
    const double det = x1 * y2 - x2 * y1;
    if (det == 0 || !(g->a.iz > 0) || !(g->b.iz > 0) || !(g->c.iz > 0)) return 0;
    const double d1 = g->b.iz - g->a.iz, d2 = g->c.iz - g->a.iz;
    const double gx = (d1 * y2 - d2 * y1) / det, gy = (x1 * d2 - x2 * d1) / det;   /* iz = 4096/z, linear on screen */
    for (int k = 0; k < nv; k++) {
        if (!(v[k].iz > 0)) return 0;
        const double gz = g->a.iz + gx * (v[k].x - g->a.x) + gy * (v[k].y - g->a.y);
        if (front ? !(v[k].iz > gz * 1.005) : !(v[k].iz < gz * 0.995)) return 0;
    }
    return 1;
}
static int glass_behind(const Vtx *v, int nv, const GlassTri *g) { return glass_side(v, nv, g, 0); }
static int glass_infront(const Vtx *v, int nv, const GlassTri *g) { return glass_side(v, nv, g, 1); }
/* iz of the plane through a, b, c at x, y (iz linear on screen); 0 when degenerate */
static double plane_iz(const Vtx *a, const Vtx *b, const Vtx *c, double x, double y) {
    const double x1 = b->x - a->x, y1 = b->y - a->y, x2 = c->x - a->x, y2 = c->y - a->y, det = x1 * y2 - x2 * y1;
    if (det == 0 || !(a->iz > 0) || !(b->iz > 0) || !(c->iz > 0)) return 0;
    const double d1 = b->iz - a->iz, d2 = c->iz - a->iz;
    return a->iz + (d1 * y2 - d2 * y1) / det * (x - a->x) + (x1 * d2 - x2 * d1) / det * (y - a->y);
}
static int tri_inside(const Vtx *a, const Vtx *b, const Vtx *c, double x, double y) {
    const double d0 = (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
    const double d1 = (c->x - b->x) * (y - b->y) - (c->y - b->y) * (x - b->x);
    const double d2 = (a->x - c->x) * (y - c->y) - (a->y - c->y) * (x - c->x);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}
/* a piece (tri or GP0 quad) nearer than glass g where they overlap on screen: depths compared at the glass corners /
   centre inside the piece and the piece corners inside the glass; needs at least one such point, all nearer */
static int piece_front_of_glass(const Vtx *v, int nv, const GlassTri *g) {
    const Vtx *gt[3] = { &g->a, &g->b, &g->c };
    double sx[8], sy[8];
    int n = 0;
    for (int k = 0; k < 3; k++) { sx[n] = gt[k]->x; sy[n] = gt[k]->y; n++; }
    sx[n] = (g->a.x + g->b.x + g->c.x) / 3.0; sy[n] = (g->a.y + g->b.y + g->c.y) / 3.0; n++;
    int used = 0;
    for (int s = 0; s < n + nv; s++) {
        double x, y;
        if (s < n) { x = sx[s]; y = sy[s]; } else { x = v[s - n].x; y = v[s - n].y; if (!tri_inside(gt[0], gt[1], gt[2], x, y)) continue; }
        /* which piece triangle holds the point (quad = 0,1,2 + 1,3,2) */
        const Vtx *p0 = &v[0], *p1 = &v[1], *p2 = &v[2];
        if (!tri_inside(p0, p1, p2, x, y)) {
            if (nv < 4 || !tri_inside(&v[1], &v[3], &v[2], x, y)) continue;
            p0 = &v[1]; p1 = &v[3]; p2 = &v[2];
        }
        const double pz = plane_iz(p0, p1, p2, x, y), gz = plane_iz(gt[0], gt[1], gt[2], x, y);
        if (!(pz > 0) || !(gz > 0)) return 0;
        if (!(pz > gz * 1.005)) return 0;
        used++;
    }
    return used > 0;
}
static int glass_wnt_on(void) {   /* write-notest room pieces release overlapping held glass (exec_poly) */
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_GLASSWNT"); on = e ? atoi(e) != 0 : 1; }
    return on;
}
static void glass_flush_over(const Vtx *v, int nv, int dm) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("RUGRATS_GLASSFIX"); mode = e ? atoi(e) : 1; }
    if (mode == 3) return;   /* debug: glass only at the list end etc. (reference for regression checks) */
    static int behind_on = -1;
    if (behind_on < 0) { const char *e = getenv("RUGRATS_GLASSBEHIND"); behind_on = e ? atoi(e) != 0 : 1; }
    if (mode == 2 || nv < 3) { gpu_glass_flush(); return; }
    int p[4][2];
    static const int ord4[4] = { 0, 1, 3, 2 };   /* GP0 quad corners -> outline order */
    for (int k = 0; k < nv; k++) { const Vtx *s = &v[nv == 4 ? ord4[k] : k]; p[k][0] = s->x; p[k][1] = s->y; }
    const int n = glass_n;
    if (n > 1024) { gpu_glass_flush(); return; }   /* pathological: keep it cheap */
    static unsigned char mk[1024];
    static int tq[1024][3][2];
    int any = 0;
    for (int i = 0; i < n; i++) {
        const GlassTri *g3 = &glass_q[i];
        tq[i][0][0] = g3->a.x; tq[i][0][1] = g3->a.y; tq[i][1][0] = g3->b.x; tq[i][1][1] = g3->b.y;
        tq[i][2][0] = g3->c.x; tq[i][2][1] = g3->c.y;
        mk[i] = (unsigned char)glass_hit(p, nv, tq[i], 3, 1);
        if (mk[i] && dm == DM_TEST && behind_on && glass_behind(v, nv, g3)) mk[i] = 0;
        /* write-notest room pieces release only glass they lie wholly in front of (a corridor wall in front of far
           engine-room glass draws over it, game order; a case floor touching its glass wall keeps the glass held) */
        if (mk[i] && dm == DM_WNT) {   /* depth compared where they overlap (long wall pieces span near..far) */
            const int front = piece_front_of_glass(v, nv, g3);
            if (gpu_cmdlog) rt_logf("   glass: wnt piece %s\n", front ? "in front: glass drawn first" : "not in front: glass kept");
            if (!front) mk[i] = 0;
        }
        if (mk[i] && gpu_cmdlog)
            rt_logf("   glass: hits held (%d,%d %.3f) (%d,%d %.3f) (%d,%d %.3f) dm %d, own iz %.3f %.3f %.3f\n", g3->a.x,
                    g3->a.y, g3->a.iz, g3->b.x, g3->b.y, g3->b.iz, g3->c.x, g3->c.y, g3->c.iz, dm, v[0].iz, v[1].iz,
                    v[2].iz);
        any |= mk[i];
    }
    if (!any) return;
    for (int i = n - 1; i > 0; i--)   /* earlier glass under / over a released one goes first too */
        if (mk[i])
            for (int j = 0; j < i; j++)
                if (!mk[j] && glass_hit(tq[i], 3, tq[j], 3, -1)) {
                    /* a sheet wholly in front of the released one stays held: drawing it later is the right order
                       anyway (front case glass held before the back glass got pulled out with it, 2026-11-12) */
                    const GlassTri *gj = &glass_q[j];
                    const Vtx vj[3] = { gj->a, gj->b, gj->c };
                    if (behind_on && glass_infront(vj, 3, &glass_q[i])) continue;
                    mk[j] = 1;
                }
    int out = 0, kept = 0;
    for (int i = 0; i < n; i++) {
        const GlassTri *q = &glass_q[i];
        if (mk[i]) {
            hr_tri_emit(&q->st, q->a, q->b, q->c, q->textured, q->gouraud, q->semi_on, q->raw, q->clutx, q->cluty,
                        q->tpx, q->tpy, q->depth);
            out++;
        } else {
            if (kept != i) glass_q[kept] = *q;
            kept++;
        }
    }
    glass_n = kept;
    if (gpu_cmdlog) rt_logf("   glass: %d held triangles drawn first (%d stay held)\n", out, kept);
}

static int tri_nat_skip;          /* set by clamp_draw for its tri() calls: hi-res only (native keeps the game's polygon) */
static void tri(Vtx a, Vtx b, Vtx c, int textured, int gouraud, int semi_on, int raw, int clutx, int cluty,
                int tpx, int tpy, int depth) {
    St st = cur_st();
    if (tri_nat_skip) {
        if (glass_cur && (hw_s || hr.buf)) { glass_push(&st, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth); return; }
        hr_tri_emit(&st, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        return;
    }
    if (hw_s) {
        int x0 = a.x, x1 = a.x, y0 = a.y, y1 = a.y, u0 = a.u, u1 = a.u, v0 = a.v, v1 = a.v;
        const Vtx *q[2] = { &b, &c };
        for (int k = 0; k < 2; k++) {
            if (q[k]->x < x0) x0 = q[k]->x;
            if (q[k]->x > x1) x1 = q[k]->x;
            if (q[k]->y < y0) y0 = q[k]->y;
            if (q[k]->y > y1) y1 = q[k]->y;
            if (q[k]->u < u0) u0 = q[k]->u;
            if (q[k]->u > u1) u1 = q[k]->u;
            if (q[k]->v < v0) v0 = q[k]->v;
            if (q[k]->v > v1) v1 = q[k]->v;
        }
        if (textured) nl_tex(tpx, tpy, depth, u0, v0, u1, v1, clutx, cluty);
        St pc[HOT_MAX];
        int n = x1 - x0 > 1023 || y1 - y0 > 511 ? 0 : nat_pieces(&st, x0, y0, x1, y1, pc);
        if (native_mode() == 0) {
            hw_guard(st.dx1, st.dy1, st.dx2, st.dy2);
            tri_t(&st, &nat, 0, 1, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        } else {
            for (int i = 0; i < n; i++) {
                hw_guard(pc[i].dx1, pc[i].dy1, pc[i].dx2, pc[i].dy2);
                nat_nocost = 1;
                tri_t(&pc[i], &nat, 0, 1, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth);
                nat_nocost = 0;
            }
            tri_cost(&st, a, b, c);
        }
        if (!tri_hr_skip) {
            if (glass_cur) glass_push(&st, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth);
            else hw_tri(&st, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        }
        return;
    }
    if (hr.buf) native_guard();
    tri_t(&st, &nat, 0, 1, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth);
    if (!hr.buf || tri_hr_skip) return;
    if (glass_cur) { glass_push(&st, a, b, c, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth); return; }
    HCmd h = { HC_TRI, textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth, { a, b, c } };
    h.st = st;
    if (textured) tex_note(tpx, tpy, depth, clutx, cluty);
    hq_push(&h);
}

/* True geometry for a wall piece with a clamped corner (RUGRATS_CLAMPPOS=0: off; hi-res only). The GTE pins points
   past the screen limit (or behind the camera) to +-1024, so such a piece is drawn with corners where its vertices are
   not: the wall's outline near the camera is wrong, and the game's 2D splits of it inherit that. Every corner's true
   camera-space point travels with the tags (split points: the 3D midpoint of their ends), so the triangle is clipped in
   3D against the camera (z >= 1) and a guard band just outside the drawing area, then projected exactly; each clipped
   corner gets uv, colour and 1/z from its 3D position (uv is affine on the polygon in 3D). Native VRAM keeps the
   game's polygon (exec_poly draws it native-only). */
/* depth slope of a triangle per native pixel, from its exact (sub-pixel) positions: max |d(1/z)/dx|, |d(1/z)/dy| */
static float tri_slope(const Vtx *a, const Vtx *b, const Vtx *c) {
    const double ax = a->x + a->fx / 256.0, ay = a->y + a->fy / 256.0, bx = b->x + b->fx / 256.0, by = b->y + b->fy / 256.0;
    const double cx = c->x + c->fx / 256.0, cy = c->y + c->fy / 256.0;
    const double area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (fabs(area) < 1e-6) return 0;
    const double gx = ((b->iz - a->iz) * (cy - ay) - (c->iz - a->iz) * (by - ay)) / area;
    const double gy = ((c->iz - a->iz) * (bx - ax) - (b->iz - a->iz) * (cx - ax)) / area;
    const double m = fabs(gx) > fabs(gy) ? fabs(gx) : fabs(gy);
    return m > 0 ? (float)m : 1e-12f;
}
#define CP_MAX 10
typedef struct { double p[3], u, v, r, g, b; } CPt;
static int cp_clip(CPt *in, int n, CPt *out, double a0, double a1, double a2, double c) {   /* keep a.p + c <= 0 */
    int m = 0;
    for (int i = 0; i < n; i++) {
        const CPt *A = &in[i], *B = &in[(i + 1) % n];
        const double fa = a0 * A->p[0] + a1 * A->p[1] + a2 * A->p[2] + c, fb = a0 * B->p[0] + a1 * B->p[1] + a2 * B->p[2] + c;
        if (fa <= 0 && m < CP_MAX) out[m++] = *A;
        if ((fa < 0 && fb > 0) || (fa > 0 && fb < 0)) {
            const double t = fa / (fa - fb);
            CPt q;
            for (int k = 0; k < 3; k++) q.p[k] = A->p[k] + (B->p[k] - A->p[k]) * t;
            q.u = A->u + (B->u - A->u) * t; q.v = A->v + (B->v - A->v) * t;
            q.r = A->r + (B->r - A->r) * t; q.g = A->g + (B->g - A->g) * t; q.b = A->b + (B->b - A->b) * t;
            if (m < CP_MAX) out[m++] = q;
        }
    }
    return m;
}
static int bigtri(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_BIGTRI"); z = e ? atoi(e) : 1; } return z; }
static int clamppos(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_CLAMPPOS"); z = e ? atoi(e) : 1; } return z; }
/* one triangle: src = the game's corners (attributes, depth class), P / U / V = their true points and texture coords */
static void clamp_draw(const Vtx *const src[3], const double P[3][3], const double U[3], const double V[3], int textured,
                       int gouraud, int semi_on, int raw, int clutx, int cluty, int tpx, int tpy, int depth) {
    CPt a[CP_MAX], b[CP_MAX];
    int n = 3;
    for (int k = 0; k < 3; k++) {
        for (int c = 0; c < 3; c++) a[k].p[c] = P[k][c];
        a[k].u = U[k]; a[k].v = V[k]; a[k].r = src[k]->r; a[k].g = src[k]->g; a[k].b = src[k]->b;
    }
    double ox, oy, hh, ww;
    gte_proj_params(&ox, &oy, &hh, &ww);
    const St st = cur_st();
    const double bx = g.offx + ox, by = g.offy + oy;   /* VRAM x = bx + hh*ww*x/z, y = by + hh*y/z */
    const double x0 = st.dx1 - 4, x1 = st.dx2 + 5, y0 = st.dy1 - 4, y1 = st.dy2 + 5;
    n = cp_clip(a, n, b, 0, 0, -1, 1.0);                       /* z >= 1 */
    n = cp_clip(b, n, a, hh * ww, 0, bx - x1, 0);              /* X <= x1 */
    n = cp_clip(a, n, b, -hh * ww, 0, x0 - bx, 0);             /* X >= x0 */
    n = cp_clip(b, n, a, 0, hh, by - y1, 0);                   /* Y <= y1 */
    n = cp_clip(a, n, b, 0, -hh, y0 - by, 0);                  /* Y >= y0 */
    if (n < 3) return;
    int umn = src[0]->u, umx = src[0]->u, vmn = src[0]->v, vmx = src[0]->v;   /* the game's texel box */
    for (int k = 1; k < 3; k++) {
        if (src[k]->u < umn) umn = src[k]->u;
        if (src[k]->u > umx) umx = src[k]->u;
        if (src[k]->v < vmn) vmn = src[k]->v;
        if (src[k]->v > vmx) vmx = src[k]->v;
    }
    Vtx o[CP_MAX];
    float zmax = 0;
    for (int i = 0; i < n; i++) {
        const CPt *q = &b[i];
        const double sx = bx + hh * ww * q->p[0] / q->p[2], sy = by + hh * q->p[1] / q->p[2];
        Vtx *w = &o[i];
        *w = *src[0];
        w->x = (int)floor(sx); w->fx = (int)floor((sx - w->x) * 256.0 + 0.5);
        w->y = (int)floor(sy); w->fy = (int)floor((sy - w->y) * 256.0 + 0.5);
        double uu = q->u, vv = q->v;
        if (uu < umn) uu = umn;
        if (uu > umx + 0.999) uu = umx + 0.999;
        if (vv < vmn) vv = vmn;
        if (vv > vmx + 0.999) vv = vmx + 0.999;
        w->u = (int)floor(uu); w->v = (int)floor(vv); w->uf = (float)(uu - w->u); w->vf = (float)(vv - w->v);
        w->r = (int)lround(q->r); w->g = (int)lround(q->g); w->b = (int)lround(q->b);
        w->iz = w->izn = (float)(4096.0 / q->p[2]);
        w->pw = textured && opt.perspective ? w->iz : 0;
        if (w->iz > zmax) zmax = w->iz;
    }
    const float zl = (src[0]->dm == DM_WRITE || src[0]->dm == DM_WNT) ? depth_reach(zmax) : 0;
    for (int i = 0; i < n; i++) o[i].zl = zl;
    {   /* CPU renderer's slope bias from the biggest piece (see tri_t) */
        double best = -1; float zs = 0;
        for (int i = 1; i + 1 < n; i++) {
            const double ar = fabs((double)(o[i].x - o[0].x + (o[i].fx - o[0].fx) / 256.0) * (o[i + 1].y - o[0].y + (o[i + 1].fy - o[0].fy) / 256.0) -
                                   (double)(o[i + 1].x - o[0].x + (o[i + 1].fx - o[0].fx) / 256.0) * (o[i].y - o[0].y + (o[i].fy - o[0].fy) / 256.0));
            if (ar > best) { best = ar; zs = tri_slope(&o[0], &o[i], &o[i + 1]); }
        }
        if (!(zs > 0)) zs = 1e-12f;   /* > 0 also marks a hi-res-only piece */
        for (int i = 0; i < n; i++) o[i].zs = zs;
    }
    if (gpu_cmdlog) {
        rt_logf("   clamp_draw: %d corners", n);
        for (int i = 0; i < n; i++) rt_logf(" (%.2f,%.2f uv %.2f,%.2f w%.3f)", o[i].x - g.offx + o[i].fx / 256.0, o[i].y - g.offy + o[i].fy / 256.0,
                                            o[i].u + o[i].uf, o[i].v + o[i].vf, o[i].iz);
        rt_logf("\n");
    }
    tri_nat_skip = 1;
    for (int i = 1; i + 1 < n; i++) tri(o[0], o[i], o[i + 1], textured, gouraud, semi_on, raw, clutx, cluty, tpx, tpy, depth);
    tri_nat_skip = 0;
}

static void rect(int x0, int y0, int w, int h, int r, int gg, int b, int textured, int semi_on, int raw,
                 int u0, int v0, int clutx, int cluty) {
    St st = cur_st();
    if (hw_s) {
        if (textured) nl_tex(st.tpx, st.tpy, st.tdepth, u0, v0, (u0 + w - 1) & 255, (v0 + h - 1) & 255, clutx, cluty);
        St pc[HOT_MAX];
        int n = nat_pieces(&st, x0, y0, x0 + w - 1, y0 + h - 1, pc);
        if (native_mode() == 0) {
            hw_guard(st.dx1, st.dy1, st.dx2, st.dy2);
            rect_t(&st, &nat, 0, 1, x0, y0, w, h, r, gg, b, textured, semi_on, raw, u0, v0, clutx, cluty);
        } else {
            for (int i = 0; i < n; i++) {
                hw_guard(pc[i].dx1, pc[i].dy1, pc[i].dx2, pc[i].dy2);
                nat_nocost = 1;
                rect_t(&pc[i], &nat, 0, 1, x0, y0, w, h, r, gg, b, textured, semi_on, raw, u0, v0, clutx, cluty);
                nat_nocost = 0;
            }
            cpu.cycles += (uint32_t)(w * h) / 8;
        }
        hw_rect(&st, x0, y0, w, h, r, gg, b, textured, semi_on, raw, u0, v0, clutx, cluty);
        return;
    }
    if (hr.buf) native_guard();
    rect_t(&st, &nat, 0, 1, x0, y0, w, h, r, gg, b, textured, semi_on, raw, u0, v0, clutx, cluty);
    if (!hr.buf) return;
    HCmd c = { HC_RECT, textured, 0, semi_on, raw, clutx, cluty, st.tpx, st.tpy, st.tdepth };
    c.v[0].x = x0; c.v[0].y = y0; c.v[0].u = u0; c.v[0].v = v0;
    c.w = w; c.h = h; c.r = r; c.gg = gg; c.b = b;
    c.st = st;
    if (textured) tex_note(st.tpx, st.tpy, st.tdepth, clutx, cluty);
    hq_push(&c);
}

static void line(Vtx a, Vtx b, int gouraud, int semi_on) {
    St st = cur_st();
    if (hw_s) {
        St pc[HOT_MAX];
        int n = nat_pieces(&st, a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.x > b.x ? a.x : b.x,
                           a.y > b.y ? a.y : b.y, pc);
        if (native_mode() == 0) {
            hw_guard(st.dx1, st.dy1, st.dx2, st.dy2);
            line_t(&st, &nat, 0, 1, a, b, gouraud, semi_on);
        } else {
            for (int i = 0; i < n; i++) {
                hw_guard(pc[i].dx1, pc[i].dy1, pc[i].dx2, pc[i].dy2);
                line_t(&pc[i], &nat, 0, 1, a, b, gouraud, semi_on);
            }
        }
        hw_line(&st, a, b, gouraud, semi_on);
        return;
    }
    if (hr.buf) native_guard();
    line_t(&st, &nat, 0, 1, a, b, gouraud, semi_on);
    if (!hr.buf) return;
    HCmd c = { HC_LINE, 0, gouraud, semi_on };
    c.v[0] = a; c.v[1] = b;
    c.st = st;
    hq_push(&c);
}

/* ---- command handling ----------------------------------------------- */
static int cmd_len(uint32_t c) {
    int op = c >> 24;
    if (op >= 0x20 && op < 0x40) {
        int quad = op & 8, tex = op & 4, gour = op & 0x10;
        int nv = quad ? 4 : 3;
        int per = 1 + (tex ? 1 : 0) + (gour ? 1 : 0);
        return 1 + nv * per - (gour ? 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) {
        if (op & 8) return 1000; /* polyline: until terminator */
        return (op & 0x10) ? 4 : 3;
    }
    if (op >= 0x60 && op < 0x80) {
        int n = 2 + ((op & 4) ? 1 : 0);
        if (((op >> 3) & 3) == 0) n++;
        return n;
    }
    if (op == 0x02) return 3;
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xC0) return 3;
    if (op >= 0xC0 && op < 0xE0) return 3;
    return 1;
}

static void set_texpage(uint32_t v) {
    g.tpx = (v & 15) * 64;
    g.tpy = ((v >> 4) & 1) * 256;
    g.semi = (v >> 5) & 3;
    g.tdepth = (v >> 7) & 3;
    if (g.tdepth == 3) g.tdepth = 2;
    g.dither = (v >> 9) & 1;
    g.draw_disp = (v >> 10) & 1;
    g.rect_flipx = (v >> 12) & 1;
    g.rect_flipy = (v >> 13) & 1;
}

extern int gpu_cmdlog;
/* Sub-pixel crack fill (RUGRATS_SUBPXGROW=0 = off). Separate level meshes can meet up to a pixel apart in the
   distance (e.g. mini golf ground strips under the far wall); the PS1's whole-pixel snap hides that, exact positions
   show it as a crack with water / sky behind. Opaque scenery polygons (not world objects: those take the depth test;
   every corner a sub-pixel GTE point, also ground drawn by other renderers than the room one) are pushed outward by up to
   3/8 of a native pixel (each edge moves by d; both sides of a crack grow, so up to 3/4 px closes: the mini golf
   cracks measured ~0.7 px).
   Only the drawn position moves (texture coordinates stay on the corners, so no texels from outside the face). d
   ramps with each corner's depth: 0 nearer than z0, full from z1 (near cracks would show on a PS1 too, and near
   edges stay exact). Env: RUGRATS_SUBPXGROW=<d in 1/256 px>,<z0>,<z1>. */
static int grow_d = -1, grow_z0, grow_z1;
static int growpoly(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_GROWPOLY"); z = e ? atoi(e) : 1; } return z; }
/* returns 1 when the face should grow and growpoly() is on: dkv[k] = corner k's grow distance (native px); the outline
   is then grown by grow_draw instead of moving the corners here */
static int poly_grow(Vtx *v, int nv, const uint32_t *f, const int *vi, double *dkv) {
    if (grow_d < 0) {
        const char *e = getenv("RUGRATS_SUBPXGROW");
        grow_d = 96; grow_z0 = 4000; grow_z1 = 8000;
        if (e) sscanf(e, "%d,%d,%d", &grow_d, &grow_z0, &grow_z1);
        if (grow_z1 <= grow_z0) grow_z1 = grow_z0 + 1;
    }
    if (!grow_d || !opt.subpixel || !(hr.buf || hw_s)) return 0;
    float zg[4];
    for (int k = 0; k < nv; k++) if ((zg[k] = subpx_zg(f[vi[k]])) <= 0) {   /* every corner a GTE sub-pixel point */
        if (gpu_cmdlog) rt_logf("   grow: no, corner %d has no GTE sub-pixel point\n", k);
        return 0;
    }
    /* room polygons ramp by their own depth: the nearest point on the pixel can be a character's corner (castle wall
       grew less in single frames while Angelica turned, and its bricks twitched). RUGRATS_SUBPXCLS=0: off */
    if ((v[0].dm == DM_WRITE || v[0].dm == DM_WNT) && subpxcls())
        for (int k = 0; k < nv; k++) if (v[k].iz > 0) zg[k] = v[k].iz;
    static const int ring4[4] = { 0, 1, 3, 2 }, ring3[3] = { 0, 1, 2 };   /* PS1 quad corner order is a Z */
    const int *r = nv == 4 ? ring4 : ring3;
    double px[4], py[4], dk[4];
    for (int i = 0; i < nv; i++) {
        const Vtx *q = &v[r[i]];
        px[i] = q->x + q->fx / 256.0; py[i] = q->y + q->fy / 256.0;
        const double z = 4096.0 / zg[r[i]];
        double t = (z - grow_z0) / (double)(grow_z1 - grow_z0);
        dk[i] = (t < 0 ? 0 : t > 1 ? 1 : t) * grow_d / 256.0;
    }
    /* winding (and convexity for quads: a bow-tie is left alone) */
    double sgn = 0;
    for (int i = 0; i < nv; i++) {
        const int j = (i + 1) % nv, l = (i + 2) % nv;
        const double c = (px[j] - px[i]) * (py[l] - py[j]) - (py[j] - py[i]) * (px[l] - px[j]);
        if (fabs(c) < 1e-9) continue;
        if (sgn == 0) sgn = c;
        else if ((c > 0) != (sgn > 0)) return 0;
    }
    if (sgn == 0) return 0;
    if (growpoly()) {
        int any = 0;
        for (int i = 0; i < nv; i++) { dkv[r[i]] = dk[i]; any |= dk[i] > 0; }
        return any;
    }
    double nx[4], ny[4];   /* outward unit normal of edge i -> i+1 (0 for a zero-length edge) */
    for (int i = 0; i < nv; i++) {
        const int j = (i + 1) % nv;
        double ex = px[j] - px[i], ey = py[j] - py[i], len = sqrt(ex * ex + ey * ey);
        if (len < 1e-6) { nx[i] = ny[i] = 0; continue; }
        /* screen y down: for a positive (clockwise on screen) winding the outside is to the left of the edge */
        if (sgn > 0) { nx[i] = ey / len; ny[i] = -ex / len; } else { nx[i] = -ey / len; ny[i] = ex / len; }
    }
    for (int i = 0; i < nv; i++) {
        if (dk[i] <= 0) continue;
        const int h = (i + nv - 1) % nv;
        double ax = nx[h] + nx[i], ay = ny[h] + ny[i];
        const double c = 1.0 + nx[h] * nx[i] + ny[h] * ny[i];   /* 1 + cos(angle between normals) */
        double ox, oy;
        if ((nx[h] == 0 && ny[h] == 0) || (nx[i] == 0 && ny[i] == 0)) { ox = ax * dk[i]; oy = ay * dk[i]; }
        else {
            /* both edges move by exactly d (miter) when the corner is not sharp. Thin slanted ground strips have
               ~13 degree corners whose miter is ~9d long and pokes spikes into the next face (stair steps on the far
               curb); there the corner moves d in x and d in y, towards its outside (square instead of round grow),
               which still moves both long edges of a flat strip by ~d */
            const double l = sqrt(ax * ax + ay * ay);
            if (l < 1e-9) continue;
            const double m = l / (c > 1e-9 ? c : 1e-9);   /* miter length / d */
            if (m <= 2) { ox = ax / l * m * dk[i]; oy = ay / l * m * dk[i]; }
            else {
                ox = (ax > 1e-6 ? 1 : ax < -1e-6 ? -1 : 0) * dk[i];
                oy = (ay > 1e-6 ? 1 : ay < -1e-6 ? -1 : 0) * dk[i];
            }
        }
        Vtx *q = &v[r[i]];
        q->fx += (int)lround(ox * 256.0); q->fy += (int)lround(oy * 256.0);
        if (gpu_cmdlog) rt_logf("   grow: v%d by %.3f,%.3f (d %.3f)\n", r[i], ox, oy, dk[i]);
    }
    return 0;
}

/* Grown outline (crack fill, growpoly): every outer edge of the face moves out by its own distance de[i] (edge i =
   corner i -> i+1 of the ring) along its whole length. A corner is where its two moved edges meet, unless that point
   is more than 2x the distance away (thin slanted ground strips have ~13 degree tips: the meeting point is ~9 d out
   and poked spikes into the next face); there the corner is cut flat (two points, each on one moved edge). The old
   rule moved such a tip by d in x and d in y, which tilted the strip's long edge instead of moving it: cracks between
   far ground strips stayed open (mini golf, 2026-11-11). A quad grows as ONE outline (ring 0 1 3 2) that is then cut
   along its diagonal v1-v2, so both halves share the exact cut points: grown separately their diagonal edges ended
   at different points of the same line and left 1-pixel slits in the CPU renderer. Returns the number of points. */
static int grow_ring(const double *px, const double *py, int n, const double *de, double *qx, double *qy) {
    double area = 0;
    for (int i = 0; i < n; i++) { const int j = (i + 1) % n; area += px[i] * py[j] - px[j] * py[i]; }
    if (fabs(area) < 1e-6) return 0;
    double nx[4], ny[4];
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double ex = px[j] - px[i], ey = py[j] - py[i], len = sqrt(ex * ex + ey * ey);
        if (len < 1e-9) return 0;
        if (area > 0) { nx[i] = ey / len; ny[i] = -ex / len; } else { nx[i] = -ey / len; ny[i] = ex / len; }
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        const int h = (i + n - 1) % n;   /* edge h ends at corner i, edge i starts there */
        const double a = nx[h], b = ny[h], c = nx[i], d = ny[i], det = a * d - b * c;
        const double dm = de[h] > de[i] ? de[h] : de[i];
        double ox = 0, oy = 0;
        int ok = fabs(det) > 1e-9;
        if (ok) { ox = (de[h] * d - b * de[i]) / det; oy = (a * de[i] - de[h] * c) / det; ok = ox * ox + oy * oy <= 4.0 * dm * dm + 1e-12; }
        if (ok) { qx[m] = px[i] + ox; qy[m] = py[i] + oy; m++; }
        else {
            qx[m] = px[i] + a * de[h]; qy[m] = py[i] + b * de[h]; m++;
            qx[m] = px[i] + c * de[i]; qy[m] = py[i] + d * de[i]; m++;
        }
    }
    return m;
}
/* keep the part of polygon q on side sd (+1 / -1) of the line a -> b (0 = on it) */
static int clip_side(const double *qx, const double *qy, int n, double ax, double ay, double bx, double by, int sd,
                     double *ox, double *oy) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const double fi = sd * ((bx - ax) * (qy[i] - ay) - (by - ay) * (qx[i] - ax));
        const double fj = sd * ((bx - ax) * (qy[j] - ay) - (by - ay) * (qx[j] - ax));
        if (fi >= 0) { ox[m] = qx[i]; oy[m] = qy[i]; m++; }
        if ((fi > 0 && fj < 0) || (fi < 0 && fj > 0)) {
            /* same edge, same order from both sides: identical cut point in both halves */
            const double t = fabs(fi) / (fabs(fi) + fabs(fj));
            ox[m] = qx[i] + (qx[j] - qx[i]) * t; oy[m] = qy[i] + (qy[j] - qy[i]) * t; m++;
        }
    }
    return m;
}
/* the triangle's attributes at screen point (x, y) (native px, its planes extended): colour, depth, texture (u * w,
   v * w, w under perspective; kept inside the triangle's texel box) */
static void tri_attr(const Vtx *const t[3], double x, double y, Vtx *w) {
    double px[3], py[3];
    for (int i = 0; i < 3; i++) { px[i] = t[i]->x + t[i]->fx / 256.0; py[i] = t[i]->y + t[i]->fy / 256.0; }
    const double area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    const double l1 = area ? ((x - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (y - py[0])) / area : 0;
    const double l2 = area ? ((px[1] - px[0]) * (y - py[0]) - (x - px[0]) * (py[1] - py[0])) / area : 0;
    const double l[3] = { 1.0 - l1 - l2, l1, l2 };
    int umn = t[0]->u, umx = t[0]->u, vmn = t[0]->v, vmx = t[0]->v;
    for (int k = 1; k < 3; k++) {
        if (t[k]->u < umn) umn = t[k]->u;
        if (t[k]->u > umx) umx = t[k]->u;
        if (t[k]->v < vmn) vmn = t[k]->v;
        if (t[k]->v > vmx) vmx = t[k]->v;
    }
    const int persp = t[0]->pw > 0 && t[1]->pw > 0 && t[2]->pw > 0;
    *w = *t[0];
    w->x = (int)floor(x); w->fx = (int)floor((x - w->x) * 256.0 + 0.5);
    w->y = (int)floor(y); w->fy = (int)floor((y - w->y) * 256.0 + 0.5);
    double r = 0, gg = 0, bb = 0, iz = 0, izn = 0, pw = 0, uw = 0, vw = 0;
    for (int k = 0; k < 3; k++) {
        r += l[k] * t[k]->r; gg += l[k] * t[k]->g; bb += l[k] * t[k]->b;
        iz += l[k] * t[k]->iz; izn += l[k] * t[k]->izn;
        const double U = t[k]->u + (persp ? t[k]->uf : 0), V = t[k]->v + (persp ? t[k]->vf : 0);
        const double ww = persp ? t[k]->pw : 1.0;
        pw += l[k] * ww; uw += l[k] * U * ww; vw += l[k] * V * ww;
    }
    double U = uw / pw, V = vw / pw;
    if (U < umn) U = umn;
    if (U > umx + 0.999) U = umx + 0.999;
    if (V < vmn) V = vmn;
    if (V > vmx + 0.999) V = vmx + 0.999;
    if (persp) { w->u = (int)floor(U); w->v = (int)floor(V); w->uf = (float)(U - w->u); w->vf = (float)(V - w->v); w->pw = (float)pw; }
    else { w->u = (int)lround(U); w->v = (int)lround(V); w->uf = w->vf = 0; w->pw = 0; }
    if (w->u < umn) w->u = umn;
    if (w->u > umx) w->u = umx;
    if (w->v < vmn) w->v = vmn;
    if (w->v > vmx) w->v = vmx;
    w->r = (int)lround(r < 0 ? 0 : r > 255 ? 255 : r); w->g = (int)lround(gg < 0 ? 0 : gg > 255 ? 255 : gg);
    w->b = (int)lround(bb < 0 ? 0 : bb > 255 ? 255 : bb);
    w->iz = (float)iz; w->izn = (float)izn;
    w->zs = tri_slope(t[0], t[1], t[2]);   /* CPU renderer's slope bias: the source triangle's (see tri_t) */
    if (!(w->zs > 0)) w->zs = 1e-12f;   /* > 0 also marks a hi-res-only piece */
}

unsigned fp_poly;   /* RUGRATS_FRAMEPROF: polygons this present (host_win32.c) */
/* back faces the GTE let through this frame (see "Back faces behind front faces" in exec_poly): screen triangles
   keyed by their three corner words (packet format y << 16 | x, 11 bits each), any order */
enum { BF_N = 8192, BF_PROBE = 16 };
static struct { uint32_t a, b, c, gen; } bf_tab[BF_N];
static uint32_t bf_last = 0x80000000u;   /* present_count of the last note */
static void bf_key(uint32_t *a, uint32_t *b, uint32_t *c) {
    uint32_t t;
    *a &= 0x07FF07FFu; *b &= 0x07FF07FFu; *c &= 0x07FF07FFu;
    if (*a > *b) { t = *a; *a = *b; *b = t; }
    if (*b > *c) { t = *b; *b = *c; *c = t; }
    if (*a > *b) { t = *a; *a = *b; *b = t; }
}
static uint32_t bf_hash(uint32_t a, uint32_t b, uint32_t c) {
    return (uint32_t)(((a * 0x9E3779B1u) ^ (b * 0x85EBCA77u) ^ (c * 0xC2B2AE3Du)) * 0x27D4EB2Fu) >> 19;   /* 13 bits */
}
void gpu_backface_note(const int16_t sxy[3][2]) {
    uint32_t a = (uint16_t)sxy[0][1] << 16 | (uint16_t)sxy[0][0], b = (uint16_t)sxy[1][1] << 16 | (uint16_t)sxy[1][0],
             c = (uint16_t)sxy[2][1] << 16 | (uint16_t)sxy[2][0];
    bf_key(&a, &b, &c);
    const uint32_t h = bf_hash(a, b, c);
    int best = -1; uint32_t oldest = 0;
    for (int i = 0; i < BF_PROBE; i++) {
        const int j = (int)((h + (uint32_t)i) & (BF_N - 1));
        const uint32_t age = present_count - bf_tab[j].gen;
        if (bf_tab[j].a == a && bf_tab[j].b == b && bf_tab[j].c == c) { best = j; break; }
        if (age > 2u) { best = j; break; }
        if (best < 0 || age > oldest) { best = j; oldest = age; }
    }
    bf_tab[best].a = a; bf_tab[best].b = b; bf_tab[best].c = c; bf_tab[best].gen = present_count;
    bf_last = present_count;
}
static int bf_seen(uint32_t a, uint32_t b, uint32_t c) {
    bf_key(&a, &b, &c);
    const uint32_t h = bf_hash(a, b, c);
    for (int i = 0; i < BF_PROBE; i++) {
        const int j = (int)((h + (uint32_t)i) & (BF_N - 1));
        if (bf_tab[j].a == a && bf_tab[j].b == b && bf_tab[j].c == c && present_count - bf_tab[j].gen <= 2u) return 1;
    }
    return 0;
}
static void exec_poly(uint32_t *f) {
    int op = f[0] >> 24;
    poly_seq++; fp_poly++;
    if (poly_id_vis < 0) poly_id_vis = getenv("RUGRATS_POLYID") != NULL;
    if (gpu_cmdlog) rt_logf("   #%u %04x\n", poly_seq, (unsigned)((poly_seq * 2654435761u) >> 8) & 0x7FFF);
    int quad = op & 8, tex = op & 4, gour = op & 0x10, semi_on = op & 2, raw = op & 1;
    int nv = quad ? 4 : 3;
    Vtx v[4];
    for (int k = 0; k < 4; k++) { v[k].oid = 0; v[k].izn = 0; v[k].pw = 0; v[k].seq = poly_seq; v[k].uf = v[k].vf = 0; v[k].zs = 0; }
    int vi[4], tagged[4] = { 0, 0, 0, 0 };
    float tiz[4] = { -1.0f, -1.0f, -1.0f, -1.0f };   /* exact depth from the tag (perspective weight) */
    TagP cp3[4] = { { { 0 }, 0 }, { { 0 }, 0 }, { { 0 }, 0 }, { { 0 }, 0 } };   /* camera-space point from the tag */
    const uint32_t *fa = f == g.fifo ? gfa : NULL;   /* source addresses (exact positions, see subpx_tag) */
    uint32_t vc[4];
    int i = 0, clutx = 0, cluty = 0, tpx = g.tpx, tpy = g.tpy, depth = g.tdepth;
    uint32_t col = f[0];
    for (int k = 0; k < nv; k++) {
        if (gour && k > 0) col = f[++i];
        if (k == 0) i = 0;
        uint32_t xy = f[++i];
        int uvf_want = 0;
        vi[k] = i;
        vc[k] = col;
        v[k].x = sext11(xy) + g.offx;
        v[k].y = sext11(xy >> 16) + g.offy;
        v[k].fx = v[k].fy = 0;
        if ((hr.buf || hw_s) && opt.subpixel) {
            const int t = fa ? subpx_tag_get(fa[i], xy, &v[k].fx, &v[k].fy) : -1;
            subpx_n++;
            int e[3], mid = t >= 0 && tag_mid;
            if (t >= 0) { tagged[k] = 1; subpx_hit += t; tiz[k] = tag_iz_out; cp3[k] = tp_out; }
            else if (subpxtag() && mid_cache_get(xy, e)) {   /* an untagged copy of a split point */
                tagged[k] = 1; mid = 1; subpx_hit += e[0]; tiz[k] = tag_iz_out; cp3[k] = tp_out;
                if (e[0]) { v[k].fx = e[1]; v[k].fy = e[2]; }
            }
            else subpx_hit += subpx_get(xy, &v[k].fx, &v[k].fy);
            uvf_want = tex && opt.perspective && (mid || t < 0);   /* split point: exact texture fraction (uvf_tab) */
        }
        if (gpu_cmdlog) {
            rt_logf("   v%d (%d,%d) sub %d,%d%s @%06x", k, v[k].x - g.offx, v[k].y - g.offy, v[k].fx, v[k].fy, tagged[k] ? " tag" : "", fa ? fa[i] : 0xFFFFFF);
            if (cp3[k].ok) rt_logf(" p3 %.0f,%.0f,%.0f", cp3[k].p[0], cp3[k].p[1], cp3[k].p[2]);
            rt_logf("\n");
        }
        v[k].r = col & 255; v[k].g = (col >> 8) & 255; v[k].b = (col >> 16) & 255;
        if (tex) {
            uint32_t uv = f[++i];
            v[k].u = uv & 255; v[k].v = (uv >> 8) & 255;
            if (uvf_want) {   /* (looked up with this corner's own rounded uv: neighbours share split points) */
                int fu, fv;
                if (uvf_get(xy, uv & 0xFFFF, &fu, &fv)) { v[k].uf = fu / 256.0f; v[k].vf = fv / 256.0f; }
            }
            if (k == 0) { clutx = ((uv >> 16) & 63) * 16; cluty = (uv >> 22) & 511; }
            if (k == 1) {
                uint32_t tp = uv >> 16;
                tpx = (tp & 15) * 64; tpy = ((tp >> 4) & 1) * 256;
                depth = (tp >> 7) & 3; if (depth == 3) depth = 2;
                g.tpx = tpx; g.tpy = tpy; g.tdepth = depth;
                g.semi = (tp >> 5) & 3;
            }
        } else {
            v[k].u = v[k].v = 0;
        }
    }
    if (tex && !gour && raw) { for (int k = 0; k < nv; k++) v[k].r = v[k].g = v[k].b = 128; }
    /* depth check (hi-res only): every vertex projected by room geometry -> writes depth (opaque only); every vertex
       by a world object -> tested against it; anything else (HUD, 2D, mixed, unknown) draws as before.
       Only in frames that show the 3D world (gpu_world_frame, room geometry): the title screen draws no fill, so its
       depth was never cleared and the logo / earlier scenes hid the babies peeking in at the bottom corners
       (2026-11-22). RUGRATS_DEPTHWORLD=0 = old (depth check in every frame). */
    static int dworld = -1;
    if (dworld < 0) { const char *e = getenv("RUGRATS_DEPTHWORLD"); dworld = e ? atoi(e) != 0 : 1; }
    int dm = DM_NONE, npdepth = 0;   /* npdepth: depths from the wall plane (corners clamped / without depth) */
    if (opt.depthfix && (hr.buf || hw_s) && (gpu_world_frame || !dworld)) {
        /* all corners room points (opaque) -> write; else all corners object points -> test */
        int k;
        if (!semi_on) {
            float zn[4];
            for (k = 0; k < nv && subpx_zr(f[vi[k]], &v[k].iz, &zn[k]); k++) ;
            if (k == nv) {
                dm = DM_WRITE;
                /* a quad corner whose pixel two room points at different depths share (e.g. a near wall corner on the
                   same screen pixel as far scenery): 1/z is a plane in screen space over a flat polygon, so the other
                   three (unambiguous) corners predict it exactly; take the candidate nearest that prediction. Taking
                   the farther one always tilted near walls away, the room test then hid patches of them and the
                   previous picture showed through (flicker while the camera turns, toy store blocks). Any other
                   polygon with a shared corner keeps the farther depth (an average of the other corners picked wrong
                   on steep near walls) and is drawn without the room test (DM_WNT): it still hides objects behind it,
                   but an unsure depth never hides it. */
                int amb = -1, namb = 0;
                /* a corner with its own exact depth (GTE tag, tiz) takes the candidate that matches it: the db 5
                   corridor wall next to the door frame (z 13897) shared a pixel with engine-room scenery (z ~25800),
                   kept the far one, wrote depth far behind itself and engine-room glass drawn later (glass_q) showed
                   on it as a light-blue slab (2026-11-12). RUGRATS_ZTAGPICK=0 = old. */
                static int ztp = -1;
                if (ztp < 0) { const char *e = getenv("RUGRATS_ZTAGPICK"); ztp = e ? atoi(e) != 0 : 1; }
                /* Neither candidate near the tag (2026-11-20, user video: Reptar 2010 "Pizza Planetoid" sign, single frames
                   with a brick wedge over it): the sign's corner pixel held only a far room point (iz 0.221; the
                   corner's own tag 0.443), so the sign half got a corner twice as far away, the room test hid it
                   behind the coplanar brick facade, and the facade showed through. The tag is the corner's own GTE
                   output: when it is off from both table depths by more than 3%, it wins. RUGRATS_ZTAGPICK=0 = old. */
                for (k = 0; ztp && k < nv; k++)
                    if (tiz[k] > 0) {
                        const float pick = fabsf(zn[k] - tiz[k]) < fabsf(v[k].iz - tiz[k]) ? zn[k] : v[k].iz;
                        if (fabsf(pick - tiz[k]) <= 0.03f * tiz[k]) {
                            if (zn[k] != v[k].iz && gpu_cmdlog) rt_logf("   ztag: corner %d depth %.3f / %.3f -> %.3f (tag %.3f)\n", k, v[k].iz, zn[k], pick, tiz[k]);
                            if (zn[k] != v[k].iz) v[k].iz = zn[k] = pick;
                        } else {
                            if (gpu_cmdlog) rt_logf("   ztag: corner %d depth %.3f / %.3f off the tag -> %.3f\n", k, v[k].iz, zn[k], tiz[k]);
                            v[k].iz = zn[k] = tiz[k];
                        }
                    }
                for (k = 0; k < nv; k++) if (zn[k] != v[k].iz) { amb = k; namb++; }
                if (namb && zpick()) dm = DM_WNT;
                if (nv == 4 && namb == 1 && zpick()) {
                    int q[3], m = 0;
                    for (k = 0; k < 4; k++) if (k != amb) q[m++] = k;
                    double x0 = v[q[0]].x, y0 = v[q[0]].y, z0 = v[q[0]].iz;
                    double x1 = v[q[1]].x - x0, y1 = v[q[1]].y - y0, z1 = v[q[1]].iz - z0;
                    double x2 = v[q[2]].x - x0, y2 = v[q[2]].y - y0, z2 = v[q[2]].iz - z0;
                    double det = x1 * y2 - x2 * y1;
                    if (fabs(det) > 0.5) {
                        double px = v[amb].x - x0, py = v[amb].y - y0;
                        double a = (px * y2 - x2 * py) / det, b = (x1 * py - px * y1) / det; /* p = a*e1 + b*e2 */
                        double zp = z0 + a * z1 + b * z2;
                        if (fabs(zn[amb] - zp) < fabs(v[amb].iz - zp)) v[amb].iz = zn[amb];
                        dm = DM_WRITE;   /* resolved exactly */
                    }
                }
                for (k = 0; k < nv; k++) v[k].izn = dm == DM_WNT ? zn[k] : 0;   /* near candidates (room over object) */
            } else if (nearplane()) {
                /* a wall right at the camera (stick camera close behind Tommy): corners too near / behind the camera
                   are clamped by the GTE (e.g. -1024,-1024) and have no depth, and the game's 2D split pieces of that
                   wall have none either, so it wrote no depth and things behind it that the game sorts later painted
                   over it (closet door by the house boxes). Each corner leads back to real wall vertices (GTE points
                   keep their camera-space position; split midpoints keep their two ends), those give the wall's
                   plane, and 1/z of that plane at each drawn corner is exact over the whole piece. The corners that
                   do have depth must agree. Write-notest: hides objects behind it, never hidden itself. */
                double P[NP_MAX][3];
                double n[3], dd;
                float zb[4];
                int np = 0, ok = 0, flat = 0;
                for (int mode = 0; mode < 4 && !flat; mode++) {   /* 4+ points: three alone always look flat */
                    np = 0; ok = 1; np_ngrp = 0;
                    for (k = 0; k < nv && ok; k++) ok = np_collect(f[vi[k]], P, &np, 0, mode);
                    flat = ok && np >= 4 && np_plane(P, np, n, &dd);
                    if (ok && !flat && np_ngrp && np + np_ngrp >= 4 && np + np_ngrp <= NP_MAX) {
                        /* one candidate per clamped group: the right corner lies on the plane of the others */
                        const int n0 = subpx_tab[np_grp[0]].ccn, n1 = np_ngrp > 1 ? subpx_tab[np_grp[1]].ccn : 1;
                        for (int c0 = 0; c0 < n0 && !flat; c0++) for (int c1 = 0; c1 < n1 && !flat; c1++) {
                            for (int g = 0; g < np_ngrp; g++) {
                                const float *c = subpx_tab[np_grp[g]].cc[g ? c1 : c0];
                                P[np + g][0] = c[0]; P[np + g][1] = c[1]; P[np + g][2] = c[2];
                            }
                            flat = np_plane(P, np + np_ngrp, n, &dd);
                            if (flat) np += np_ngrp;
                        }
                    }
                    if (gpu_cmdlog && !flat && ok) {   /* debug: the points and how far each is off */
                        double n2[3], d2 = 0;
                        rt_logf("   nearplane mode %d: %d points%s\n", mode, np, np >= 3 ? "" : " (too few)");
                        if (np >= 3) { np_plane(P, 3, n2, &d2); }
                        for (int i = 0; i < np; i++)
                            rt_logf("     p %.1f %.1f %.1f off %.2f\n", P[i][0], P[i][1], P[i][2],
                                    np >= 3 ? n2[0] * P[i][0] + n2[1] * P[i][1] + n2[2] * P[i][2] - d2 : 0.0);
                    }
                }
                if (flat) {
                    for (k = 0; k < nv && ok; k++) {
                        const double z = np_iz(n, dd, sext11(f[vi[k]]), sext11(f[vi[k]] >> 16));
                        float zr, zr2;
                        ok = z > 0.01 && z < 60.0;
                        if (ok && subpx_zr(f[vi[k]], &zr, &zr2) && fabs(z - zr) > 0.03 * zr) {
                            /* known depth disagrees: wrong plane, unless the corner is a split midpoint that another
                               vertex also landed on (its depth belongs to that one) */
                            const int h = subpx_find(f[vi[k]]);
                            ok = h >= 0 && subpx_tab[h].mk == 1;
                        }
                        zb[k] = (float)z;
                    }
                    if (ok) {
                        for (k = 0; k < nv; k++) v[k].iz = v[k].izn = zb[k];
                        dm = DM_WNT; npdepth = 1;
                        if (gpu_cmdlog) rt_logf("   nearplane: %d wall points, depths from the plane\n", np);
                        /* positions (hi-res only, RUGRATS_NEARPOS=0 = off): corners that are split midpoints of a
                           clamped corner sit at the 2D midpoint of the clamp, not of the real corner, so the wall's
                           pieces near the lens bend (bricks curving up the right edge of the golf wall in 16:9; the
                           4:3 picture never shows those pieces). Each corner's 3D point projected again gives where
                           it really is; the offset goes into fx/fy, so native VRAM keeps the PS1's picture. */
                        if (nearpos() && (hr.buf || hw_s)) {
                            double p3[4][3], ox, oy, hh, ww;
                            int cl[4] = { 0 }, any = 0, good = 1;
                            gte_proj_params(&ox, &oy, &hh, &ww);
                            for (k = 0; k < nv && good; k++) {
                                good = np_point3(f[vi[k]], n, dd, p3[k], &cl[k], 0) && p3[k][2] > 1.0;
                                any |= cl[k];
                            }
                            if (good && any) {
                                int fx2[4], fy2[4];
                                float iz2[4];
                                for (k = 0; k < nv && good; k++) {
                                    const double sx = ox + hh * ww * p3[k][0] / p3[k][2], sy = oy + hh * p3[k][1] / p3[k][2];
                                    const double dx = sx - sext11(f[vi[k]]), dy = sy - sext11(f[vi[k]] >> 16);
                                    good = fabs(dx) < 2048 && fabs(dy) < 2048;
                                    if (!cl[k]) { fx2[k] = v[k].fx; fy2[k] = v[k].fy; }   /* untouched corner: as before */
                                    else { fx2[k] = (int)lround(dx * 256.0); fy2[k] = (int)lround(dy * 256.0); }
                                    iz2[k] = (float)(4096.0 / p3[k][2]);
                                }
                                if (good) {
                                    for (k = 0; k < nv; k++) {
                                        if (gpu_cmdlog && cl[k])
                                            rt_logf("   nearpos: v%d moved %.2f,%.2f (z %.1f)\n", k, (fx2[k] - v[k].fx) / 256.0, (fy2[k] - v[k].fy) / 256.0, p3[k][2]);
                                        v[k].fx = fx2[k]; v[k].fy = fy2[k];
                                        if (cl[k]) v[k].iz = v[k].izn = iz2[k];
                                    }
                                } else if (gpu_cmdlog) rt_logf("   nearpos: too far off, kept\n");
                            } else if (gpu_cmdlog && any) rt_logf("   nearpos: corner without 3D point / behind the lens, kept\n");
                        }
                    } else if (gpu_cmdlog) rt_logf("   nearplane: plane does not fit (%d points)\n", np);
                } else if (gpu_cmdlog) rt_logf("   nearplane: no plane (%s, %d points)\n", ok ? "too few / not flat" : "corner without wall points", np);
            }
        }
        if (!dm) {
            for (k = 0; k < nv && subpx_z(f[vi[k]], DCLS_OBJ, &v[k].iz); k++) ;
            if (k == nv) dm = DM_TEST;
            /* object sort fix (see objsort_store): all corners from one model (oid), opaque; stores when it faces the
               camera (screen y down: clockwise) */
            if (dm == DM_TEST && opt.objsort && !semi_on) {
                uint32_t cs[4];
                for (k = 0; k < nv; k++) cs[k] = f[vi[k]];
                int id = (int)subpx_common_oid(cs, nv);
                if (id && oidz()) for (k = 0; k < nv; k++) subpx_oid_z(cs[k], (unsigned)id, &v[k].iz);   /* own model's depth */
                /* characters in front of props (hooks.c dcls_run): a face of a prop that stands still (OID_STATIC in
                   its model id) keeps that bit = "hidden where it is clearly behind a character's pixel", unless it
                   is seen from above (1/z grows down the screen: floors, shadows; the game draws those over a
                   character's feet on purpose, e.g. sinking into the soap) */
                if (id & (int)OID_STATIC) {
                    const double x1 = v[1].x - v[0].x, y1 = v[1].y - v[0].y, x2 = v[2].x - v[0].x, y2 = v[2].y - v[0].y;
                    const double det = x1 * y2 - x2 * y1;
                    const double dzy = det ? (x1 * (v[2].iz - v[0].iz) - x2 * (v[1].iz - v[0].iz)) / det : 1;
                    const double dzx = det ? ((v[1].iz - v[0].iz) * y2 - (v[2].iz - v[0].iz) * y1) / det : 0;
                    /* floor-like = 1/z grows down the screen AND mostly down (a level floor seen without camera roll
                       has no sideways depth change at all). The sign alone flipped on upright faces seen from the
                       side (the armchair's front edge next to Tommy: 0.00015 down vs 0.0057 sideways), so Tommy's body
                       blinked in front of / behind it while the stick camera turned. RUGRATS_FLOORK=0: sign only. */
                    static double floork = -1;
                    if (floork < 0) { const char *e = getenv("RUGRATS_FLOORK"); floork = e ? atof(e) : 2.0; }
                    if (dzy > 0 && dzy >= floork * fabs(dzx) && hooks_actor_front() != 2)
                        id = gpu_floor_k() > 0 ? id | (int)OID_FLOOR : id & ~(int)OID_STATIC;   /* RUGRATS_FLOORGAP=0: never hidden (old) */
                }
                for (k = 0; k < nv; k++) v[k].oid = id;
                if (id && (int64_t)(v[1].x - v[0].x) * (v[2].y - v[0].y) - (int64_t)(v[1].y - v[0].y) * (v[2].x - v[0].x) > 0)
                    dm = DM_TW;
            }
            /* Object corner off its own tag (2026-11-20, user: Reptar 2010, a window box hidden under a wall-grey prop
               face until the camera moved): a prop face's corner took 0.413 from the pixel table (another object
               point on that pixel) while its own tag said 0.381, ~900 units too near; with objsort it stored that
               depth and hid the planter drawn after it. After the objsort model lookup (subpx_oid_z), which
               reads the same kind of per-pixel table. Same rule as the room corners (ZTAGPICK): a tag depth more
               than 3% off the table wins. RUGRATS_ZTAGPICK=0 = old. */
            if (dm == DM_TEST || dm == DM_TW) {
                static int ztp = -1;
                if (ztp < 0) { const char *e = getenv("RUGRATS_ZTAGPICK"); ztp = e ? atoi(e) != 0 : 1; }
                for (k = 0; ztp && k < nv; k++)
                    if (tiz[k] > 0 && fabsf(v[k].iz - tiz[k]) > 0.03f * tiz[k]) {
                        if (gpu_cmdlog) rt_logf("   ztag: object corner %d depth %.3f off the tag -> %.3f\n", k, v[k].iz, tiz[k]);
                        v[k].iz = tiz[k];
                    }
            }
            /* shadows under characters (semi-transparent quads lying on the floor, GP0 2E): the game sorts the
               player's shadow among the slippers' faces, so while the stick camera turned it painted over the feet in
               some frames and lay under them in others. A semi-transparent object face seen from above is treated
               like a still prop face: hidden where it lies more than the object gap behind a stored player pixel (the
               slipper's side; floor right under the sole stays). The floor gap on top (=2) still let it darken the
               slippers' lower half. RUGRATS_SHADOWFIX=0: old. */
            if (dm == DM_TEST && opt.objsort && semi_on && nv >= 3 && hooks_actor_front() && gpu_floor_k() > 0) {
                static int on = -1;
                if (on < 0) { const char *e = getenv("RUGRATS_SHADOWFIX"); on = e ? atoi(e) : 1; }
                const double x1 = v[1].x - v[0].x, y1 = v[1].y - v[0].y, x2 = v[2].x - v[0].x, y2 = v[2].y - v[0].y;
                const double det = x1 * y2 - x2 * y1;
                if (on && det != 0 && v[0].iz > 0 && v[1].iz > 0 && v[2].iz > 0) {
                    const double dzy = (x1 * (v[2].iz - v[0].iz) - x2 * (v[1].iz - v[0].iz)) / det;
                    const double dzx = ((v[1].iz - v[0].iz) * y2 - (v[2].iz - v[0].iz) * y1) / det;
                    if (dzy > 0 && dzy >= 2.0 * fabs(dzx)) {
                        for (k = 0; k < nv; k++) v[k].oid = (int)(OID_STATIC | (on == 2 ? OID_FLOOR : 0));
                        if (gpu_cmdlog) rt_logf("   shadowfix: semi face seen from above, hidden behind the player\n");
                    }
                }
            }
        }
        /* HUD protection: the game's sort can put floor pieces right in front of a low camera after the HUD (the
           cookie counter box was cut by the floor with the stick camera tilted up, native PS1 order too). A polygon
           without any room/object corner (pure 2D), opaque, drawn after the frame's 3D scene has started (64 polygons
           with world corners since the drawing area was set) is an overlay: it marks its pixels, and later polygons
           with world corners (also those with no usable depth, HTEST) leave them alone. Backdrops drawn before the
           3D scene are not affected. */
        int nworld = 0;
        for (int k = 0; k < nv; k++) nworld += subpx_isworld(f[vi[k]]);
        if (nworld < 2) nworld = 0;   /* one corner on a world point's pixel can be chance (the cookie box once) */
        if (nworld) hud_3d++;
        if (!dm) dm = nworld ? DM_HTEST : (!semi_on && hud_3d >= 64 && hud_like(f, vi, nv, tex) ? DM_HUD : DM_NONE);
    }
    /* Back faces behind front faces (2026-11-20, user: Reptar 2010, stick camera tilted down, black bars along the
       balcony slabs; then a grey bar at a storefront). While back faces are drawn (gte.c NCLIP: room faces with the
       eye over the wall, prop faces in back-face levels), a back face sits a little behind front faces (balcony slab
       ~140 units behind its front, sampling a black texel at its tile edge; a storefront prop face ~40 units behind
       the wall) and the depth tests let through anything within the gap (+ the front face's slope bias), so it
       painted over them. Every triangle the GTE let through as a back face is noted (gpu_backface_note); a room or
       object polygon whose corners match one gets its depth pushed back by four gaps (iz / (1 + 4 k iz), 384 units;
       two still left a 1 px line): the tests then hide it wherever anything not clearly behind it is drawn, and it
       still covers empty / far areas (hedge backs, roofless buildings, tree backs). Both renderers (per-vertex
       depth). RUGRATS_BACKZ=0 = off, =<n> = push by n gaps. */
    if ((dm == DM_WRITE || dm == DM_WNT || dm == DM_TEST || dm == DM_TW) && present_count - bf_last <= 2u) {
        static int bz = -1;
        static float bzk = 4.0f;
        if (bz < 0) { const char *e = getenv("RUGRATS_BACKZ"); bz = e ? atoi(e) != 0 : 1; if (e && atof(e) > 0) bzk = (float)atof(e); }
        int hit = 0;
        if (bz && (dm == DM_WRITE || dm == DM_WNT) && present_count - gte_backface_frame <= 2u) {
            /* room: wound backwards on screen. The game subdivides big room faces after NCLIP (balcony slab: its
               pieces matched no noted triangle), but room faces are never double-sided, so the winding is exact */
            const int64_t w = (int64_t)(v[1].x - v[0].x) * (v[2].y - v[0].y) - (int64_t)(v[2].x - v[0].x) * (v[1].y - v[0].y);
            hit = w < 0;
        } else if (bz && (dm == DM_TEST || dm == DM_TW)) {
            /* props: exact corner match (props do have double-sided faces, wound either way) */
            static const int tri[4][3] = { { 0, 1, 2 }, { 1, 2, 3 }, { 0, 1, 3 }, { 0, 2, 3 } };
            for (int t = 0; t < (nv == 4 ? 4 : 1) && !hit; t++)
                hit = bf_seen(f[vi[tri[t][0]]], f[vi[tri[t][1]]], f[vi[tri[t][2]]]);
        }
        if (hit) {
            const float k2 = bzk * gpu_depth_k();
            for (int k = 0; k < nv; k++) {
                v[k].iz = v[k].iz / (1.0f + k2 * v[k].iz);
                if (v[k].izn > 0) v[k].izn = v[k].izn / (1.0f + k2 * v[k].izn);
            }
            if (gpu_cmdlog) rt_logf("   backz: back face, depth pushed back\n");
        }
    }
    for (int k = 0; k < nv; k++) v[k].dm = dm;
    glass_cur = 0;
    if (dm == DM_TEST && semi_on && (hw_s || hr.buf) && glass_on()) {   /* see glass_q */
        for (int k = 0; k < nv; k++) v[k].oid |= (int)OID_GLASS;
        glass_cur = 1;
        if (gpu_cmdlog) rt_logf("   glass: hi-res held back (glass_q)\n");
    } else if (glass_n && dm != DM_WRITE && dm != DM_TW && (dm != DM_WNT || glass_wnt_on())) {
        /* the glass only moves past pieces whose depth hides it where they are in front: room pieces that test and
           write depth (depth buffer) and objsort faces (oz). Anything else (pieces without usable depth next to the
           lens, write-notest room pieces, objects without a model id, 2D) keeps the game's order: held glass it
           overlaps first (a crate corner at the lens was painted over; engine-room glass ~19000 away showed as a
           light-blue slab on the db 5 corridor wall next to the door frame, a write-notest piece at ~13000, in some
           frames: 2026-11-12, RUGRATS_GLASSWNT=0 = old) */
        glass_flush_over(v, nv, dm);
    }
    if (opt.subpixel && dm && dm != DM_HUD && dm != DM_HTEST) {   /* corner on an unsure shared pixel: own kind's position */
        const int c = (dm == DM_TEST || dm == DM_TW) ? 1 : 0;
        unsigned oc = 0;
        if (c) { uint32_t cs[4]; for (int k = 0; k < nv; k++) cs[k] = f[vi[k]]; oc = subpx_common_oid(cs, nv); }
        for (int k = 0; k < nv; k++)
            if (!tagged[k] && !v[k].fx && !v[k].fy) {
                if (oc && subpx_getid(f[vi[k]], oc, &v[k].fx, &v[k].fy)) {
                    if (gpu_cmdlog) rt_logf("   v%d sub from model %u %d,%d\n", k, oc, v[k].fx, v[k].fy);
                } else if (subpx_getc(f[vi[k]], c, v[k].iz, &v[k].fx, &v[k].fy)) {
                    if (gpu_cmdlog) rt_logf("   v%d sub from %s points %d,%d\n", k, c ? "object" : "room", v[k].fx, v[k].fy);
                } else if (gpu_cmdlog) {   /* debug: why no position (shared / own kind: amb, none, -) */
                    const int h = subpx_find(f[vi[k]]);
                    const int a = h < 0 ? 0 : subpx_tab[h].fx, b = h < 0 ? 0 : subpx_tab[h].fxc[c];
                    const char *m = !c ? "" : !oc ? " / no model" : h < 0 || subpx_tab[h].oid[0] == 0xFFFF ? " / model ?" : " / model -";
                    if (c && oc && h >= 0)
                        for (int i = 0; i < SUBPX_OIDS; i++)
                            if (subpx_tab[h].oid[i] == oc)
                                m = subpx_tab[h].fxid[i] == FX_AMB ? " / model amb" : " / model none";
                    rt_logf("   v%d no sub: %s / %s%s\n", k, h < 0 ? "-" : a == FX_AMB ? "amb" : a == FX_NONE ? "none" : "0",
                            h < 0 ? "-" : b == FX_AMB ? "amb" : b == FX_NONE ? "none" : "0", m);
                }
            }
    }
    {
        float zl = 0;
        if (dm == DM_WRITE || dm == DM_WNT) {
            for (int k = 0; k < nv; k++) if (v[k].iz > zl) zl = v[k].iz;
            zl = depth_reach(zl);
        }
        for (int k = 0; k < 4; k++) v[k].zl = zl;
    }
    if (gpu_cmdlog && (dm == DM_HUD || dm == DM_HTEST)) rt_logf("   depth %s\n", dm == DM_HUD ? "hud" : "hud-test");
    else if (gpu_cmdlog && dm) {
        if (nv == 4) rt_logf("   depth %s %.3f %.3f %.3f %.3f id %d\n", dm == DM_WRITE ? "write" : dm == DM_WNT ? "write-notest" : dm == DM_TW ? "test+store" : "test", v[0].iz, v[1].iz, v[2].iz, v[3].iz, v[0].oid);
        else rt_logf("   depth %s %.3f %.3f %.3f id %d\n", dm == DM_WRITE ? "write" : dm == DM_WNT ? "write-notest" : dm == DM_TW ? "test+store" : "test", v[0].iz, v[1].iz, v[2].iz, v[0].oid);
    }
    if (gpu_cmdlog && (dm == DM_TEST || dm == DM_TW)) {   /* debug: model ids per corner */
        char b[200]; int n = 0;
        for (int k = 0; k < nv; k++) {
            int h = subpx_find(f[vi[k]]);
            if (h < 0) n += snprintf(b + n, sizeof b - n, " -");
            else n += snprintf(b + n, sizeof b - n, " %u/%u/%u/%u", subpx_tab[h].oid[0], subpx_tab[h].oid[1], subpx_tab[h].oid[2], subpx_tab[h].oid[3]);
        }
        rt_logf("   ids%s\n", b);
    }
    if (gpu_cmdlog && (!dm || dm == DM_HTEST) && opt.depthfix) {   /* debug: why no depth (per corner: room/object depth, '-' not seen) */
        char b[200]; int n = 0;
        for (int k = 0; k < nv; k++) {
            int h = subpx_find(f[vi[k]]);
            if (h < 0) n += snprintf(b + n, sizeof b - n, " -");
            else n += snprintf(b + n, sizeof b - n, " r%.3f/o%.3f", subpx_tab[h].izr, subpx_tab[h].izo);
        }
        rt_logf("   nodepth%s%s\n", b, semi_on ? " semi" : "");
    }
    /* perspective-correct textures (opt.perspective, hi-res only): the PS1 maps textures affinely, so a big polygon
       whose corners lie at very different depths bends its texture (bricks of the golf wall curving up the right edge
       when walking along it; 16:9 shows more of such near pieces). Corners with a known depth (room / object points,
       split midpoints) carry 1/z as the weight; uv * w and w are interpolated and divided per pixel. Only when every
       corner has one and they are sane; native VRAM stays affine (the game's own picture). */
    /* not for a piece with a corner at the GTE screen limit or split from one (clampaff): such a corner is not where
       its vertex projects (or the vertex is behind the camera), but it carries that vertex's texture coordinate. Affine,
       the PS1 spreads that evenly; with the corner's true depth as the weight the texture squeezed toward it, and the
       near golf wall's cap ran up the screen instead of down to the corner (2026-11-11). */
    int clampd = 0;
    if (opt.perspective && tex && npdepth && clampaff())
        for (int k = 0; k < nv && !clampd; k++) clampd = np_fromclamp(f[vi[k]], 0);
    int clampx = 0;   /* exact texture mapping for that piece from its corners' 3D points (clamp_uv) */
    double cu[4], cv[4];   /* the corners' own texture coordinates (game's + split fraction), for clamp_draw */
    for (int k = 0; k < nv; k++) { cu[k] = v[k].u + v[k].uf; cv[k] = v[k].v + v[k].vf; }
    if (clampd && clampuv()) clampx = clamp_uv(v, nv, cp3);
    if (gpu_cmdlog && clampd) rt_logf("   persp %s: corner from a clamped point\n", clampx ? "from 3D points" : "off");
    if (clampx) clampd = 0;
    if (opt.perspective && tex && (dm == DM_WRITE || dm == DM_WNT || dm == DM_TEST || dm == DM_TW) && !clampd && !clampx) {
        float lo = v[0].iz, hi = v[0].iz;
        for (int k = 1; k < nv; k++) { if (v[k].iz < lo) lo = v[k].iz; if (v[k].iz > hi) hi = v[k].iz; }
        if (lo > 0 && hi < 64.0f * lo && hi == hi)
            for (int k = 0; k < nv; k++) v[k].pw = tiz[k] > 0 && pwtag() ? tiz[k] : v[k].iz;   /* tag: exact */
        if (gpu_cmdlog) {   /* debug: perspective weights (0 = affine) and where each came from */
            char b[200]; int n = 0;
            for (int k = 0; k < nv; k++) n += snprintf(b + n, sizeof b - n, " %.3f%s", v[k].pw, tiz[k] > 0 ? "t" : "");
            rt_logf("   persp%s uv", b);
            for (int k = 0; k < nv; k++) rt_logf(" %.2f,%.2f", v[k].u + v[k].uf, v[k].v + v[k].vf);
            rt_logf("\n");
        }
    }
    double gdk[4] = { 0, 0, 0, 0 };
    const int grow = !semi_on && dm != DM_TEST && dm != DM_TW && dm != DM_HUD && poly_grow(v, nv, f, vi, gdk);   /* not objects / 2D */
    /* repeated triangles (opt.dupfix): see tri_hr_skip */
    const int dupon = opt.dupfix && (hr.buf || hw_s) && !semi_on;
    uint32_t tstate = (uint32_t)(op & ~8) | (uint32_t)(g.twmx | g.twmy << 5 | g.twox << 10 | g.twoy << 15) << 8;   /* a tri may repeat half of a quad */
    uint64_t ts = tex ? ((uint64_t)(f[vi[0] + 1] >> 16) | (uint64_t)(f[vi[1] + 1] >> 16) << 16) << 32 | tstate : tstate;
    tri_hr_skip = dupon && dup_seen(dup_key(f, vi, vc, 0, 1, 2, tex, ts));
    if (tri_hr_skip && gpu_cmdlog) rt_logf("   repeat: hi-res skips v0 v1 v2\n");
    /* any polygon (wall, floor, object, textured or not) with a corner pinned at the screen limit or split from one,
       whose corners all carry their true 3D point */
    int reproj = 0;
    /* Unclassified face with a corner behind the camera (2026-11-23, user: Toy Palace intro, first shot: a big blue
       slab over Tommy). DM_HTEST = world corners, but not drawn through the room / object draw hooks. The cutscene's
       shopping cart has such a face (untextured) with a corner just behind the lens (z -54): drawn from its true points
       it covered the right half of the screen, but its game triangle is 79x803, which the PS1 drops (Redux: Tommy in the
       cart, no slab). While a streamed cutscene runs (shots framed on the PS1), such a triangle that the PS1 would drop
       for size stays dropped (behind_un, below). Not in gameplay: the stick camera pushed into a hedge in Let There Be
       Light has the same kind of face (textured HTEST, also too big for the PS1), and there the true version fills
       what would be a black wedge (tries: all unclassified faces -> wedges at hub + LTBL hedges; size rule only ->
       LTBL wedge). RUGRATS_BEHINDOBJ=1 = old (always drawn from the true points). */
    int behind_un = 0;
    static int behind_obj = -1;
    if (behind_obj < 0) { const char *e = getenv("RUGRATS_BEHINDOBJ"); behind_obj = e ? atoi(e) != 0 : 0; }
    const int classed = v[0].dm == DM_WRITE || v[0].dm == DM_WNT || v[0].dm == DM_TEST || v[0].dm == DM_TW;
    if (clamppos() && (hr.buf || hw_s) && opt.subpixel) {
        reproj = 1;
        for (int k = 0; k < nv && reproj; k++) reproj = cp3[k].ok;
        if (reproj) {
            int any = 0;
            for (int k = 0; k < nv && !any; k++) any = np_fromclamp(f[vi[k]], 0);
            /* a corner behind the camera (hooks.c near_room lets such room faces through): its projection is mirrored,
               not always pinned, so the game's polygon is garbage; drawn from the true points (clipped at z >= 1) */
            for (int k = 0; k < nv && !any; k++) {
                int hooks_cutscene_running(void);
                any = cp3[k].p[2] < 1.0f;
                behind_un = any && !classed && hooks_cutscene_running();
            }
            if (!any && bigtri()) {
                /* too big for the PS1 GPU (> 1023 x 511) with every corner really projected (in front of the lens, not
                   pinned): a door frame right at the stick camera spans ~620 lines; the PS1 drops it and the next room
                   showed through. Drawn from its true points like the pinned case (RUGRATS_BIGTRI=0: dropped). */
                int x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y, front = 1;
                for (int k = 0; k < nv; k++) {
                    if (v[k].x < x0) x0 = v[k].x;
                    if (v[k].x > x1) x1 = v[k].x;
                    if (v[k].y < y0) y0 = v[k].y;
                    if (v[k].y > y1) y1 = v[k].y;
                    front &= cp3[k].p[2] >= 1.0f;
                }
                any = front && (x1 - x0 > 1023 || y1 - y0 > 511);
                /* nor right at the lens (corner nearer than RUGRATS_BIGTRIZ, default 1000 camera units): the trunk's
                   pieces beside the view centre sat at z 550-780, the door frame's nearest corners at 1335+ */
                {
                    static float zmin = -1;
                    if (zmin < 0) { const char *e = getenv("RUGRATS_BIGTRIZ"); zmin = e ? (float)atof(e) : 1000.0f; }
                    for (int k = 0; k < nv && any; k++) if (cp3[k].p[2] < zmin) any = 0;
                }
                /* but not over the middle of the view, where the player stands (camera looks at it): a tree trunk
                   right at the lens (mini golf) would cover Angelica as a big slab; the PS1's drop stands there */
                for (int t = 0; any && t < (quad ? 2 : 1); t++)
                    for (int c = 0; c < 2 && any; c++) {
                        const double px = g.offx, py = g.offy + 60 * c;
                        const Vtx *A = &v[t], *B = &v[t + 1], *C = &v[t + 2];
                        const double e0 = (double)(B->x - A->x) * (py - A->y) - (double)(B->y - A->y) * (px - A->x);
                        const double e1 = (double)(C->x - B->x) * (py - B->y) - (double)(C->y - B->y) * (px - B->x);
                        const double e2 = (double)(A->x - C->x) * (py - C->y) - (double)(A->y - C->y) * (px - C->x);
                        if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) any = 0;
                    }
                if (gpu_cmdlog && front && (x1 - x0 > 1023 || y1 - y0 > 511)) {   /* any = 0: centre or near rule */
                    float zn = cp3[0].p[2];
                    for (int k = 1; k < nv; k++) if (cp3[k].p[2] < zn) zn = cp3[k].p[2];
                    rt_logf("   bigtri: %dx%d, all corners in front (nearest z %.0f): %s\n", x1 - x0, y1 - y0, zn,
                            any ? "true 3D" : "at the lens or over the view centre, dropped like the PS1");
                }
            }
            reproj = any;
        }
    }
    if (reproj) {   /* true geometry in hi-res, the game's polygon in native VRAM */
        const int skip0 = tri_hr_skip, skip1 = quad && dupon && dup_seen(dup_key(f, vi, vc, 1, 2, 3, tex, ts));
        tri_hr_skip = 1;
        tri(v[0], v[1], v[2], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        if (quad) tri(v[1], v[2], v[3], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        tri_hr_skip = 0;
        for (int t = 0; t < (quad ? 2 : 1); t++) {
            if (t ? skip1 : skip0) continue;
            /* the PS1 GPU drops a triangle wider than 1023 / taller than 511 (pinned corners make many of them, e.g. a
               tree trunk right at the lens): the true triangle is drawn only where the game's one is */
            {
                int x0 = v[t].x, x1 = v[t].x, y0 = v[t].y, y1 = v[t].y;
                for (int k = 1; k < 3; k++) {
                    if (v[t + k].x < x0) x0 = v[t + k].x;
                    if (v[t + k].x > x1) x1 = v[t + k].x;
                    if (v[t + k].y < y0) y0 = v[t + k].y;
                    if (v[t + k].y > y1) y1 = v[t + k].y;
                }
                /* room geometry is the exception: the game splits a near wall into pieces it culls by their pinned
                   positions, so pieces whose true area is on screen can be missing; the whole wall's true shape covers
                   exactly what its pieces would (same plane and texture) */
                const int room = v[t].dm == DM_WRITE || v[t].dm == DM_WNT;
                int pinned = 0;   /* positions from pinned corners: the PS1's drop stands (tree trunk at the lens) */
                for (int k = 0; k < 3 && !pinned; k++) pinned = np_fromclamp(f[vi[t + k]], 0);
                if ((x1 - x0 > 1023 || y1 - y0 > 511) &&
                    ((pinned && !(room && x1 > x0 && y1 > y0)) || (behind_un && !behind_obj))) {
                    if (gpu_cmdlog) rt_logf("   clamp_draw: tri %d too big for the PS1 (%dx%d), skipped\n", t, x1 - x0, y1 - y0);
                    continue;
                }
            }
            const Vtx *s3[3]; double P[3][3], U[3], V[3];
            for (int k = 0; k < 3; k++) {
                const int j = t + k;
                s3[k] = &v[j];
                for (int c = 0; c < 3; c++) P[k][c] = cp3[j].p[c];
                U[k] = cu[j]; V[k] = cv[j];
            }
            clamp_draw(s3, P, U, V, tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        }
        return;
    }
    if (grow) {   /* crack fill: the game's polygon in native VRAM, the grown outline in hi-res */
        const int skip0 = tri_hr_skip, skip1 = quad && dupon && dup_seen(dup_key(f, vi, vc, 1, 2, 3, tex, ts));
        tri_hr_skip = 1;
        tri(v[0], v[1], v[2], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        if (quad) tri(v[1], v[2], v[3], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
        tri_hr_skip = 0;
        static const int ring4[4] = { 0, 1, 3, 2 };
        double rx[4], ry[4], de[4], qx[8], qy[8];
        for (int i = 0; i < nv; i++) {
            const int k = quad ? ring4[i] : i;
            rx[i] = v[k].x + v[k].fx / 256.0; ry[i] = v[k].y + v[k].fy / 256.0;
        }
        for (int i = 0; i < nv; i++) {
            const int k0 = quad ? ring4[i] : i, k1 = quad ? ring4[(i + 1) % nv] : (i + 1) % nv;
            de[i] = 0.5 * (gdk[k0] + gdk[k1]);
        }
        const int nq = grow_ring(rx, ry, nv, de, qx, qy);
        for (int t = 0; t < (quad ? 2 : 1); t++) {
            if (t ? skip1 : skip0) continue;
            const Vtx *t3[3] = { &v[t], &v[t + 1], &v[t + 2] };
            double hx[10], hy[10];
            int n = nq;
            if (quad && nq >= 3) {   /* this triangle's half: the side of the diagonal v1-v2 its third corner is on */
                const double ax = v[1].x + v[1].fx / 256.0, ay = v[1].y + v[1].fy / 256.0;
                const double bx = v[2].x + v[2].fx / 256.0, by = v[2].y + v[2].fy / 256.0;
                const Vtx *o3 = t ? &v[3] : &v[0];
                const double fo = (bx - ax) * (o3->y + o3->fy / 256.0 - ay) - (by - ay) * (o3->x + o3->fx / 256.0 - ax);
                n = clip_side(qx, qy, nq, ax, ay, bx, by, fo > 0 ? 1 : -1, hx, hy);
            } else for (int m = 0; m < nq; m++) { hx[m] = qx[m]; hy[m] = qy[m]; }
            if (n < 3) { tri_nat_skip = 1; tri(v[t], v[t + 1], v[t + 2], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth); tri_nat_skip = 0; continue; }
            Vtx o[10];
            for (int m = 0; m < n; m++) tri_attr(t3, hx[m], hy[m], &o[m]);
            if (gpu_cmdlog) {
                rt_logf("   grow: tri %d %d corners", t, n);
                for (int m = 0; m < n; m++) rt_logf(" (%.3f,%.3f)", hx[m] - g.offx, hy[m] - g.offy);
                rt_logf("\n");
            }
            tri_nat_skip = 1;
            for (int m = 1; m + 1 < n; m++) tri(o[0], o[m], o[m + 1], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
            tri_nat_skip = 0;
        }
        return;
    }
    tri(v[0], v[1], v[2], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
    if (quad) {
        tri_hr_skip = dupon && dup_seen(dup_key(f, vi, vc, 1, 2, 3, tex, ts));
        if (tri_hr_skip && gpu_cmdlog) rt_logf("   repeat: hi-res skips v1 v2 v3\n");
        tri(v[1], v[2], v[3], tex, gour, semi_on, raw, clutx, cluty, tpx, tpy, depth);
    }
    tri_hr_skip = 0;
}

static void exec_rect(uint32_t *f) {
    int op = f[0] >> 24;
    int tex = op & 4, semi_on = op & 2, raw = op & 1, sz = (op >> 3) & 3;
    int i = 1;
    uint32_t xy = f[i++];
    int x = sext11(xy) + g.offx, y = sext11(xy >> 16) + g.offy;
    int u = 0, v = 0, clutx = 0, cluty = 0;
    if (tex) {
        uint32_t uv = f[i++];
        u = uv & 255; v = (uv >> 8) & 255;
        clutx = ((uv >> 16) & 63) * 16; cluty = (uv >> 22) & 511;
    }
    int w, h;
    if (sz == 0) { w = f[i] & 0x3FF; h = (f[i] >> 16) & 0x1FF; }
    else if (sz == 1) w = h = 1;
    else if (sz == 2) w = h = 8;
    else w = h = 16;
    int r = f[0] & 255, gg = (f[0] >> 8) & 255, b = (f[0] >> 16) & 255;
    if (tex && raw) r = gg = b = 128;
    rect(x, y, w, h, r, gg, b, tex, semi_on, raw, u, v, clutx, cluty);
}

static void exec_fill(uint32_t *f) {
    uint16_t c = rgb15(f[0] & 255, (f[0] >> 8) & 255, (f[0] >> 16) & 255, 0, 0, 0);
    int x = f[1] & 0x3F0, y = (f[1] >> 16) & 0x1FF;
    int w = ((f[2] & 0x3FF) + 15) & ~15, h = (f[2] >> 16) & 0x1FF;
    if (w >= 256 && h >= 200) { /* the frame's background clear: gap colour while looking over the walls (hooks.c) */
        uint32_t hooks_gap_fill(uint32_t col);
        const uint32_t nc = hooks_gap_fill(f[0] & 0xFFFFFF);
        if (nc != (f[0] & 0xFFFFFF)) c = rgb15(nc & 255, (nc >> 8) & 255, (nc >> 16) & 255, 0, 0, 0);
    }
    if (hw_s) hw_guard(x, y, x + w - 1, y + h - 1);
    nl_set(x, y, x + w - 1, y + h - 1, 0);
    if (hr.buf) { /* queued shadow work must not see the native fill early */
        if (x + w > 1024 || y + h > 512) gpu_hr_sync();
        else guard_rect(x, y, x + w - 1, y + h - 1);
    }
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            vram[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] = c;
    cpu.cycles += (uint32_t)(w * h) / 16;
    { void tex_fill(int, int, int, int); tex_fill(x, y, w, h); }   /* texpack.c */
    if (hw_s && w > 0 && h > 0) hw_area(x, y, w, h, HWF_FILL, c);
    if (hr.buf && w > 0 && h > 0) {
        HCmd q = { HC_FILL };
        q.v[0].x = x; q.v[0].y = y; q.w = w; q.h = h; q.r = c;
        hq_push(&q);
    }
}

static void exec_copy(uint32_t *f) {
    gpu_hr_sync();
    int sx = f[1] & 1023, sy = (f[1] >> 16) & 511, dx = f[2] & 1023, dy = (f[2] >> 16) & 511;
    int w = ((f[3] - 1) & 1023) + 1, h = (((f[3] >> 16) - 1) & 511) + 1;
    if (dx + w > 1024 || dy + h > 512) late_dirty = 1; else late_touch(dx, dy, dx + w - 1, dy + h - 1);
    nl_read(sx, sy, sx + w - 1, sy + h - 1, 2); /* GPU-only source pixels: copy them back first */
    if (hw_s) hw_copy(sx, sy, dx, dy, w, h); /* flushes queued draws first, then copies on the GPU */
    { void tex_copy(int, int, int, int, int, int); tex_copy(sx, sy, dx, dy, w, h); }   /* texpack.c */
    nl_set(dx, dy, dx + w - 1, dy + h - 1, 0); /* native copy below makes the destination exact */
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            uint16_t s = vram[((sy + yy) & 511) * 1024 + ((sx + xx) & 1023)];
            uint16_t *d = &vram[((dy + yy) & 511) * 1024 + ((dx + xx) & 1023)];
            if (g.mask_check && (*d & 0x8000)) continue;
            *d = s | (g.mask_set ? 0x8000 : 0);
        }
    if (hr.buf) { /* copy the detailed pixels too */
        int k = hr.s;
        const uint16_t set = g.mask_set ? 0x8000 : 0;
        const int W = w * k, wrap = sx * k + W > hr.w || dx * k + W > hr.w;
        for (int yy = 0; yy < h * k; yy++) {
            const uint16_t *srow = &hr.buf[(size_t)((sy * k + yy) % hr.h) * hr.w];
            uint16_t *drow = &hr.buf[(size_t)((dy * k + yy) % hr.h) * hr.w];
            if (!wrap) {
                const uint16_t *sp = srow + sx * k;
                uint16_t *dp = drow + dx * k;
                if (!g.mask_check) for (int i = 0; i < W; i++) dp[i] = sp[i] | set;
                else for (int i = 0; i < W; i++) { if (!(dp[i] & 0x8000)) dp[i] = sp[i] | set; }
                continue;
            }
            for (int xx = 0; xx < W; xx++) {
                uint16_t s = srow[(sx * k + xx) % hr.w];
                uint16_t *d = &drow[(dx * k + xx) % hr.w];
                if (g.mask_check && (*d & 0x8000)) continue;
                *d = s | set;
            }
        }
    }
}

int gpu_cmdlog; /* RUGRATS_GP0LOG=<vblank>: log every drawing command during that vblank (debug) */
static void exec(uint32_t *f, int n) {
    int op = f[0] >> 24;
    if (gpu_cmdlog) {
        char b[200];
        int k = 0;
        for (int i = 0; i < n && i < 12; i++) k += snprintf(b + k, sizeof b - k, " %08x", f[i]);
        rt_logf("gp0:%s | area %d,%d-%d,%d off %d,%d tp %d,%d semi %d\n", b, g.dx1, g.dy1, g.dx2, g.dy2, g.offx, g.offy,
                g.tpx, g.tpy, g.semi);
    }
    if (glass_n && op >= 0x40 && op < 0x80) gpu_glass_flush();   /* glass before a squeezed sprite/line (Stretch 2D) */
    sq_set();
    if (op >= 0x20 && op < 0x40) { exec_poly(f); glass_cur = 0; sq_on = 0; return; }
    if (glass_n && (op == 0x02 || op >= 0x40 && op < 0xE0)) gpu_glass_flush();   /* line / sprite / fill / VRAM transfer: glass first */
    if (op >= 0x40 && op < 0x60) {
        int gour = op & 0x10, semi_on = op & 2;
        Vtx v[2];
        int i = 0, k = 0;
        uint32_t col = f[0];
        Vtx prev = { 0 };
        int have = 0;
        while (i < n) {
            if (k > 0 && gour) col = f[i++];
            if (i >= n) break;
            uint32_t xy = f[k == 0 ? (i = 1, i++) : i++];
            if (k > 0 && (xy & 0xF000F000) == 0x50005000) break;
            v[0].x = sext11(xy) + g.offx; v[0].y = sext11(xy >> 16) + g.offy;
            v[0].r = col & 255; v[0].g = (col >> 8) & 255; v[0].b = (col >> 16) & 255;
            if (have) line(prev, v[0], gour, semi_on);
            prev = v[0];
            have = 1;
            k++;
            if (!(op & 8) && k == 2) break;
        }
        sq_on = 0;
        return;
    }
    if (op >= 0x60 && op < 0x80) { exec_rect(f); sq_on = 0; return; }
    sq_on = 0;
    switch (op) {
    case 0x02: exec_fill(f); break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        exec_copy(f); break;
    default:
        if (op >= 0xA0 && op < 0xC0) {
            g.xx = f[1] & 1023; g.xy = (f[1] >> 16) & 511;
            g.xw = ((f[2] - 1) & 1023) + 1; g.xh = (((f[2] >> 16) - 1) & 511) + 1;
            g.xcx = g.xcy = 0;
            g.xfer = 1;
            {   /* RUGRATS_FLIPLOG also logs tall uploads (movie strips) with the vblank, to compare with the flips */
                static int fl = -1;
                if (fl < 0) fl = getenv("RUGRATS_FLIPLOG") != NULL;
                unsigned hooks_frame(void);
                if (fl && g.xh >= 200) rt_logf("upload vbl %u x %d w %d disp %d cyc %u\n", hooks_frame(), g.xx, g.xw, g.disp_x, (unsigned)cpu.cycles);
            }
            gpu_hr_sync(); /* CPU->VRAM upload follows */
            if (hw_s) hw_guard(g.xx, g.xy, g.xx + g.xw - 1, g.xy + g.xh - 1);
            nl_set(g.xx, g.xy, g.xx + g.xw - 1, g.xy + g.xh - 1, 0);
            if (g.xx + g.xw > 1024 || g.xy + g.xh > 512) late_dirty = 1;
            else late_touch(g.xx, g.xy, g.xx + g.xw - 1, g.xy + g.xh - 1);
        } else if (op >= 0xC0 && op < 0xE0) {
            g.xx = f[1] & 1023; g.xy = (f[1] >> 16) & 511;
            g.xw = ((f[2] - 1) & 1023) + 1; g.xh = (((f[2] >> 16) - 1) & 511) + 1;
            g.xcx = g.xcy = 0;
            g.xfer = 2;
            nl_read(g.xx, g.xy, g.xx + g.xw - 1, g.xy + g.xh - 1, 3);
        }
        break;
    }
}

static void xfer_pixel_in(uint16_t p) {
    int x = (g.xx + g.xcx) & 1023, y = (g.xy + g.xcy) & 511;
    uint16_t *d = &vram[y * 1024 + x];
    if (!(g.mask_check && (*d & 0x8000))) *d = p | (g.mask_set ? 0x8000 : 0);
    if (hr.buf) {
        int s = hr.s;
        for (int j = 0; j < s; j++)
            for (int i = 0; i < s; i++) {
                uint16_t *q = &hr.buf[(size_t)(y * s + j) * hr.w + x * s + i];
                if (!(g.mask_check && (*q & 0x8000))) *q = p | (g.mask_set ? 0x8000 : 0);
            }
    }
    if (++g.xcx >= g.xw) {
        g.xcx = 0;
        if (++g.xcy >= g.xh) {
            g.xfer = 0;
            { void night_upload(int, int, int, int); night_upload(g.xx, g.xy, g.xw, g.xh); }   /* hooks.c: "Let There Be Light" Fixes (furniture) */
            { void tex_upload(int, int, int, int); tex_upload(g.xx, g.xy, g.xw, g.xh); }   /* texpack.c */
            if (hw_s) { /* the uploaded native pixels go to the hi-res target as s x s blocks */
                hw_mark(g.xy, g.xy + g.xh - 1);
                hw_area(g.xx, g.xy, g.xw, g.xh, HWF_BLIT, 0);
            }
        }
    }
}

/* "Let There Be Light" Fixes, furniture part (hooks.c): read / rewrite one native VRAM pixel. gpu_px_put is only for the moment an upload has just
   been written (night_upload, before the upload's own hi-res / GPU marking); it also updates the CPU shadow. */
uint16_t gpu_px_get(int x, int y) { return vram[(y & 511) * 1024 + (x & 1023)]; }
void gpu_px_put(int x, int y, uint16_t p) {
    x &= 1023; y &= 511;
    vram[y * 1024 + x] = p;
    if (hr.buf) {
        int s = hr.s;
        for (int j = 0; j < s; j++)
            for (int i = 0; i < s; i++) hr.buf[(size_t)(y * s + j) * hr.w + x * s + i] = p;
    }
}
/* Write a w x h block of native pixels outside any upload, with the same syncing and marking as a CPU->VRAM upload
   (used when the "Let There Be Light" Fixes option is switched in the F1 menu). */
void gpu_vram_patch(int x, int y, int w, int h, const uint16_t *px) {
    gpu_hr_sync();
    if (hw_s) hw_guard(x, y, x + w - 1, y + h - 1);
    nl_set(x, y, x + w - 1, y + h - 1, 0);
    late_touch(x, y, x + w - 1, y + h - 1);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) gpu_px_put(x + i, y + j, px[j * w + i]);
    { void tex_upload(int, int, int, int); tex_upload(x, y, w, h); }
    if (hw_s) { hw_mark(y, y + h - 1); hw_area(x, y, w, h, HWF_BLIT, 0); }
}

static uint16_t xfer_pixel_out(void) {
    uint16_t p = vram[((g.xy + g.xcy) & 511) * 1024 + ((g.xx + g.xcx) & 1023)];
    if (++g.xcx >= g.xw) { g.xcx = 0; if (++g.xcy >= g.xh) g.xfer = 0; }
    return p;
}

void gpu_gp0(uint32_t v) {
    if (g.xfer == 1) {
        xfer_pixel_in((uint16_t)v);
        if (g.xfer == 1) xfer_pixel_in((uint16_t)(v >> 16));
        return;
    }
    if (g.fifo_n == 0) {
        int op = v >> 24;
        if (op >= 0xE1 && op <= 0xE6) {
            switch (op) {
            case 0xE1: set_texpage(v); break;
            case 0xE2: g.twmx = v & 31; g.twmy = (v >> 5) & 31; g.twox = (v >> 10) & 31; g.twoy = (v >> 15) & 31; break;
            case 0xE3: if (glass_n) gpu_glass_flush(); g.dx1 = v & 1023; g.dy1 = (v >> 10) & 511; dup_gen++; hud_3d = 0; break;   /* new frame for the repeat check */
            case 0xE4: g.dx2 = v & 1023; g.dy2 = (v >> 10) & 511; break;
            case 0xE5: g.offx = sext11(v); g.offy = sext11(v >> 11); break;
            case 0xE6: g.mask_set = v & 1; g.mask_check = (v >> 1) & 1; break;
            }
            return;
        }
        if (op == 0x00 || op == 0x01 || (op >= 0x03 && op < 0x20) || op >= 0xE0) {
            if (op == 0x1F) g.irq = 1;
            return;
        }
        g.fifo_need = cmd_len(v);
        g.polyline = (op >= 0x48 && op < 0x60 && (op & 8));
    }
    if (g.fifo_n < 16) { g.fifo[g.fifo_n] = v; gfa[g.fifo_n] = gpu_src_addr; }
    g.fifo_n++;
    if (g.polyline) {
        int gour = (g.fifo[0] >> 24) & 0x10;
        /* terminator is checked on vertex words only; simple test works for both variants */
        if (g.fifo_n > 3 && (v & 0xF000F000) == 0x50005000 && (!gour || (g.fifo_n % 2) == 1)) {
            exec(g.fifo, g.fifo_n < 16 ? g.fifo_n : 16);
            g.fifo_n = 0;
        } else if (g.fifo_n >= 16) {
            /* long polyline: draw what we have, keep the last vertex */
            exec(g.fifo, 16);
            g.fifo[1] = g.fifo[15]; gfa[1] = gfa[15];
            g.fifo_n = 2;
        }
        return;
    }
    if (g.fifo_n >= g.fifo_need) {
        exec(g.fifo, g.fifo_n);
        g.fifo_n = 0;
    }
}

void gpu_gp1(uint32_t v) {
    int op = (v >> 24) & 0x3F;
    switch (op) {
    case 0x00: { uint16_t keep[1]; (void)keep; gpu_init(); break; }
    case 0x01: g.fifo_n = 0; g.xfer = 0; break;
    case 0x02: g.irq = 0; break;
    case 0x03: g.disp_off = v & 1; break;
    case 0x04: g.dma_dir = v & 3; break;
    case 0x05: {
        int nx = v & 0x3FE, ny = (v >> 10) & 0x1FF;
        if (nx != g.disp_x || ny != g.disp_y) {
            if (glass_n) gpu_glass_flush();
            gpu_flips++;
            if (subpx_byflip()) { subpx_gen++; subpx_noflip = 0; }
            static int fl = -1;
            if (fl < 0) fl = getenv("RUGRATS_FLIPLOG") != NULL;
            unsigned hooks_frame(void);
            if (fl) rt_logf("flip vbl %u to %d,%d cyc %u ra %08x\n", hooks_frame(), nx, ny, (unsigned)cpu.cycles, cpu.r[31]);
        }
        g.disp_x = nx; g.disp_y = ny;
        break;
    }
    case 0x06: g.h1 = v & 0xFFF; g.h2 = (v >> 12) & 0xFFF; break;
    case 0x07: g.v1 = v & 0x3FF; g.v2 = (v >> 10) & 0x3FF; break;
    case 0x08: g.mode = v & 0xFF; break;
    case 0x10: {
        switch (v & 7) {
        case 2: g.gpuread = g.twmx | (g.twmy << 5) | (g.twox << 10) | (g.twoy << 15); break;
        case 3: g.gpuread = g.dx1 | (g.dy1 << 10); break;
        case 4: g.gpuread = g.dx2 | (g.dy2 << 10); break;
        case 5: g.gpuread = (g.offx & 0x7FF) | ((g.offy & 0x7FF) << 11); break;
        case 7: g.gpuread = 2; break;
        }
        break;
    }
    }
}

/* debug export (RUGRATS_EXPORT, savestate.c): the GP0 E1-E6 and GP1 03/05-08 words that recreate the current
   drawing and display settings on another PS1 GPU (e.g. an emulator used as a reference). Returns the count. */
int gpu_export_cmds(uint32_t *out) {
    int n = 0;
    uint32_t tp = (uint32_t)(g.tpx / 64) | (uint32_t)(g.tpy / 256) << 4 | (uint32_t)g.semi << 5 | (uint32_t)g.tdepth << 7 |
                  (uint32_t)g.dither << 9 | (uint32_t)g.draw_disp << 10 | (uint32_t)g.rect_flipx << 12 | (uint32_t)g.rect_flipy << 13;
    out[n++] = 0xE1000000u | tp;
    out[n++] = 0xE2000000u | (uint32_t)(g.twmx | g.twmy << 5 | g.twox << 10 | g.twoy << 15);
    out[n++] = 0xE3000000u | (uint32_t)(g.dx1 | g.dy1 << 10);
    out[n++] = 0xE4000000u | (uint32_t)(g.dx2 | g.dy2 << 10);
    out[n++] = 0xE5000000u | (uint32_t)((g.offx & 0x7FF) | (g.offy & 0x7FF) << 11);
    out[n++] = 0xE6000000u | (uint32_t)(g.mask_set | g.mask_check << 1);
    out[n++] = 0x03000000u | (uint32_t)g.disp_off;
    out[n++] = 0x05000000u | (uint32_t)(g.disp_x | g.disp_y << 10);
    out[n++] = 0x06000000u | (uint32_t)(g.h1 | g.h2 << 12);
    out[n++] = 0x07000000u | (uint32_t)(g.v1 | g.v2 << 10);
    out[n++] = 0x08000000u | (uint32_t)g.mode;
    return n;
}

uint32_t gpu_read(void) {
    if (g.xfer == 2) {
        uint32_t lo = xfer_pixel_out();
        uint32_t hi = g.xfer == 2 ? xfer_pixel_out() : 0;
        return lo | (hi << 16);
    }
    return g.gpuread;
}

uint32_t gpu_stat(void) {
    uint32_t s = 0;
    s |= (g.tpx / 64) | ((g.tpy / 256) << 4) | (g.semi << 5) | (g.tdepth << 7) | (g.dither << 9) | (g.draw_disp << 10);
    s |= g.mask_set << 11 | g.mask_check << 12;
    s |= 1u << 13; /* interlace field */
    int hr2 = (g.mode >> 6) & 1, hr1 = g.mode & 3;
    s |= (uint32_t)hr2 << 16 | (uint32_t)hr1 << 17;
    s |= (uint32_t)((g.mode >> 2) & 1) << 19;
    s |= (uint32_t)((g.mode >> 3) & 1) << 20;
    s |= (uint32_t)((g.mode >> 4) & 1) << 21;
    s |= (uint32_t)((g.mode >> 5) & 1) << 22;
    s |= (uint32_t)g.disp_off << 23;
    s |= g.irq << 24;
    s |= 1u << 26 | 1u << 28;          /* ready for command, ready for DMA block */
    if (g.xfer == 2) s |= 1u << 27;    /* ready to send VRAM to CPU */
    s |= (uint32_t)g.dma_dir << 29;
    if (g.dma_dir == 1 || g.dma_dir == 2) s |= 1u << 25;
    else if (g.dma_dir == 3 && g.xfer == 2) s |= 1u << 25;
    /* bit31: odd/even line in interlace; toggles per frame */
    if (g.odd) s |= 0x80000000u;
    return s;
}

void gpu_vblank(void) {
    g.odd ^= 1;
    {
        static int at = -1, to;   /* RUGRATS_GP0LOG=<vblank> or <first>-<last> */
        static uint32_t vbl;
        if (at < 0) {
            const char *e = getenv("RUGRATS_GP0LOG"), *d = e ? strchr(e, '-') : NULL;
            at = e ? atoi(e) : 0;
            to = d ? atoi(d + 1) : at;
        }
        vbl++;
        uint32_t hooks_frame(void);   /* also matches the game's vblank count (as in RUGRATS_SHOTLOG lines) */
        uint32_t hf = hooks_frame();
        gpu_cmdlog = at && ((vbl >= (uint32_t)at && vbl <= (uint32_t)to) || (hf >= (uint32_t)at && hf <= (uint32_t)to));
        if (gpu_cmdlog) rt_logf("gp0 log: vblank %u, display %d,%d mode %02x off %d\n", vbl, g.disp_x, g.disp_y, g.mode, g.disp_off);
    }
    if (!subpx_byflip() || ++subpx_noflip >= 3) { subpx_gen++; subpx_noflip = 0; }
    if (depth_vis < 0) depth_vis = getenv("RUGRATS_DEPTHVIS") != NULL;
    static int slog = -1;
    if (slog < 0) slog = getenv("RUGRATS_SUBPXLOG") != 0;
    if (slog && subpx_gen % 300 == 0) {   /* debug: share of polygon vertices that got a sub-pixel position */
        rt_logf("subpixel: %u of %u vertices (%.0f%%), %u live tags evicted\n", subpx_hit, subpx_n,
                subpx_n ? 100.0 * subpx_hit / subpx_n : 0.0, tag_evict);
        subpx_n = subpx_hit = 0; tag_evict = 0;
        /* fraction histogram in quarter pixels from -1 to +3 px (expect all in the [0,1) buckets 4..7) */
        char b[256]; int n = 0;
        for (int i = 0; i < 32; i++) n += snprintf(b + n, sizeof b - n, "%s%u", i == 16 ? " | y " : i ? " " : "x ", subpx_hist[i]);
        rt_logf("  %s\n", b);
        memset(subpx_hist, 0, sizeof subpx_hist);
    }
}

/* display info for the host */
void gpu_display(int *x, int *y, int *w, int *h, int *is24, int *off) {
    static const int widths[4] = { 256, 320, 512, 640 };
    int ww = (g.mode & 0x40) ? 368 : widths[g.mode & 3];
    int hh = ((g.mode & 4) && (g.mode & 0x20)) ? 480 : 240;
    int lines = g.v2 - g.v1;
    if (lines > 0 && lines < hh && !(g.mode & 0x20)) hh = lines;
    if (lines > 0 && (g.mode & 0x20) && lines * 2 < hh) hh = lines * 2;
    *x = g.disp_x; *y = g.disp_y; *w = ww; *h = hh;
    *is24 = (g.mode >> 4) & 1;
    *off = g.disp_off;
}

/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
void gpu_state(StateIO *s) {
    if (st_loading(s)) gpu_hr_sync();
    else gpu_native_sync(); /* GPU-only pixels back into native VRAM before it is saved */
    st_prefix(s, "gpu");
    ST(s, g.tpx); ST(s, g.tpy); ST(s, g.tdepth); ST(s, g.semi); ST(s, g.dither); ST(s, g.draw_disp); ST(s, g.tex_disable);
    ST(s, g.twmx); ST(s, g.twmy); ST(s, g.twox); ST(s, g.twoy);
    ST(s, g.dx1); ST(s, g.dy1); ST(s, g.dx2); ST(s, g.dy2); ST(s, g.offx); ST(s, g.offy);
    ST(s, g.mask_set); ST(s, g.mask_check); ST(s, g.rect_flipx); ST(s, g.rect_flipy);
    ST(s, g.disp_x); ST(s, g.disp_y); ST(s, g.h1); ST(s, g.h2); ST(s, g.v1); ST(s, g.v2); ST(s, g.mode);
    ST(s, g.disp_off); ST(s, g.dma_dir); ST(s, g.odd); ST(s, g.irq);
    ST(s, g.fifo); ST(s, g.fifo_n); ST(s, g.fifo_need);
    ST(s, g.xfer); ST(s, g.xx); ST(s, g.xy); ST(s, g.xw); ST(s, g.xh); ST(s, g.xcx); ST(s, g.xcy);
    ST(s, g.polyline); ST(s, g.gpuread);
    ST_N(s, "vram", vram, sizeof vram);
    if (!st_loading(s)) return;
    /* the hi-res picture is not saved: start it again as a copy of the restored native VRAM */
    late_mark = -1;
    memset(nl_map, 0, sizeof nl_map);
    nl_any = 0;
    if (hw_s) {
        hw_flush();
        hw_mark(0, 511);
        hw_area(0, 0, 1024, 512, HWF_BLIT, 0);
        hw_flush();
    } else if (hr.buf) {
        const int k = hr.s;
        for (int y = 0; y < 512 * k; y++)
            for (int x = 0; x < 1024 * k; x++) hr.buf[(size_t)y * hr.w + x] = vram[(y / k) * 1024 + x / k];
    }
}
