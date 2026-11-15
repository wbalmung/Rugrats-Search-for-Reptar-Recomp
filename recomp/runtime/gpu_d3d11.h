/* D3D11 hardware renderer for the hi-res picture (see gpu_d3d11.c). */
#pragma once
#include <stdint.h>

/* one vertex; positions in hi-res pixels (software coordinates), colours 0..255, uv in texels */
typedef struct {
    float x, y;
    float r, g, b;
    float u, v;
    uint32_t clut;   /* clutx | cluty << 16 (fill: the 15-bit colour) */
    uint32_t tpage;  /* tpx/64 | (tpy/256) << 4 | twmx << 8 | twmy << 13 | twox << 18 | twoy << 23 */
    uint32_t flags;  /* HWF_* */
    uint32_t uvlim;  /* umin | vmin << 8 | umax << 16 | vmax << 24; rects: u0 | v0 << 8 */
    float z;         /* depth check value (HW_DM_*): larger = nearer, 0 = nothing */
    float zn;        /* HW_DM_WNT room pieces: the near candidate depth (same scale as z) for room over object, else 0 */
    float pw;        /* perspective weight (1/z) for perspective-correct uv, 0 = affine (opt.perspective) */
    uint32_t rep;    /* texture pack (texpack.c tex_tag, set in hw_add): record id | region width << 16, 0 = none */
    uint32_t rrect;  /* region x | y << 10 | height << 19 (VRAM words) */
} HwVert;

void tex_tag(HwVert *tri, int dx0, int dy0, int dx1, int dy1);   /* texpack.c: rep / rrect of one triangle */
void tex_bind(void *ctx);          /* texpack.c: atlas (t2), records (t3), sampler (s0) */

/* depth check modes of a draw: none / write z always (room geometry, fills with z 0) / keep the pixel only where
   it is not more than the depth gap behind the stored depth (world objects; ps_test widens z per pixel) */
enum { HW_DM_NONE, HW_DM_WRITE, HW_DM_TEST, HW_DM_CLEAR, HW_DM_WNT, HW_DM_HUD, HW_DM_HTEST, HW_DM_TW };   /* CLEAR: fills (depth 0 = nothing);
   TW: object tested like TEST, then stores its own depth pushed back by (room gap - object gap) (opt.objsort);
   WNT: room piece with an unsure corner depth: writes depth like WRITE but is never hidden by the room test;
   HUD: 2D overlay drawn after the 3D scene, writes depth 1.0 (nothing later with 3D corners paints over it);
   HTEST: polygon with some 3D corners but no usable depth: hidden only on HUD pixels */

enum {
    HWF_DEPTH = 3, HWF_TEX = 4, HWF_RAW = 8, HWF_SEMI = 16, /* semi mode at bits 5-6 */
    HWF_DITHER = 128, HWF_MASK = 256, HWF_RECT = 512, HWF_FLIPX = 1024, HWF_FLIPY = 2048,
    HWF_FILL = 1 << 12, HWF_BLIT = 2 << 12,
    HWF_FULL = 1 << 14   /* shaded colour kept at full precision (no 5-bit truncation; opt.dither 2) */
};

int hw_begin(int s);               /* create / resize the hi-res target at s x; 0 = not available */
void hw_end(void);
int hw_scale(void);                /* 0 when off */
int hw_samples_req(void);          /* MSAA samples asked for when the target was made (gpu_msaa then) */
int hw_samples(void);              /* MSAA samples in use (1 = off; lower than asked when not supported), 0 = no target */
/* add triangles (n multiple of 3) with the native drawing area clip (inclusive), sub = semi mode 2 pair,
   dm = depth mode (HW_DM_*) */
void hw_add(const HwVert *v, int n, int sub, int dm, int cx0, int cy0, int cx1, int cy1);
void hw_note_read(int x0, int y0, int x1, int y1); /* native area queued draws read (texture/CLUT/blit) */
void hw_guard(int x0, int y0, int x1, int y1);     /* native VRAM is about to be written here */
void hw_mark(int y0, int y1);                      /* native rows changed (upload before next draws) */
void hw_copy(int sx, int sy, int dx, int dy, int w, int h);
void hw_flush(void);
const uint16_t *hw_readback(void); /* whole hi-res VRAM as 15-bit (slow; menu / test shots) */
int hw_read_native(int x0, int y0, int x1, int y1, uint16_t *out); /* native rect of the target -> out (pitch 1024) */
void hw_present(int sx, int sy, int w, int h, int dx, int dy, int dw, int dh, int smooth, int sync, int tear);
