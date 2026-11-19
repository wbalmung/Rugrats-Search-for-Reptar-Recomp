/* D3D11 hardware renderer for the hi-res picture.

   The native 1024x512 VRAM is still drawn exactly on the CPU (gpu.c): texture reads, VRAM copies and CPU
   readback come from it, as with the software shadow. This file replaces the software shadow: the hi-res
   VRAM is a GPU render target (RGBA8, 5-bit values / 31, mask bit in alpha) and every drawing command is
   drawn into it by a pixel shader that does the PS1's texel / CLUT / texture window / shading / dither
   logic, reading textures from a copy of the native VRAM (R16_UINT) that is re-uploaded (dirty rows) at
   each flush.

   Ordering: draws are batched. A native write into an area that batched draws still read (texture, CLUT,
   upload blit) flushes the batch first, so each draw sees the native VRAM as it was when it was queued
   (the same rule as the software shadow's guard_rect).

   Semi-transparency: modes 0, 1, 3 use dual-source blending (dest factor per pixel, so opaque texels in a
   semi-transparent primitive stay opaque). Mode 2 (subtract) is drawn twice: opaque texels with blending
   off, then semi texels with reverse subtract. The mask check bit (GP0 E6) is not emulated here. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_d3d11.h"
#include "present_d3d11.h"

void rt_logf(const char *fmt, ...);
extern uint16_t vram[512 * 1024];

static const char hw_src[] =
    /* MS = the MSAA build of every shader here (ms_init). The single-sample build must stay token for token what
       it was: even an untaken branch changed its float code (2026-11-15, ~100 pixels per frame). */
    "#ifdef MS\n"
    "#define HP 0.0\n"
    "#else\n"
    "#define HP 0.5\n"
    "#endif\n"
    "cbuffer C : register(b0) { float4 rt; uint4 cfg; float4 ex; uint4 cf2; };\n" /* rt: 2/W, 2/H, 1/s, 0.5/s; cfg: s, pass; cf2.x: anisotropic taps (1 = off) */
    "struct VI { float2 p : POS; float3 c : COLOR0; float2 t : TEXCOORD0; uint4 a : ATTR0; float z : DEPTH0; float zn : DEPTH1; float pw : DEPTH2;\n"
    "            uint2 r : ATTR1; };\n"
    "struct P { float4 p : SV_Position; noperspective float3 c : COLOR0; noperspective float3 tw : TEXCOORD0;\n"
    "           nointerpolation uint4 a : ATTR0; noperspective float zn : DEPTH1; nointerpolation uint2 r : ATTR1; };\n"
    /* +0.5: the pixel centre of hi-res pixel X lands on software coordinate X (same coverage and attributes) */
    /* MSAA (MS): HP 0 instead of +0.5, so screen-aligned edges (fills, sprites, HUD, frame border) cover whole
       pixels in every sample; shade() takes the texel position back to the old point instead */
    "P vs_main(VI i) { P o; o.p = float4((i.p.x + HP) * rt.x - 1, 1 - (i.p.y + HP) * rt.y, i.z, 1);\n"
    "  float w = i.pw > 0 ? i.pw : 1.0;\n" /* perspective: uv * w and w, divided per pixel (w 1 = affine, exact) */
    "  o.c = i.c; o.tw = float3(i.t * w, w); o.a = i.a; o.zn = i.zn; o.r = i.r; return o; }\n"
    "Texture2D<uint> vr : register(t0);\n"
    /* texture packs (texpack.c): atlas slices, 2 float4 per record {atlas rect}, {slice, half texel} */
    "Texture2DArray<float4> ta : register(t2);\n"
    "Buffer<float4> rb : register(t3);\n"
    "SamplerState sl : register(s0);\n"
    "uint V(int x, int y) { return vr.Load(int3(x & 1023, y & 511, 0)); }\n"
    "float4 C15(uint c) { return float4(float(c & 31u), float((c >> 5) & 31u), float((c >> 10) & 31u),\n"
    "                                   float((c >> 15) & 1u) * 31.0) / 31.0; }\n"
    "static const int DT[16] = { -4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2 };\n"
    "struct O { float4 c : SV_Target0; float4 f : SV_Target1; };\n"
    "O shade(P i);\n"
    "O ps_main(P i) { return shade(i); }\n"
    /* depth check testers (world objects): z = iz / 64; pass while the stored room depth <= iz / (1 - iz * k)
       (k = gap / 4096 in cfg.z), i.e. not more than the gap behind it */
    "struct OT { float4 c : SV_Target0; float4 f : SV_Target1; float d : SV_Depth; };\n"
    "OT ps_test(P i) {\n"
    "  O o = shade(i); OT t; t.c = o.c; t.f = o.f;\n"
    "  float iz = i.p.z * 64.0, k = iz * asfloat(cfg.z);\n"
    "  t.d = k >= 0.999 ? 1.0 : min(iz / (1.0 - k) / 64.0, 1.0);\n"
    "  return t;\n"
    "}\n"
    /* HUD protection (HTEST, and unsure room pieces): depth 0.999 fails only against HUD pixels (depth 1.0); real
       depths are iz / 64, far below. Tested, never written. */
    "OT ps_h(P i) {\n"
    "  O o = shade(i); OT t; t.c = o.c; t.f = o.f; t.d = 0.999;\n"
    "  return t;\n"
    "}\n"
    /* object sort fix (opt.objsort, gpu.c hr_oz): object depth in its own R32_FLOAT target `oz` (t1 while colour is
       drawn). Objects (TEST / TW) are also hidden where oz is nearer than iz / (1 - iz * k) (same gap rule). */
    /* MSAA (compiled with MS defined): oz is multi-sample and its readers run per sample (SV_SampleIndex) */
    "#ifdef MS\n"
    "#define SIP , uint si : SV_SampleIndex\n"
    "#define SI si\n"
    "Texture2DMS<float> oz : register(t1);\n"
    "float OZL(float2 p, uint s) { return oz.Load(int2(p), s); }\n"
    "#else\n"
    "#define SIP\n"
    "#define SI 0u\n"
    "Texture2D<float> oz : register(t1);\n"
    "float OZL(float2 p, uint s) { return oz.Load(int3(int2(p), 0)); }\n"
    "#endif\n"
    /* MSAA: the oz readers shade once per pixel and test oz for every sample, writing the passing samples as
       SV_Coverage (ANDed with the raster coverage by the hardware). They used to run per sample (SV_SampleIndex),
       which made 4x MSAA shade objects and most room pixels 4 times: 9x + 4x MSAA 4.3 -> ~3 ms GPU (goose level,
       2026-11-15). Depth tests use the pixel centre's depth, like every other MS shader. */
    "bool ohid(P i, float o) {\n"
    "  float iz = i.p.z * 64.0, k = iz * asfloat(cfg.z);\n"
    "  uint id = (i.a.z >> 16) & 0x1FFFu, oi = asuint(o) & 0x1FFFu;\n"
    "  if (o > 0 && k < 0.999 && iz / (1.0 - k) < o &&\n"
    "      ((id != 0u && oi == id) || ((oi & 0x1000u) != 0u && (i.a.z & 0x20000000u) != 0u) ||\n"
    "       (oi != 0u && (i.a.z & 0x80000000u) != 0u))) {\n"
    "    float kf = iz * (asfloat(cfg.z) + ex.w);\n"
    "    if ((oi == id && id != 0u) || (i.a.z & 0x40000000u) == 0u || (kf < 0.999 && iz / (1.0 - kf) < o)) return true;\n"
    "  }\n"
    "  return false;\n"
    "}\n"
    "#ifdef MS\n"
    "struct OTM { float4 c : SV_Target0; float4 f : SV_Target1; float d : SV_Depth; uint m : SV_Coverage; };\n"
    "struct OM { float4 c : SV_Target0; float4 f : SV_Target1; uint m : SV_Coverage; };\n"
    "uint NSM() { uint w, h, n; oz.GetDimensions(w, h, n); return n; }\n"
    "OTM ps_to(P i) {\n"
    "  OT t = ps_test(i); uint m = 0u, n = NSM();\n"
    "  [loop] for (uint s = 0u; s < n; s++) if (!ohid(i, OZL(i.p.xy, s))) m |= 1u << s;\n"
    "  if (m == 0u) discard;\n"
    "  OTM r; r.c = t.c; r.f = t.f; r.d = t.d; r.m = m; return r;\n"
    "}\n"
    "#else\n"
    "OT ps_to(P i SIP) {\n"
    "  OT t = ps_test(i);\n"
    "  float iz = i.p.z * 64.0, k = iz * asfloat(cfg.z), o = OZL(i.p.xy, SI);\n"
    /* model id: faces of the same model hide each other; a character's pixel (bit 12, OID_ACTOR) also hides faces
       of still props (flag bit 29 = OID_STATIC of the polygon, gpu.c exec_poly) */
    "  uint id = (i.a.z >> 16) & 0x1FFFu, oi = asuint(o) & 0x1FFFu;\n"
    "  if (o > 0 && k < 0.999 && iz / (1.0 - k) < o &&\n"
    "      ((id != 0u && oi == id) || ((oi & 0x1000u) != 0u && (i.a.z & 0x20000000u) != 0u) ||\n"
    /* glass drawn at the end of the list (bit 31, OID_GLASS, gpu.c glass_q): behind ANY stored model pixel */
    "       (oi != 0u && (i.a.z & 0x80000000u) != 0u))) {\n"
    /* floor-like still prop faces (bit 30, OID_FLOOR): only when also past the floor gap (ex.w = gpu_floor_k) */
    "    float kf = iz * (asfloat(cfg.z) + ex.w);\n"
    "    if ((oi == id && id != 0u) || (i.a.z & 0x40000000u) == 0u || (kf < 0.999 && iz / (1.0 - kf) < o)) discard;\n"
    "  }\n"
    "  return t;\n"
    "}\n"
    "#endif\n"
    /* oz draws (render target = oz): TW store iz / (1 - iz * k2), k2 = (room gap - object gap) / 4096 in cfg.w (blend
       MAX); room pieces and fills clear it (0) where they paint. SV_Depth as in their colour draw (ps_test / ps_h). */
    "struct OZ { float o : SV_Target0; float d : SV_Depth; };\n"
    "OZ ps_ow(P i) {\n"
    "  OT t = ps_test(i); OZ z; z.d = t.d;\n"
    "  float iz = i.p.z * 64.0, k = iz * asfloat(cfg.w);\n"
    "  z.o = asfloat((asuint(k >= 0.99 ? 1e29 : iz / (1.0 - k)) & ~0x1FFFu) | ((i.a.z >> 16) & 0x1FFFu));\n"
    "  return z;\n"
    "}\n"
    "OZ ps_oc(P i) { OT t = ps_test(i); OZ z; z.d = t.d; z.o = 0; return z; }\n"
    "OZ ps_och(P i) { O o = shade(i); OZ z; z.d = 0.999; z.o = 0; return z; }\n"
    /* room over object (gpu.c gpu_room_obj): a room pixel is left out where the stored object pixel is nearer than
       it (oz holds iz pushed by k2; ex.x = k2 - gap / 4096 takes that back; ex.y = 64 * s * slope: the room depth
       with its slope bias, as the depth buffer stores it) */
    /* zn > 0 (write-notest room pieces, gpu.c Vtx.izn): their near candidate depth instead of the stored far one */
    "float rzb(P i) { return (i.zn > 0 ? i.zn : i.p.z) * 64.0 - max(abs(ddx(i.p.z)), abs(ddy(i.p.z))) * ex.y; }\n"
    /* zlim: the room polygon's nearest corner + the room gap (flags bits 16-28, 512 steps per octave from 1/64).
       An object pixel nearer than that is nearer than the whole polygon: no sort tie, the game drew the polygon
       after it on purpose (gpu.c Vtx.zl) */
    "float zlim(P i) { return exp2(float((i.a.z >> 16) & 0x1FFFu) / 512.0 - 6.0); }\n"
    "bool rhid(P i, uint s) {\n"
    "  float o = OZL(i.p.xy, s), k = 1.0 + o * ex.x;\n"
    "  if (o <= 0 || (asuint(o) & 0x1FFFu) == 0u || k <= 0.001) return false;\n"   /* id 0 = a room mark */
    "  float ot = o / k;\n"
    "  return ot >= rzb(i) && (ex.z == 0.0 || ot <= zlim(i));\n"   /* ex.z: span condition on (gpu_room_obj_span) */
    "}\n"
    /* oz draws of a room run: marks = a depth as an object would have stored it, model id 0 (hides nothing: ps_to
       and rhid need an id). 1st, blend MAX, the room depth: replaces the object values behind this pixel, keeps the
       ones in front. 2nd (cfg.y == 3), blend MIN, zlim: replaces the ones too near to be kept by rhid. What is left
       are exactly the object values rhid kept. */
    "float rmark(P i) {\n"
    "  float rz = cfg.y == 3u ? zlim(i) : rzb(i), k = 1.0 - rz * ex.x;\n"
    "  return asfloat(asuint(rz <= 0 ? 0.0 : k <= 0.001 ? 1e29 : rz / k) & ~0x1FFFu);\n"
    "}\n"
    "OZ ps_om(P i) { OT t = ps_test(i); OZ z; z.d = t.d; z.o = rmark(i); return z; }\n"
    "OZ ps_omh(P i) { O o = shade(i); OZ z; z.d = 0.999; z.o = rmark(i); return z; }\n"
    "#ifdef MS\n"   /* per pixel + per-sample coverage (see ps_to); rzb's gradients are taken once, outside the loop */
    "bool rhidz(P i, float o, float rz) {\n"
    "  float k = 1.0 + o * ex.x;\n"
    "  if (o <= 0 || (asuint(o) & 0x1FFFu) == 0u || k <= 0.001) return false;\n"
    "  float ot = o / k;\n"
    "  return ot >= rz && (ex.z == 0.0 || ot <= zlim(i));\n"
    "}\n"
    "uint RM(P i) {\n"
    "  float rz = rzb(i); uint m = 0u, n = NSM();\n"
    "  [loop] for (uint s = 0u; s < n; s++) if (!rhidz(i, OZL(i.p.xy, s), rz)) m |= 1u << s;\n"
    "  if (m == 0u) discard;\n"
    "  return m;\n"
    "}\n"
    "OTM ps_rt(P i) { uint m = RM(i); OT t = ps_test(i); OTM r; r.c = t.c; r.f = t.f; r.d = t.d; r.m = m; return r; }\n"
    "OTM ps_rh(P i) { uint m = RM(i); OT t = ps_h(i); OTM r; r.c = t.c; r.f = t.f; r.d = t.d; r.m = m; return r; }\n"
    "OM ps_r(P i) { uint m = RM(i); O t = shade(i); OM r; r.c = t.c; r.f = t.f; r.m = m; return r; }\n"
    "#else\n"
    "OT ps_rt(P i SIP) { if (rhid(i, SI)) discard; return ps_test(i); }\n"
    "OT ps_rh(P i SIP) { if (rhid(i, SI)) discard; return ps_h(i); }\n"
    "O ps_r(P i SIP) { if (rhid(i, SI)) discard; return shade(i); }\n"
    "#endif\n"
    /* Early-Z depth check (2026-11-15, RUGRATS_EARLYZ, default 1; only with the room test pass): the testers above
       write SV_Depth = iz / (1 - iz k) / 64 (pushed nearer by the gap), which turns off the GPU's early depth test,
       so every hidden pixel (hedges behind a fence: GPU 3.6 ms looking up in Grandpa's Teeth) was fully shaded. The
       room depth pass instead stores S' = S / (1 + 64 S k) (ps_pw; same order, so "nearest wins" is unchanged) and
       the testers compare their plain depth: z / (1 - 64 z k) >= S  <=>  z >= S / (1 + 64 S k). Testers then need
       no SV_Depth: ps (for ps_test), ps_r (for ps_rt), ps_toe, and the oz draws ps_owe / ps_oce / ps_ome. The slope
       bias the rasterizer gave room writes (rs_w) is applied here, as rzb does (SV_Depth skips the rasterizer bias). */
    "float ps_pw(P i) : SV_Depth {\n"
    "  float zb = max(i.p.z - max(abs(ddx(i.p.z)), abs(ddy(i.p.z))) * ex.y / 64.0, 0.0);\n"
    "  O o = shade(i);\n"
    "  return zb / (1.0 + 64.0 * zb * asfloat(cfg.z));\n"
    "}\n"
    "#ifdef MS\n"
    "OM ps_toe(P i) {\n"
    "  O t = shade(i); uint m = 0u, n = NSM();\n"
    "  [loop] for (uint s = 0u; s < n; s++) if (!ohid(i, OZL(i.p.xy, s))) m |= 1u << s;\n"
    "  if (m == 0u) discard;\n"
    "  OM r; r.c = t.c; r.f = t.f; r.m = m; return r;\n"
    "}\n"
    "#else\n"
    "O ps_toe(P i) { O t = shade(i); if (ohid(i, OZL(i.p.xy, 0u))) discard; return t; }\n"
    "#endif\n"
    "float ps_owe(P i) : SV_Target0 {\n"
    "  O t = shade(i); float iz = i.p.z * 64.0, k = iz * asfloat(cfg.w);\n"
    "  return asfloat((asuint(k >= 0.99 ? 1e29 : iz / (1.0 - k)) & ~0x1FFFu) | ((i.a.z >> 16) & 0x1FFFu));\n"
    "}\n"
    "float ps_oce(P i) : SV_Target0 { O t = shade(i); return 0; }\n"
    "float ps_ome(P i) : SV_Target0 { O t = shade(i); return rmark(i); }\n"
    /* one triangle texel at texel position q, exactly as the centre lookup in shade (uv box clamp, texture window,
       4/8/15-bit, CLUT); for the anisotropic taps */
    "uint TX(float2 q, uint tp, uint lim, uint ca, uint depth) {\n"
    "  int u = clamp(int(floor(q.x + rt.w)), int(lim & 255u), int((lim >> 16) & 255u));\n"
    "  int v = clamp(int(floor(q.y + rt.w)), int((lim >> 8) & 255u), int(lim >> 24));\n"
    "  int twmx = int((tp >> 8) & 31u), twmy = int((tp >> 13) & 31u), twox = int((tp >> 18) & 31u), twoy = int((tp >> 23) & 31u);\n"
    "  u = ((u & ~(twmx * 8)) | ((twox & twmx) * 8)) & 255;\n"
    "  v = ((v & ~(twmy * 8)) | ((twoy & twmy) * 8)) & 255;\n"
    "  int tpx = int(tp & 15u) * 64, tpy = int((tp >> 4) & 1u) * 256, cx = int(ca & 0xFFFFu), cy = int(ca >> 16);\n"
    "  uint t;\n"
    "  if (depth == 0u) { uint w = V(tpx + (u >> 2), tpy + v); t = V(cx + int((w >> ((u & 3) * 4)) & 15u), cy); }\n"
    "  else if (depth == 1u) { uint w = V(tpx + (u >> 1), tpy + v); t = V(cx + int((w >> ((u & 1) * 8)) & 255u), cy); }\n"
    "  else t = V(tpx + u, tpy + v);\n"
    "  return t;\n"
    "}\n"
    "O shade(P i) {\n"
    "  O o; o.f = float4(0, 0, 0, 0);\n"
    "  uint fl = i.a.z;\n"
    "  float2 tt = i.tw.xy / i.tw.z;\n"
    /* anisotropic filtering (cf2.x taps max): texels per hi-res pixel along the longer screen axis; where the
       texture is squeezed (> 1 texel per pixel) up to cf2.x taps spread along that axis are averaged. Gradients
       here, before any branch. an = 1: nothing changes (Off). */
    "  float2 gx = ddx(tt), gy = ddy(tt); float lx = dot(gx, gx), ly = dot(gy, gy);\n"
    /* MSAA: geometry is not shifted by +0.5 (vs_main); the texel position half a pixel up-left = where the
       single-sample path samples it, so texel choice stays the same */
    "#ifdef MS\n"
    "  tt -= 0.5 * (gx + gy);\n"
    "#endif\n"
    "  float2 ax = lx >= ly ? gx : gy;\n"
    "  uint an = cf2.x > 1u ? min(cf2.x, uint(max(ceil(sqrt(max(lx, ly)) - 0.05), 1.0))) : 1u;\n"
    "  float3 tav = float3(0, 0, 0); bool av = false;\n"   /* averaged texel colour (5-bit units) */
    "  int2 np = int2(uint2(i.p.xy) / cfg.x);\n" /* native pixel (dither pattern, blits) */
    "  uint kind = (fl >> 12) & 3u;\n"
    "  if (kind == 1u) { o.c = C15(i.a.x); return o; }\n"
    "  if (kind == 2u) { o.c = C15(V(np.x, np.y)); return o; }\n"
    "  bool semi = (fl & 16u) != 0;\n"
    "  int r = int(floor(i.c.r + 0.5)), g = int(floor(i.c.g + 0.5)), b = int(floor(i.c.b + 0.5));\n"
    "  int d = (fl & 128u) != 0 ? DT[(np.y & 3) * 4 + (np.x & 3)] : 0;\n"
    "  uint px;\n"
    "  float3 fc = i.c.rgb;\n" /* full colour (HWF_FULL): unrounded 0..255 value, before 5-bit truncation */
    "  bool rep = i.r.x != 0u; float4 rc = float4(0, 0, 0, 0);\n" /* texture pack replacement (texpack.c) */
    "  if ((fl & 4u) != 0) {\n"
    "    int u, v; uint lim = i.a.w; float fu, fv;\n" /* fu, fv: position inside the texel (0..1), replacements only */
    "    if ((fl & 512u) != 0) {\n" /* sprite: texel = start +- native pixels from the corner */
    "      int ou = int(floor(tt.x + 0.25 * rt.z)), ov = int(floor(tt.y + 0.25 * rt.z));\n"
    "      u = int(lim & 255u) + ((fl & 1024u) != 0 ? -ou : ou);\n"
    "      v = int((lim >> 8) & 255u) + ((fl & 2048u) != 0 ? -ov : ov);\n"
    "      fu = saturate(tt.x - float(ou) + 0.5 * rt.z); fv = saturate(tt.y - float(ov) + 0.5 * rt.z);\n"
    "      if ((fl & 1024u) != 0) fu = 1.0 - fu;\n"
    "      if ((fl & 2048u) != 0) fv = 1.0 - fv;\n"
    "    } else {\n" /* rounding as the software renderer: one texel per native pixel step; stay inside the uv box */
    "      u = clamp(int(floor(tt.x + rt.w)), int(lim & 255u), int((lim >> 16) & 255u));\n"
    "      v = clamp(int(floor(tt.y + rt.w)), int((lim >> 8) & 255u), int(lim >> 24));\n"
    "      fu = frac(tt.x + rt.w); fv = frac(tt.y + rt.w);\n"
    "    }\n"
    "    uint tp = i.a.y;\n"
    "    int twmx = int((tp >> 8) & 31u), twmy = int((tp >> 13) & 31u), twox = int((tp >> 18) & 31u), twoy = int((tp >> 23) & 31u);\n"
    "    u = ((u & ~(twmx * 8)) | ((twox & twmx) * 8)) & 255;\n"
    "    v = ((v & ~(twmy * 8)) | ((twoy & twmy) * 8)) & 255;\n"
    "    int tpx = int(tp & 15u) * 64, tpy = int((tp >> 4) & 1u) * 256;\n"
    "    int cx = int(i.a.x & 0xFFFFu), cy = int(i.a.x >> 16);\n"
    "    uint depth = fl & 3u, t;\n"
    "    if (depth == 0u) { uint w = V(tpx + (u >> 2), tpy + v); t = V(cx + int((w >> ((u & 3) * 4)) & 15u), cy); }\n"
    "    else if (depth == 1u) { uint w = V(tpx + (u >> 1), tpy + v); t = V(cx + int((w >> ((u & 1) * 8)) & 255u), cy); }\n"
    "    else t = V(tpx + u, tpy + v);\n"
    /* replacement: the texel position inside the uploaded region -> the same spot of its picture in the atlas.
       Alpha < 0.5 = see-through; colour from the picture, semi-transparency bit from the original texel. */
    "    if (rep) {\n"
    "      uint id = i.r.x & 0xFFFFu; float kk = depth == 0u ? 4.0 : depth == 1u ? 2.0 : 1.0;\n"
    "      float rx = float(i.r.y & 1023u), ry = float((i.r.y >> 10) & 511u), rh = float((i.r.y >> 19) & 1023u), rw = float(i.r.x >> 16);\n"
    "      float2 n = float2((float(tpx) + (float(u) + fu) / kk - rx) / rw, (float(tpy + v) + fv - ry) / rh);\n"
    "      float4 ra = rb.Load(int(id * 2u)), rq = rb.Load(int(id * 2u + 1u));\n"
    "      float2 uv = clamp(ra.xy + saturate(n) * ra.zw, ra.xy + rq.yz, ra.xy + ra.zw - rq.yz);\n"
    "      rc = ta.SampleLevel(sl, float3(uv, rq.x), 0);\n"
    "      if (rc.a < 0.5) discard;\n"
    "    } else if (t == 0u) discard;\n"
    /* anisotropic taps (triangles only, not sprites / replacements): average of the opaque taps; the centre
       texel keeps deciding see-through and the semi-transparency bit (same coverage as Off) */
    "    else if (an > 1u && (fl & 512u) == 0u) {\n"
    "      float3 acc = float3(0, 0, 0); float n = 0;\n"
    "      [loop] for (uint k = 0; k < an; k++) {\n"
    "        uint s = TX(tt + ax * ((float(k) + 0.5) / float(an) - 0.5), tp, lim, i.a.x, depth);\n"
    "        if (s != 0u) { acc += float3(float(s & 31u), float((s >> 5) & 31u), float((s >> 10) & 31u)); n += 1; }\n"
    "      }\n"
    "      if (n > 0) {\n"
    "        tav = acc / n; av = true;\n"
    "        t = (t & 0x8000u) | uint(tav.r + 0.5) | (uint(tav.g + 0.5) << 5) | (uint(tav.b + 0.5) << 10);\n"
    "      }\n"
    "      if (cf2.y != 0u) { t = (t & 0x8000u) | 31u; tav = float3(31, 0, 0); av = true; }\n"   /* RUGRATS_ANISOSHOW=1: red where taps run */
    "    }\n"
    "    semi = semi && (t & 0x8000u) != 0;\n"
    "    if ((fl & 8u) != 0) px = t;\n"
    "    else {\n"
    "      int R = clamp((((int(t & 31u) << 3) * r) >> 7) + d, 0, 255);\n"
    "      int G = clamp((((int((t >> 5) & 31u) << 3) * g) >> 7) + d, 0, 255);\n"
    "      int B = clamp((((int((t >> 10) & 31u) << 3) * b) >> 7) + d, 0, 255);\n"
    "      px = uint(R >> 3) | (uint(G >> 3) << 5) | (uint(B >> 3) << 10) | (t & 0x8000u);\n"
    "      fc = (av ? tav : float3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u))) * i.c.rgb * (8.0 / 128.0);\n"
    "    }\n"
    "  } else {\n"
    "    r = clamp(r + d, 0, 255); g = clamp(g + d, 0, 255); b = clamp(b + d, 0, 255);\n"
    "    px = uint(r >> 3) | (uint(g >> 3) << 5) | (uint(b >> 3) << 10);\n"
    "  }\n"
    "  if ((fl & 256u) != 0) px |= 0x8000u;\n"
    "  if (cfg.y == 1u && semi) discard;\n"  /* subtract mode, pass 1: opaque texels only */
    "  if (cfg.y == 2u && !semi) discard;\n" /* pass 2: semi texels, reverse subtract */
    "  o.c = C15(px);\n"
    /* the target stores 5-bit value / 31 = 8-bit value / 248 (present scales 1.0 back to 248/255) */
    "  if (rep) { float3 c8 = rc.rgb * 255.0; o.c.rgb = saturate(((fl & 8u) != 0 ? c8 : c8 * i.c.rgb / 128.0) / 248.0); }\n"
    "  else if ((fl & 16384u) != 0 && (fl & 8u) == 0) o.c.rgb = saturate(fc / 248.0);\n"
    "  if (cfg.y == 0u && semi) {\n"
    "    uint m = (fl >> 5) & 3u;\n"
    "    if (m == 0u) { o.c.rgb *= 0.5; o.f = float4(0.5, 0.5, 0.5, 0.5); }\n"
    "    else if (m == 3u) { o.c.rgb *= 0.25; o.f = float4(1, 1, 1, 1); }\n"
    "    else o.f = float4(1, 1, 1, 1);\n"
    "  }\n"
    "  return o;\n"
    "}\n";

/* MSAA helpers: resolve (average of a multi-sample target's samples, colour in linear light, alpha = mask bit as is)
   and per-sample copy (a VRAM copy between areas of a multi-sample target goes through a multi-sample scratch:
   D3D11 copies of multi-sample resources must be whole subresources). Full-screen triangle over the viewport;
   pixel position + off = source texel. */
static const char ms_src[] =
    "Texture2DMS<float4> ms : register(t0);\n"
    "cbuffer K : register(b0) { int4 off; };\n"
    "float4 vs_fs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 t = float2((id << 1) & 2, id & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps_res(float4 p : SV_Position) : SV_Target {\n"
    "  uint w, h, n; ms.GetDimensions(w, h, n);\n"
    "  float3 c = float3(0, 0, 0); float a = 0;\n"
    "  for (uint k = 0; k < n; k++) { float4 s = ms.Load(int2(p.xy) + off.xy, k); c += pow(max(s.rgb, 0.0), 2.2); a += s.a; }\n"
    "  return float4(pow(max(c / float(n), 0.0), 1.0 / 2.2), a / float(n)); }\n"
    "float4 ps_cpy(float4 p : SV_Position, uint si : SV_SampleIndex) : SV_Target { return ms.Load(int2(p.xy) + off.xy, si); }\n";

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
/* MSAA (opt.msaa, gpu_msaa): NS samples in rt / dtex / otex; rt1 = single-sample resolve of rt (present, readback);
   with NS 1 rt1 is rt and nothing below is used */
static int NS = 1, ns_req = 1;
static ID3D11Texture2D *rt1;
static ID3D11RenderTargetView *rtv1;
static ID3D11ShaderResourceView *rsrv1;
static ID3D11Texture2D *scr_ms;          /* MS copy scratch */
static ID3D11RenderTargetView *scr_rtv;
static ID3D11ShaderResourceView *scr_srv;
static int scr_w, scr_h;
static ID3D11VertexShader *vs_fs;
static ID3D11PixelShader *ps_res, *ps_cpy;
static ID3D11Buffer *kcb;
/* every hw_src shader in a single-sample and an MSAA (MS) build; the active pointers (vs, ps, ps_t, ...) are set
   from these in hw_begin. Index 0 = vertex shader. */
#define NSH 18
static const char *const sh_name[NSH] = { "vs_main", "ps_main", "ps_test", "ps_h", "ps_to", "ps_ow", "ps_oc", "ps_och",
                                          "ps_rt", "ps_rh", "ps_r", "ps_om", "ps_omh",
                                          "ps_pw", "ps_toe", "ps_owe", "ps_oce", "ps_ome" };
static void *sh_ss[NSH], *sh_ms[NSH];
static void **sh_slot(int k);
static int ms_shaders;                   /* 0 not tried, 1 ok, -1 failed */
int gpu_msaa(void);                      /* gpu.c: opt.msaa samples (1 = off) */
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps, *ps_t;   /* ps_t: depth-check testers (writes SV_Depth with the gap) */
static ID3D11PixelShader *ps_h;         /* HUD protection test (SV_Depth 0.999) */
static ID3D11PixelShader *ps_to, *ps_ow, *ps_oc, *ps_och;   /* object sort fix: object tester / oz store / oz clear */
static ID3D11PixelShader *ps_rt, *ps_rh, *ps_r;   /* room over object: room testers that skip nearer object pixels */
static ID3D11PixelShader *ps_om, *ps_omh;         /* room over object: oz draw of a room run (room mark, MAX) */
static ID3D11PixelShader *ps_pw, *ps_toe, *ps_owe, *ps_oce, *ps_ome;   /* early-Z depth check (see ps_pw) */
static int early_z(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_EARLYZ"); z = e ? atoi(e) != 0 : 1; } return z; }
int gpu_room_obj(void);
float gpu_room_obj_k(void);
float gpu_floor_k(void);
int gpu_aniso(void);                    /* gpu.c: opt.aniso, most anisotropic taps (1 = off) */
int gpu_room_obj_wnt(void);
int gpu_room_obj_span(void);
static ID3D11BlendState *bs_max;        /* oz store: keep the nearest */
static ID3D11BlendState *bs_min;        /* room mark 2 */
static ID3D11Texture2D *otex;           /* oz: object depth (R32_FLOAT, same size as rt), 0 = none */
static ID3D11RenderTargetView *ortv;
static ID3D11ShaderResourceView *osrv;
float gpu_depth_k(void);                /* gpu.c: depth gap / 4096 */
float gpu_objsort_k(void);              /* gpu.c: (room gap - object gap) / 4096 */
static ID3D11InputLayout *layout;
static ID3D11Buffer *vb, *cb;
static ID3D11BlendState *bs_dual, *bs_sub;
static ID3D11RasterizerState *rs;
static ID3D11RasterizerState *rs_w;     /* room depth writes: slope bias (gpu_depth_slope native pixels) */
static int rs_w_s;                      /* scale rs_w was made for */
float gpu_depth_slope(void);
static ID3D11DepthStencilState *ds_off, *ds_write, *ds_test, *ds_pre;   /* ds_pre: room depth pass (nearest wins) */
static ID3D11BlendState *bs_none;                                        /* no colour writes */
int gpu_room_test(void);
static ID3D11Texture2D *dtex;                /* depth for the depth check (opt.depthfix), D32_FLOAT, same size as rt */
static ID3D11DepthStencilView *dsv;
static ID3D11Texture2D *vtex;                /* native VRAM copy, R16_UINT 1024x512 */
static ID3D11ShaderResourceView *vsrv;
static ID3D11Texture2D *rt;                  /* hi-res VRAM */
static ID3D11RenderTargetView *rtv;
static ID3D11ShaderResourceView *rsrv;
static ID3D11Texture2D *scratch, *staging;
static int scratch_w, scratch_h;
static uint16_t *rb_buf;
static int S, W, H, shaders_ok;

#define VB_MAX (1 << 18)
#define RUN_MAX 16384
static HwVert *verts;
static int nverts;
static struct { int sub, dm, x0, y0, x1, y1, first, n; } runs[RUN_MAX];
static int nruns, nsegs;
uint64_t hw_prof_tests;
static int rd_x0 = 1 << 30, rd_y0, rd_x1 = -1, rd_y1; /* native area queued draws read */
static int dirty_y0 = 0, dirty_y1 = 511;              /* native rows to upload */

static int compile_src(pD3DCompile comp, const char *src, size_t len, const D3D_SHADER_MACRO *def, const char *entry,
                       const char *target, ID3DBlob **out) {
    ID3DBlob *err = NULL;
    HRESULT hr = comp(src, len, "hwgpu", def, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (err) {
        rt_logf("hwgpu: shader %s%s: %s\n", entry, def ? " (MS)" : "", (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    return SUCCEEDED(hr);
}

static int compile(pD3DCompile comp, const char *entry, const char *target, ID3DBlob **out) {
    ID3DBlob *err = NULL;
    HRESULT hr = comp(hw_src, sizeof hw_src - 1, "hwgpu", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (err) {
        rt_logf("hwgpu: shader %s: %s\n", entry, (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    return SUCCEEDED(hr);
}

static int init_shared(void) {
    if (shaders_ok) return 1;
    dev = d3d_device();
    ctx = d3d_context();
    pD3DCompile comp = (pD3DCompile)d3d_compiler();
    if (!dev || !ctx || !comp) return 0;
    ID3DBlob *bv = NULL, *bp = NULL;
    if (!compile(comp, "vs_main", "vs_4_0", &bv) || !compile(comp, "ps_main", "ps_4_0", &bp)) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bp), ID3D10Blob_GetBufferSize(bp), NULL, &ps);
    {
        ID3DBlob *bt = NULL;
        if (!compile(comp, "ps_test", "ps_4_0", &bt)) return 0;
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bt), ID3D10Blob_GetBufferSize(bt), NULL, &ps_t);
        ID3D10Blob_Release(bt);
        bt = NULL;   /* missing ps_h: no HUD protection (draws as before) */
        if (compile(comp, "ps_h", "ps_4_0", &bt)) {
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bt), ID3D10Blob_GetBufferSize(bt), NULL, &ps_h);
            ID3D10Blob_Release(bt);
        }
        /* object sort fix shaders; any missing: TW objects are only tested (no object sort fix) */
        const char *on[14] = { "ps_to", "ps_ow", "ps_oc", "ps_och", "ps_rt", "ps_rh", "ps_r", "ps_om", "ps_omh",
                               "ps_pw", "ps_toe", "ps_owe", "ps_oce", "ps_ome" };
        ID3D11PixelShader **op[14] = { &ps_to, &ps_ow, &ps_oc, &ps_och, &ps_rt, &ps_rh, &ps_r, &ps_om, &ps_omh,
                                       &ps_pw, &ps_toe, &ps_owe, &ps_oce, &ps_ome };
        for (int k = 0; k < 14; k++) {
            bt = NULL;
            if (compile(comp, on[k], "ps_4_0", &bt)) {
                ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bt), ID3D10Blob_GetBufferSize(bt), NULL, op[k]);
                ID3D10Blob_Release(bt);
            }
        }
    }
    D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "ATTR", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DEPTH", 0, DXGI_FORMAT_R32_FLOAT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DEPTH", 1, DXGI_FORMAT_R32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DEPTH", 2, DXGI_FORMAT_R32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "ATTR", 1, DXGI_FORMAT_R32G32_UINT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11Device_CreateInputLayout(dev, el, 8, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), &layout);
    ID3D10Blob_Release(bv);
    ID3D10Blob_Release(bp);

    D3D11_BUFFER_DESC bd = { 0 };
    bd.ByteWidth = VB_MAX * sizeof(HwVert);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb);
    D3D11_BUFFER_DESC cd = { 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ID3D11Device_CreateBuffer(dev, &cd, NULL, &cb);

    D3D11_BLEND_DESC b = { 0 };
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &b, &bs_dual);
    b.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
    ID3D11Device_CreateBlendState(dev, &b, &bs_sub);
    b.RenderTarget[0].BlendEnable = FALSE;
    b.RenderTarget[0].RenderTargetWriteMask = 0;
    ID3D11Device_CreateBlendState(dev, &b, &bs_none);
    b.RenderTarget[0].BlendEnable = TRUE;   /* oz store: max(stored, new) */
    b.RenderTarget[0].SrcBlend = b.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
    b.RenderTarget[0].SrcBlendAlpha = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &b, &bs_max);
    b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MIN;
    ID3D11Device_CreateBlendState(dev, &b, &bs_min);

    D3D11_RASTERIZER_DESC r = { 0 };
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.ScissorEnable = TRUE;
    r.DepthClipEnable = FALSE;
    ID3D11Device_CreateRasterizerState(dev, &r, &rs);

    D3D11_DEPTH_STENCIL_DESC d = { 0 };
    ID3D11Device_CreateDepthStencilState(dev, &d, &ds_off);
    d.DepthEnable = TRUE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11Device_CreateDepthStencilState(dev, &d, &ds_write);
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ID3D11Device_CreateDepthStencilState(dev, &d, &ds_test);
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ID3D11Device_CreateDepthStencilState(dev, &d, &ds_pre);

    D3D11_TEXTURE2D_DESC t = { 0 };
    t.Width = 1024; t.Height = 512; t.MipLevels = 1; t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R16_UINT;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device_CreateTexture2D(dev, &t, NULL, &vtex);
    if (vtex) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)vtex, NULL, &vsrv);

    verts = malloc(VB_MAX * sizeof(HwVert));
    shaders_ok = vs && ps && layout && vb && cb && bs_dual && bs_sub && rs && vsrv && verts && ds_off && ds_write && ds_test && ds_pre && bs_none;
    if (!shaders_ok) rt_logf("hwgpu: setup failed\n");
    for (int k = 0; k < NSH; k++) sh_ss[k] = *sh_slot(k);
    return shaders_ok;
}

static void rel(void **p) {
    if (*p) { IUnknown_Release((IUnknown *)*p); *p = NULL; }
}

static void **sh_slot(int k) {
    static void **const s[NSH] = { (void **)&vs, (void **)&ps, (void **)&ps_t, (void **)&ps_h, (void **)&ps_to, (void **)&ps_ow,
                                   (void **)&ps_oc, (void **)&ps_och, (void **)&ps_rt, (void **)&ps_rh, (void **)&ps_r,
                                   (void **)&ps_om, (void **)&ps_omh, (void **)&ps_pw, (void **)&ps_toe, (void **)&ps_owe,
                                   (void **)&ps_oce, (void **)&ps_ome };
    return s[k];
}

/* MSAA shaders (once): the MS build of every hw_src shader (4_1: per-sample oz readers), resolve, copy. 1 = usable
   (every shader that exists single-sample must exist as MS too). */
static int ms_init(void) {
    if (ms_shaders) return ms_shaders > 0;
    ms_shaders = -1;
    pD3DCompile comp = (pD3DCompile)d3d_compiler();
    if (!comp) return 0;
    static const D3D_SHADER_MACRO def[] = { { "MS", "1" }, { NULL, NULL } };
    int all = 1;
    for (int k = 0; k < NSH; k++) {
        if (!sh_ss[k]) continue;
        ID3DBlob *b = NULL;
        if (compile_src(comp, hw_src, sizeof hw_src - 1, def, sh_name[k], k ? "ps_4_1" : "vs_4_1", &b)) {
            if (k) ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, (ID3D11PixelShader **)&sh_ms[k]);
            else ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, (ID3D11VertexShader **)&sh_ms[k]);
            ID3D10Blob_Release(b);
        }
        if (!sh_ms[k]) all = 0;
    }
    ID3DBlob *a = NULL, *b = NULL, *c = NULL;
    if (compile_src(comp, ms_src, sizeof ms_src - 1, NULL, "vs_fs", "vs_4_1", &a) &&
        compile_src(comp, ms_src, sizeof ms_src - 1, NULL, "ps_res", "ps_4_1", &b) &&
        compile_src(comp, ms_src, sizeof ms_src - 1, NULL, "ps_cpy", "ps_4_1", &c)) {
        ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(a), ID3D10Blob_GetBufferSize(a), NULL, &vs_fs);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps_res);
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(c), ID3D10Blob_GetBufferSize(c), NULL, &ps_cpy);
    }
    if (a) ID3D10Blob_Release(a);
    if (b) ID3D10Blob_Release(b);
    if (c) ID3D10Blob_Release(c);
    D3D11_BUFFER_DESC kd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    ID3D11Device_CreateBuffer(dev, &kd, NULL, &kcb);
    const int ok = all && vs_fs && ps_res && ps_cpy && kcb;
    if (!ok) rt_logf("hwgpu: MSAA shaders not available (needs D3D 10.1), MSAA off\n");
    ms_shaders = ok ? 1 : -1;
    return ok;
}

/* full-screen triangle draw of src (multi-sample SRV) into dst over (x, y, w, h); source texel = pixel + (ox, oy).
   ps_res averages the samples (resolve), ps_cpy copies sample by sample. */
static void ms_draw(ID3D11PixelShader *p, ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, int x, int y, int w, int h,
                    int ox, int oy) {
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &dst, NULL);
    D3D11_VIEWPORT vp = { (float)x, (float)y, (float)w, (float)h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    D3D11_RECT sc = { x, y, x + w, y + h };
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs_fs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, p, NULL, 0);
    const int32_t k[4] = { ox, oy, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)kcb, 0, NULL, k, 0, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &kcb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
}

/* rt region (hi-res pixels) -> rt1 (single sample) */
static void resolve(int x, int y, int w, int h) { if (NS > 1 && w > 0 && h > 0) ms_draw(ps_res, rsrv, rtv1, x, y, w, h, 0, 0); }

static void free_target(void) {
    rel((void **)&rtv1); rel((void **)&rsrv1); rel((void **)&rt1);
    rel((void **)&scr_rtv); rel((void **)&scr_srv); rel((void **)&scr_ms);
    scr_w = scr_h = 0;
    rel((void **)&rtv); rel((void **)&rsrv); rel((void **)&rt);
    rel((void **)&dsv); rel((void **)&dtex);
    rel((void **)&ortv); rel((void **)&osrv); rel((void **)&otex);
    rel((void **)&staging); rel((void **)&scratch);
    scratch_w = scratch_h = 0;
    free(rb_buf); rb_buf = NULL;
}

int hw_scale(void) { return S; }
int hw_samples_req(void) { return ns_req; }
int hw_samples(void) { return S ? NS : 0; }

void hw_end(void) {
    if (!S) return;
    hw_flush();
    free_target();
    S = 0;
}

static void quad(HwVert *o, float x0, float y0, float x1, float y1, uint32_t a0, uint32_t fl) {
    HwVert c[4];
    memset(c, 0, sizeof c);
    for (int k = 0; k < 4; k++) {
        c[k].x = (k & 1) ? x1 : x0; c[k].y = (k & 2) ? y1 : y0;
        c[k].clut = a0; c[k].flags = fl;
    }
    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[1]; o[4] = c[3]; o[5] = c[2];
}

int hw_begin(int s) {
    const int want = gpu_msaa();
    if (S == s && rt && ns_req == want) return 1;
    hw_end();
    if (!init_shared()) return 0;
    ns_req = want;
    int n = want;
    if (n > 1 && !ms_init()) n = 1;
    while (n > 1) {   /* every multi-sample format must support n samples */
        UINT q0 = 0, q1 = 0, q2 = 0;
        ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)n, &q0);
        ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_D32_FLOAT, (UINT)n, &q1);
        ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_R32_FLOAT, (UINT)n, &q2);
        if (q0 && q1 && q2) break;
        rt_logf("hwgpu: %dx MSAA not supported, trying %dx\n", n, n / 2);
        n /= 2;
    }
again:;
    D3D11_TEXTURE2D_DESC t = { 0 };
    t.Width = 1024 * s; t.Height = 512 * s; t.MipLevels = 1; t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = (UINT)n;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &t, NULL, &rt);
    if (FAILED(hr) && n > 1) { rt_logf("hwgpu: no %dx MSAA %dx%d target (%08lx), MSAA off\n", n, t.Width, t.Height, hr); n = 1; goto again; }
    if (FAILED(hr)) { rt_logf("hwgpu: no %dx%d target (%08lx)\n", t.Width, t.Height, hr); return 0; }
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)rt, NULL, &rsrv);
    if (!rtv || !rsrv) { free_target(); return 0; }
    t.Format = DXGI_FORMAT_D32_FLOAT;
    t.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &dtex)))
        ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)dtex, NULL, &dsv);
    if (dsv) ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    else { rel((void **)&dtex); rt_logf("hwgpu: no depth target, depth check off\n"); }
    if (dsv) {   /* object depth (object sort fix) */
        t.Format = DXGI_FORMAT_R32_FLOAT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &otex))) {
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)otex, NULL, &ortv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)otex, NULL, &osrv);
        }
        if (ortv && osrv) { const float zero[4] = { 0 }; ID3D11DeviceContext_ClearRenderTargetView(ctx, ortv, zero); }
        else { rel((void **)&ortv); rel((void **)&osrv); rel((void **)&otex); rt_logf("hwgpu: no object depth target, object sort fix off\n"); }
    }
    if (n > 1) {   /* single-sample resolve target; any missing multi-sample piece (memory) = MSAA off */
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &rt1))) {
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt1, NULL, &rtv1);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)rt1, NULL, &rsrv1);
        }
        if (!rtv1 || !rsrv1 || !dsv || !ortv) {
            rt_logf("hwgpu: %dx MSAA targets incomplete (memory?), MSAA off\n", n);
            free_target();
            n = 1;
            goto again;
        }
    }
    NS = n;
    for (int k = 0; k < NSH; k++) *sh_slot(k) = NS > 1 ? sh_ms[k] : sh_ss[k];   /* MS builds with MSAA */
    S = s; W = 1024 * s; H = 512 * s;
    nverts = nruns = nsegs = 0;
    rd_x0 = 1 << 30; rd_x1 = -1;
    /* start as a nearest-neighbour copy of the native VRAM */
    dirty_y0 = 0; dirty_y1 = 511;
    HwVert q[6];
    quad(q, 0, 0, (float)W, (float)H, 0, HWF_BLIT);
    hw_add(q, 6, 0, HW_DM_NONE, 0, 0, 1023, 511);
    hw_flush();
    if (NS > 1) rt_logf("hwgpu: D3D11 renderer at %dx (%dx%d), %dx MSAA\n", s, W, H, NS);
    else rt_logf("hwgpu: D3D11 renderer at %dx (%dx%d)\n", s, W, H);
    return 1;
}

void hw_mark(int y0, int y1) {
    if (y1 > 511 || y0 < 0) { y0 = 0; y1 = 511; }
    if (dirty_y0 > dirty_y1) { dirty_y0 = y0; dirty_y1 = y1; return; }
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

void hw_note_read(int x0, int y0, int x1, int y1) {
    if (rd_x0 > rd_x1) { rd_x0 = x0; rd_x1 = x1; rd_y0 = y0; rd_y1 = y1; return; }
    if (x0 < rd_x0) rd_x0 = x0;
    if (x1 > rd_x1) rd_x1 = x1;
    if (y0 < rd_y0) rd_y0 = y0;
    if (y1 > rd_y1) rd_y1 = y1;
}

void hw_guard(int x0, int y0, int x1, int y1) {
    if (!S) return;
    if (x1 > 1023 || y1 > 511) { x0 = y0 = 0; x1 = 1023; y1 = 511; }
    if (rd_x0 <= rd_x1 && !(x1 < rd_x0 || x0 > rd_x1 || y1 < rd_y0 || y0 > rd_y1)) hw_flush();
    hw_mark(y0, y1);
}

/* 2D triangles a, b (3 vertices each): 1 when their interiors may overlap (separating axis test on the 6 edge
   normals). Triangles that only share an edge or a corner are separated (same float maths on the shared vertices,
   and D3D's top-left rule never gives one pixel to both); other near-touching pairs need a 1/128 px gap. Zero-area triangles draw
   nothing: separated. */
static int tri_overlap(const HwVert *a, const HwVert *b) {
    for (int t = 0; t < 2; t++) {
        const HwVert *p = t ? b : a;
        for (int e = 0; e < 3; e++) {
            float nx = p[(e + 1) % 3].y - p[e].y, ny = p[e].x - p[(e + 1) % 3].x;
            if (nx == 0 && ny == 0) continue;
            float a0 = a[0].x * nx + a[0].y * ny, a1 = a0, b0 = b[0].x * nx + b[0].y * ny, b1 = b0;
            for (int k = 1; k < 3; k++) {
                float d = a[k].x * nx + a[k].y * ny; if (d < a0) a0 = d; if (d > a1) a1 = d;
                d = b[k].x * nx + b[k].y * ny; if (d < b0) b0 = d; if (d > b1) b1 = d;
            }
            /* apart by a gap of 1/128 px (vertices snap to 1/256 px), or touching exactly (shared edge) */
            const float g2 = (nx * nx + ny * ny) * (1.0f / 16384.0f);
            if (a1 == b0 || b1 == a0 || (a1 < b0 && (b0 - a1) * (b0 - a1) >= g2) || (b1 < a0 && (a0 - b1) * (a0 - b1) >= g2)) return 0;
        }
    }
    return 1;
}

/* Run merging (RUGRATS_RUNMERGE, default 1; 0 = old behaviour: only a piece right after a run with the same state
   joins it, TW never). A new piece joins the latest run with the same state (sub, depth mode, clip) when none of its
   triangles can cover a pixel that a triangle of a later run covers: moving a draw past draws on other pixels
   changes nothing (every buffer here - colour, depth, oz - is read and written per pixel). TW runs also need their
   own triangles not to overlap (each stores its oz value after the whole run's colour draw; later triangles on the
   same pixels must see it). Why: the game draws objects (TW) and room pieces (WRITE) interleaved in its OT order, and
   every TW / room run costs a render target switch (colour -> oz -> colour) that stalls the GPU. The space station
   (db 7?) had ~1000 runs per frame -> ~4.7 ms of GPU time at any resolution (2026-11-12). Vertices stay in add order
   in `verts` (segments); hw_flush writes them to the vertex buffer grouped by run. */
#define SEG_MAX 32768
#define LOOK_RUNS 24      /* how far back (in runs) a piece may look for a run with the same state */
#define LOOK_TRIS 2048    /* most triangles / segments checked for one piece (else it starts a new run) */
static struct { int run, first, n, next; float x0, y0, x1, y1; } segs[SEG_MAX];   /* next: the run's next segment */
static int run_seg0[RUN_MAX], run_segl[RUN_MAX];   /* first / last segment of each run */
static float run_bb[RUN_MAX][4];                   /* each run's bounding box */
static int run_merge(void) { static int z = -1; if (z < 0) { const char *e = getenv("RUGRATS_RUNMERGE"); z = e ? atoi(e) : 1; } return z; }

static void tri_box(const HwVert *t, int n, float *b) {
    b[0] = b[2] = t[0].x; b[1] = b[3] = t[0].y;
    for (int k = 1; k < n; k++) {
        if (t[k].x < b[0]) b[0] = t[k].x; if (t[k].x > b[2]) b[2] = t[k].x;
        if (t[k].y < b[1]) b[1] = t[k].y; if (t[k].y > b[3]) b[3] = t[k].y;
    }
}

static int box_apart(const float *a, const float *b) { return a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1]; }

/* 1 when the n new vertices v (box bb) may join run K: no overlap with any triangle of a later run (and of K itself
   when self) */
static int seg_clear(int K, int self, const HwVert *v, int n, const float *bb) {
    int budget = LOOK_TRIS;
    for (int r = self ? K : K + 1; r < nruns; r++) {
        if (box_apart(bb, run_bb[r])) continue;
        for (int s = run_seg0[r]; s >= 0; s = segs[s].next) {
            if (--budget < 0) return 0;
            const float sb[4] = { segs[s].x0, segs[s].y0, segs[s].x1, segs[s].y1 };
            if (box_apart(bb, sb)) continue;
            for (int t = 0; t + 3 <= segs[s].n; t += 3) {
                const HwVert *o = verts + segs[s].first + t;
                if (--budget < 0) return 0;
                float ob[4];
                tri_box(o, 3, ob);
                if (box_apart(bb, ob)) continue;
                for (int u = 0; u + 3 <= n; u += 3) {
                    float nb[4];
                    tri_box(v + u, 3, nb);
                    hw_prof_tests++;
                    if (!box_apart(nb, ob) && tri_overlap(v + u, o)) return 0;
                }
            }
        }
    }
    return 1;
}

void hw_add(const HwVert *v, int n, int sub, int dm, int cx0, int cy0, int cx1, int cy1) {
    if (!S || n <= 0) return;
    if (!dsv) dm = HW_DM_NONE;
    if (nverts + n > VB_MAX || nruns >= RUN_MAX || nsegs >= SEG_MAX) hw_flush();
    if (n > VB_MAX) return;
    int K = -1;
    for (int i = nruns - 1; i >= 0 && i >= nruns - LOOK_RUNS; i--)
        if (runs[i].sub == sub && runs[i].dm == dm && runs[i].x0 == cx0 && runs[i].y0 == cy0 && runs[i].x1 == cx1 && runs[i].y1 == cy1) { K = i; break; }
        else if (!run_merge()) break;
    float bb[4];
    tri_box(v, n, bb);
    int join = K >= 0 && K == nruns - 1 && dm != HW_DM_TW;   /* old rule: right after a run with the same state */
    if (!join && K >= 0 && run_merge()) join = seg_clear(K, dm == HW_DM_TW, v, n, bb);
    memcpy(verts + nverts, v, (size_t)n * sizeof *v);
    for (int t = 0; t + 3 <= n; t += 3) tex_tag(verts + nverts + t, cx0, cy0, cx1, cy1);   /* texture packs; also clears rep for all */
    if (join) {
        runs[K].n += n;
        float *rb = run_bb[K];
        if (bb[0] < rb[0]) rb[0] = bb[0]; if (bb[1] < rb[1]) rb[1] = bb[1]; if (bb[2] > rb[2]) rb[2] = bb[2]; if (bb[3] > rb[3]) rb[3] = bb[3];
    } else {
        K = nruns++;
        runs[K].sub = sub; runs[K].dm = dm; runs[K].x0 = cx0; runs[K].y0 = cy0; runs[K].x1 = cx1; runs[K].y1 = cy1;
        runs[K].n = n;
        run_seg0[K] = run_segl[K] = -1;
        memcpy(run_bb[K], bb, sizeof bb);
    }
    const int l = run_segl[K];
    if (l >= 0 && l == nsegs - 1 && segs[l].first + segs[l].n == nverts) {   /* extend the run's last segment */
        segs[l].n += n;
        if (bb[0] < segs[l].x0) segs[l].x0 = bb[0]; if (bb[2] > segs[l].x1) segs[l].x1 = bb[2];
        if (bb[1] < segs[l].y0) segs[l].y0 = bb[1]; if (bb[3] > segs[l].y1) segs[l].y1 = bb[3];
    } else {
        segs[nsegs].run = K; segs[nsegs].first = nverts; segs[nsegs].n = n; segs[nsegs].next = -1;
        segs[nsegs].x0 = bb[0]; segs[nsegs].y0 = bb[1]; segs[nsegs].x1 = bb[2]; segs[nsegs].y1 = bb[3];
        if (l >= 0) segs[l].next = nsegs; else run_seg0[K] = nsegs;
        run_segl[K] = nsegs++;
    }
    nverts += n;
}

/* RUGRATS_ANISOSHOW=1 (debug): pixels that take anisotropic taps are drawn red */
static int aniso_show(void) { static int z = -1; if (z < 0) z = getenv("RUGRATS_ANISOSHOW") != NULL; return z; }

static void set_pass(int pass) {
    const int an = gpu_aniso();
    struct { float rt[4]; uint32_t cfg[4]; float ex[4]; uint32_t cf2[4]; } c = { { 2.0f / W, 2.0f / H, 1.0f / S, 0.5f / S }, { (uint32_t)S, (uint32_t)pass, 0, 0 },
                                                                 { gpu_room_obj_k(), 64.0f * S * gpu_depth_slope(), (float)gpu_room_obj_span(), gpu_floor_k() },
                                                                 { (uint32_t)(an > 1 ? an : 1), (uint32_t)aniso_show(), 0, 0 } };
    float k = gpu_depth_k(), k2 = gpu_objsort_k();
    memcpy(&c.cfg[2], &k, 4);
    memcpy(&c.cfg[3], &k2, 4);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, &c, 0, 0);
}

uint64_t hw_prof_flushes, hw_prof_verts, hw_prof_rows, hw_prof_runs, hw_prof_dm[8];

/* Occlusion boxes (2026-11-16, hooks.c low-wall rule): a room's box, as triangles with the room depth scale in z,
   drawn depth-only (no pixel shader, no depth write) inside an occlusion query right after the batch's room depth
   pass. The box is "visible" when any sample is not behind the stored room depth (the pass stores the gap-shifted
   depth, i.e. a little farther: errs to visible). Results come back a frame or more later (oc_read at present).
   hw_occl_hidden(id) = the newest result is recent and says hidden, and no visible result within `hold` presents.
   No result / old result / no depth pass = not hidden. RUGRATS_OCCLSYNC=1 waits for each result (tests: same
   one-frame delay in every run). */
#define OC_IDS 4096   /* hash slots (power of 2) */
#define OC_Q 512
#define OC_BOX 256
#define OC_V (OC_BOX * 72)
static struct { ID3D11Query *q; uint32_t id; int fly; uint32_t tick; } ocq[OC_Q];
static struct { uint32_t id; int first, n, x0, y0, x1, y1; } ocb[OC_BOX];
static HwVert ocv[OC_V];
static int oc_nb, oc_nv, oc_sync = -1;
static uint32_t oc_tick;
static struct { uint32_t key, res, vis; uint8_t have, hid, seen; } oci[OC_IDS];   /* by key (0 = free), linear probing */
static int oc_slot(uint32_t key, int make) {
    uint32_t h = (key * 2654435761u) >> 20;
    int old = -1;
    for (int i = 0; i < 16; i++) {
        const int k = (int)((h + (uint32_t)i) & (OC_IDS - 1));
        if (oci[k].key == key) return k;
        if (!oci[k].key || oc_tick - oci[k].res > 600) { if (old < 0) old = k; if (!oci[k].key) break; }
    }
    if (!make || old < 0) return -1;
    memset(&oci[old], 0, sizeof oci[old]); oci[old].key = key;
    return old;
}
void hw_occl_reset(void) { memset(oci, 0, sizeof oci); for (int k = 0; k < OC_Q; k++) ocq[k].id = 0; }
uint64_t hw_prof_occl, hw_prof_occl_hid, hw_prof_dmv[8];
void hw_occl_add(uint32_t id, const HwVert *v, int n, int cx0, int cy0, int cx1, int cy1) {
    if (!S || !id || n <= 0 || n > 72 || oc_nb >= OC_BOX || oc_nv + n > OC_V) return;
    memcpy(ocv + oc_nv, v, (size_t)n * sizeof *v);
    ocb[oc_nb].id = id; ocb[oc_nb].first = oc_nv; ocb[oc_nb].n = n;
    ocb[oc_nb].x0 = cx0; ocb[oc_nb].y0 = cy0; ocb[oc_nb].x1 = cx1; ocb[oc_nb].y1 = cy1;
    oc_nb++; oc_nv += n;
}
int hw_occl_hidden(uint32_t key, int hold) {
    const int id = S && key ? oc_slot(key, 0) : -1;
    if (id < 0 || !oci[id].have || !oci[id].hid) return 0;
    if (oc_tick - oci[id].res > 6) return 0;
    return !oci[id].seen || oc_tick - oci[id].vis > (uint32_t)hold;
}
/* a result not older than `age` presents says visible: the caller may skip re-testing for a few frames (a box that
   passes the depth test is the costly kind to draw; a hidden one is rejected early) */
int hw_occl_seen(uint32_t key, int age) {
    const int id = S && key ? oc_slot(key, 0) : -1;
    return id >= 0 && oci[id].have && !oci[id].hid && oc_tick - oci[id].res <= (uint32_t)age;
}
static void oc_read(void) {
    if (oc_sync < 0) { const char *e = getenv("RUGRATS_OCCLSYNC"); oc_sync = e ? atoi(e) != 0 : 0; }
    for (int k = 0; k < OC_Q; k++) {
        if (!ocq[k].fly) continue;
        UINT64 n = 0;
        HRESULT hr;
        int spin = 0;
        while ((hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)ocq[k].q, &n, sizeof n, oc_sync ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE &&
               oc_sync && spin++ < 2000000) YieldProcessor();
        if (hr == S_FALSE) { if (oc_tick - ocq[k].tick > 30) ocq[k].fly = 0; continue; }
        ocq[k].fly = 0;
        if (hr != S_OK) continue;
        const int id = ocq[k].id ? oc_slot(ocq[k].id, 1) : -1;
        if (id < 0) continue;
        if (oci[id].have && (int32_t)(ocq[k].tick - oci[id].res) < 0) continue;   /* older than what we have */
        oci[id].have = 1; oci[id].res = ocq[k].tick; oci[id].hid = n == 0;
        if (n) { oci[id].seen = 1; oci[id].vis = ocq[k].tick; }
        hw_prof_occl++; hw_prof_occl_hid += n == 0;
    }
    oc_tick++;
}
/* in hw_flush, after the room depth pass; vertices are at `base` in the vertex buffer */
static void oc_draw(int base) {
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, dsv);
    ID3D11DeviceContext_PSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_test, 0);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    for (int b = 0; b < oc_nb; b++) {
        D3D11_RECT sc = { ocb[b].x0 * S, ocb[b].y0 * S, (ocb[b].x1 + 1) * S, (ocb[b].y1 + 1) * S };
        if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
        int k = -1;
        for (int i = 0; i < OC_Q && k < 0; i++) if (!ocq[i].fly) k = i;
        if (k < 0) break;
        if (!ocq[k].q) {
            D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
            if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &ocq[k].q))) { ocq[k].q = NULL; break; }
        }
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)ocq[k].q);
        ID3D11DeviceContext_Draw(ctx, (UINT)ocb[b].n, (UINT)(base + ocb[b].first));
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)ocq[k].q);
        ocq[k].id = ocb[b].id; ocq[k].tick = oc_tick; ocq[k].fly = 1;
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
}

static void gt_begin(void), gt_end(void);
void hw_flush(void) {
    rd_x0 = 1 << 30; rd_x1 = -1;
    if (!S || !nverts) { oc_nb = oc_nv = 0; return; }
    gt_begin();
    if (dirty_y0 <= dirty_y1) {
        D3D11_BOX box = { 0, (UINT)dirty_y0, 0, 1024, (UINT)dirty_y1 + 1, 1 };
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)vtex, 0, &box, vram + dirty_y0 * 1024, 2048, 0);
        hw_prof_rows += (uint64_t)(dirty_y1 - dirty_y0 + 1);
        dirty_y0 = 1; dirty_y1 = 0;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        nverts = nruns = nsegs = 0;
        return;
    }
    {   /* runs get consecutive vertex ranges; segments keep their add order within a run */
        static int cur[RUN_MAX];
        int off = 0;
        for (int i = 0; i < nruns; i++) { runs[i].first = off; off += runs[i].n; cur[i] = 0; }
        HwVert *dst = (HwVert *)m.pData;
        for (int s = 0; s < nsegs; s++) {
            const int r = segs[s].run;
            memcpy(dst + runs[r].first + cur[r], verts + segs[s].first, (size_t)segs[s].n * sizeof(HwVert));
            cur[r] += segs[s].n;
        }
        if (oc_nv && nverts + oc_nv <= VB_MAX) memcpy(dst + nverts, ocv, (size_t)oc_nv * sizeof(HwVert));
        else oc_nb = oc_nv = 0;
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vb, 0);
    hw_prof_flushes++;
    hw_prof_verts += (uint64_t)nverts;

    UINT stride = sizeof(HwVert), off = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &vsrv);
    tex_bind(ctx);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
    if (rs_w_s != S) {   /* room depth writes: slope bias per hi-res pixel = slope * S native pixels */
        rel((void **)&rs_w);
        D3D11_RASTERIZER_DESC r = { 0 };
        r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.ScissorEnable = TRUE; r.DepthClipEnable = FALSE;
        r.SlopeScaledDepthBias = -gpu_depth_slope() * (float)S;
        if (SUCCEEDED(ID3D11Device_CreateRasterizerState(dev, &r, &rs_w))) rs_w_s = S;
    }
    /* room test: first a depth-only pass of this batch's room pieces and fills in order (room: nearest wins, fills:
       0), then the normal pass tests room pieces like objects (hidden only when more than the gap behind). Same
       rule as the CPU renderer, except a room piece can also be hidden by a nearer one drawn later in the batch
       (which paints over it anyway). Plain depth testing only. */
    const int rtest = gpu_room_test() && dsv && rs_w;
    /* early-Z depth check (see ps_pw in hw_src): only with the room test pass, which then writes the gap-shifted depth */
    const int ez = rtest && early_z() && ps_pw && ps_toe && ps_owe && ps_oce && ps_ome;
    if (rtest) {
        int any = 0;
        for (int i = 0; i < nruns && !any; i++) any = runs[i].dm == HW_DM_WRITE || runs[i].dm == HW_DM_WNT;
        if (any) {
            set_pass(0);
            ID3D11DeviceContext_OMSetBlendState(ctx, bs_none, NULL, 0xFFFFFFFF);
            int pm = -1;
            for (int i = 0; i < nruns; i++) {
                int m = runs[i].dm == HW_DM_WNT ? HW_DM_WRITE : runs[i].dm;
                if (m != HW_DM_WRITE && m != HW_DM_CLEAR) continue;
                D3D11_RECT sc = { runs[i].x0 * S, runs[i].y0 * S, (runs[i].x1 + 1) * S, (runs[i].y1 + 1) * S };
                if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
                if (m != pm) {
                    pm = m;
                    ID3D11DeviceContext_OMSetDepthStencilState(ctx, m == HW_DM_WRITE ? ds_pre : ds_write, 0);
                    ID3D11DeviceContext_RSSetState(ctx, m == HW_DM_WRITE && !ez ? rs_w : rs);
                    if (ez) ID3D11DeviceContext_PSSetShader(ctx, m == HW_DM_WRITE ? ps_pw : ps, NULL, 0);
                }
                ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
                ID3D11DeviceContext_Draw(ctx, (UINT)runs[i].n, (UINT)runs[i].first);
            }
            ID3D11DeviceContext_RSSetState(ctx, rs);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds_off, 0);
            if (ez && oc_nb) oc_draw(nverts);
        }
    }
    oc_nb = oc_nv = 0;   /* boxes without a room depth pass in this batch get no result (= not hidden) */
    int pass = -1, dmode = HW_DM_NONE;
    /* object sort fix (oz, see ps_to): objects test it, TW stores into it, room pieces and fills clear it */
    const int ozon = ortv && osrv && ps_to && ps_ow && ps_oc && ps_och && bs_max;
    const int ro = ozon && ps_rt && ps_rh && ps_r && ps_om && ps_omh && bs_min && gpu_room_obj();   /* room over object */
    if (ozon) ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &osrv);
    for (int i = 0; i < nruns; i++) {
        if (runs[i].dm != dmode) {
            dmode = runs[i].dm;
            int tst = dmode == HW_DM_TEST || dmode == HW_DM_TW || (rtest && dmode == HW_DM_WRITE);   /* gap test, no depth write */
            int wr = !rtest && (dmode == HW_DM_WRITE || dmode == HW_DM_WNT || dmode == HW_DM_CLEAR);  /* (room test: done in the pass above) */
            /* HUD protection: overlays write 1.0; HTEST (and unsure room pieces after the room pass) skip HUD pixels */
            int hud = dmode == HW_DM_HUD, htst = ps_h && (dmode == HW_DM_HTEST || (rtest && dmode == HW_DM_WNT));
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, tst || htst ? ds_test : wr || hud ? ds_write : ds_off, 0);
            const int obj = dmode == HW_DM_TEST || dmode == HW_DM_TW;
            const int room = ro && (dmode == HW_DM_WRITE || (dmode == HW_DM_WNT && gpu_room_obj_wnt()));   /* skips nearer object pixels */
            if (ez)   /* testers without SV_Depth: ps_test -> ps, ps_rt -> ps_r, ps_to -> ps_toe */
                ID3D11DeviceContext_PSSetShader(ctx, htst ? (room ? ps_rh : ps_h) : obj && ozon ? ps_toe : room ? ps_r : ps, NULL, 0);
            else
                ID3D11DeviceContext_PSSetShader(ctx, htst ? (room ? ps_rh : ps_h) : obj && ozon ? ps_to :
                                                     tst && ps_t ? (room ? ps_rt : ps_t) : room ? ps_r : ps, NULL, 0);
            ID3D11DeviceContext_RSSetState(ctx, wr && dmode != HW_DM_CLEAR ? rs_w : rs);
        }
        D3D11_RECT sc = { runs[i].x0 * S, runs[i].y0 * S, (runs[i].x1 + 1) * S, (runs[i].y1 + 1) * S };
        if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sc);
        if (!runs[i].sub) {
            if (pass != 0) { set_pass(0); pass = 0; }
            ID3D11DeviceContext_OMSetBlendState(ctx, bs_dual, NULL, 0xFFFFFFFF);
            ID3D11DeviceContext_Draw(ctx, (UINT)runs[i].n, (UINT)runs[i].first);
        } else {
            set_pass(1); /* opaque texels */
            ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
            ID3D11DeviceContext_Draw(ctx, (UINT)runs[i].n, (UINT)runs[i].first);
            set_pass(2); /* semi texels: back - front */
            ID3D11DeviceContext_OMSetBlendState(ctx, bs_sub, NULL, 0xFFFFFFFF);
            ID3D11DeviceContext_Draw(ctx, (UINT)runs[i].n, (UINT)runs[i].first);
            pass = 2;
        }
        /* oz draw after the colour draw, with the same depth test: TW stores (MAX), room / fills clear (where painted;
           the oz check of ps_to is not repeated: a hidden TW pixel stores less than the oz that hid it) */
        const int m = runs[i].dm;
        if (ozon && !runs[i].sub && (m == HW_DM_TW || m == HW_DM_WRITE || m == HW_DM_WNT || m == HW_DM_CLEAR)) {
            const int rm = ro && (m == HW_DM_WRITE || (m == HW_DM_WNT && gpu_room_obj_wnt()));   /* room mark (MAX) instead of a clear */
            ID3D11PixelShader *zp = m == HW_DM_TW ? (ez ? ps_owe : ps_ow) : (m == HW_DM_WNT && rtest && ps_h) ? (rm ? ps_omh : ps_och) :
                                    rm ? (ez ? ps_ome : ps_om) : ez ? ps_oce : ps_oc;
            const int zt = m == HW_DM_TW || (rtest && (m == HW_DM_WRITE || (m == HW_DM_WNT && ps_h)));
            ID3D11ShaderResourceView *none = NULL;
            ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &none);
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &ortv, dsv);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, zt ? ds_test : ds_off, 0);
            ID3D11DeviceContext_PSSetShader(ctx, zp, NULL, 0);
            ID3D11DeviceContext_OMSetBlendState(ctx, m == HW_DM_TW || rm ? bs_max : NULL, NULL, 0xFFFFFFFF);
            ID3D11DeviceContext_Draw(ctx, (UINT)runs[i].n, (UINT)runs[i].first);
            if (rm && bs_min && gpu_room_obj_span()) {   /* second mark (MIN): object values nearer than the whole polygon */
                set_pass(3); pass = -1;
                ID3D11DeviceContext_OMSetBlendState(ctx, bs_min, NULL, 0xFFFFFFFF);
                ID3D11DeviceContext_Draw(ctx, (UINT)runs[i].n, (UINT)runs[i].first);
            }
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &osrv);
            dmode = -1;   /* states are set again for the next run */
        }
    }
    if (ozon) { ID3D11ShaderResourceView *none = NULL; ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 1, &none); }
    {   /* RUGRATS_RUNLOG=<n>: dump the run list of the first flush with at least n runs (debug) */
        static int at = -1, k;
        if (at < 0) { const char *e = getenv("RUGRATS_RUNLOG"); at = e ? atoi(e) : 0; }
        if (at && nruns >= at && !k++)
            for (int i = 0; i < nruns; i++)
                rt_logf("run %d dm %d sub %d n %d clip %d,%d-%d,%d\n", i, runs[i].dm, runs[i].sub, runs[i].n, runs[i].x0, runs[i].y0, runs[i].x1, runs[i].y1);
    }
    hw_prof_runs += (uint64_t)nruns;
    for (int i = 0; i < nruns; i++) { hw_prof_dm[runs[i].dm & 7]++; hw_prof_dmv[runs[i].dm & 7] += (uint64_t)runs[i].n; }
    nverts = nruns = nsegs = 0;
    gt_end();
}

static void copy_piece(int sx, int sy, int dx, int dy, int w, int h) {
    if (sx + w > 1024) { int a = 1024 - sx; copy_piece(sx, sy, dx, dy, a, h); copy_piece(0, sy, (dx + a) & 1023, dy, w - a, h); return; }
    if (dx + w > 1024) { int a = 1024 - dx; copy_piece(sx, sy, dx, dy, a, h); copy_piece((sx + a) & 1023, sy, 0, dy, w - a, h); return; }
    if (sy + h > 512) { int a = 512 - sy; copy_piece(sx, sy, dx, dy, w, a); copy_piece(sx, 0, dx, (dy + a) & 511, w, h - a); return; }
    if (dy + h > 512) { int a = 512 - dy; copy_piece(sx, sy, dx, dy, w, a); copy_piece(sx, (sy + a) & 511, dx, 0, w, h - a); return; }
    int pw = w * S, ph = h * S;
    if (NS > 1) {   /* multi-sample: rt -> MS scratch -> rt, sample by sample (ps_cpy) */
        if (!scr_ms || scr_w < pw || scr_h < ph) {
            rel((void **)&scr_rtv); rel((void **)&scr_srv); rel((void **)&scr_ms);
            int nw = pw > scr_w ? pw : scr_w, nh = ph > scr_h ? ph : scr_h;
            scr_w = scr_h = 0;
            D3D11_TEXTURE2D_DESC t = { 0 };
            t.Width = nw; t.Height = nh; t.MipLevels = 1; t.ArraySize = 1;
            t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            t.SampleDesc.Count = (UINT)NS;
            t.Usage = D3D11_USAGE_DEFAULT;
            t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &scr_ms))) return;
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)scr_ms, NULL, &scr_rtv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)scr_ms, NULL, &scr_srv);
            if (!scr_rtv || !scr_srv) return;
            scr_w = nw; scr_h = nh;
        }
        ms_draw(ps_cpy, rsrv, scr_rtv, 0, 0, pw, ph, sx * S, sy * S);
        ms_draw(ps_cpy, scr_srv, rtv, dx * S, dy * S, pw, ph, -dx * S, -dy * S);
        return;
    }
    if (!scratch || scratch_w < pw || scratch_h < ph) {
        rel((void **)&scratch);
        int nw = pw > scratch_w ? pw : scratch_w, nh = ph > scratch_h ? ph : scratch_h;
        D3D11_TEXTURE2D_DESC t = { 0 };
        t.Width = nw; t.Height = nh; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &scratch))) { scratch_w = scratch_h = 0; return; }
        scratch_w = nw; scratch_h = nh;
    }
    D3D11_BOX src = { (UINT)(sx * S), (UINT)(sy * S), 0, (UINT)(sx * S + pw), (UINT)(sy * S + ph), 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)scratch, 0, 0, 0, 0, (ID3D11Resource *)rt, 0, &src);
    D3D11_BOX back = { 0, 0, 0, (UINT)pw, (UINT)ph, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)rt, 0, (UINT)(dx * S), (UINT)(dy * S), 0,
                                              (ID3D11Resource *)scratch, 0, &back);
}

/* VRAM -> VRAM copy of the detailed pixels (mask bits are not checked here) */
void hw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!S) return;
    hw_flush();
    copy_piece(sx, sy, dx, dy, w, h);
    hw_mark(dy, dy + h - 1);
}

const uint16_t *hw_readback(void) {
    if (!S) return NULL;
    hw_flush();
    if (!staging) {
        D3D11_TEXTURE2D_DESC t = { 0 };
        t.Width = W; t.Height = H; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_STAGING;
        t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &staging))) return NULL;
    }
    if (!rb_buf) rb_buf = malloc((size_t)W * H * 2);
    if (!rb_buf) return NULL;
    resolve(0, 0, W, H);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)(NS > 1 ? rt1 : rt));
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) return NULL;
    for (int y = 0; y < H; y++) {
        const uint8_t *s = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        uint16_t *d = rb_buf + (size_t)y * W;
        for (int x = 0; x < W; x++, s += 4) {
            unsigned r = (s[0] * 31u + 127) / 255, g = (s[1] * 31u + 127) / 255, b = (s[2] * 31u + 127) / 255;
            d[x] = (uint16_t)(r | g << 5 | b << 10 | (s[3] >= 128 ? 0x8000 : 0));
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    return rb_buf;
}

/* native rect x0..x1, y0..y1 of the hi-res target as native 15-bit pixels (the top-left hi-res pixel of each
   S x S block = the native pixel's centre rule), into out (row pitch 1024, indexed like native VRAM). Draws all
   batched work first and waits for the GPU (used rarely: save states, scale changes, first read of a GPU-only area). */
int hw_read_native(int x0, int y0, int x1, int y1, uint16_t *out) {
    if (!S || x0 > x1 || y0 > y1) return 0;
    hw_flush();
    if (!staging) {
        D3D11_TEXTURE2D_DESC t = { 0 };
        t.Width = W; t.Height = H; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_STAGING;
        t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &staging))) return 0;
    }
    D3D11_BOX box = { (UINT)(x0 * S), (UINT)(y0 * S), 0, (UINT)((x1 + 1) * S), (UINT)((y1 + 1) * S), 1 };
    resolve(x0 * S, y0 * S, (x1 - x0 + 1) * S, (y1 - y0 + 1) * S);
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)staging, 0, x0 * S, y0 * S, 0,
                                              (ID3D11Resource *)(NS > 1 ? rt1 : rt), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) return 0;
    for (int y = y0; y <= y1; y++) {
        const uint8_t *row = (const uint8_t *)m.pData + (size_t)(y * S) * m.RowPitch;
        for (int x = x0; x <= x1; x++) {
            const uint8_t *s = row + (size_t)(x * S) * 4;
            unsigned r = (s[0] * 31u + 127) / 255, g = (s[1] * 31u + 127) / 255, b = (s[2] * 31u + 127) / 255;
            out[y * 1024 + x] = (uint16_t)(r | g << 5 | b << 10 | (s[3] >= 128 ? 0x8000 : 0));
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    return 1;
}

/* GPU frame time for the frame-rate limiter (host_win32.c auto_rate, 2026-11-16): a pair of timestamp queries around
   every batch (hw_flush) and the MSAA resolve of a frame, summed; read back a few frames later without waiting (a
   frame whose slot is still in flight is not measured). Gaps between batches (the GPU idle while the CPU runs the
   game) and the wait for the display are not counted. hw_gpu_ms() = moving average in ms, 0 until known. */
#define GT_N 4
#define GT_P 16
static struct { ID3D11Query *dj, *t[2 * GT_P]; int np, state; } gt[GT_N];   /* state 0 free, 1 open, 2 in flight */
static int gt_i, gt_ok = -1, gt_pair;   /* gt_pair: a pair is open */
static double gt_avg;
double hw_gpu_ms(void) { return S ? gt_avg : 0; }
static void gt_init(void) {
    gt_ok = 1;
    for (int k = 0; k < GT_N && gt_ok; k++) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &gt[k].dj))) gt_ok = 0;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (int j = 0; j < 2 * GT_P && gt_ok; j++) if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &gt[k].t[j]))) gt_ok = 0;
    }
    if (!gt_ok) rt_logf("hwgpu: no timestamp queries (frame-rate limiter uses CPU time only)\n");
}
static void gt_read(void) {
    for (int k = 0; k < GT_N; k++) {
        if (gt[k].state != 2) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)gt[k].dj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 sum = 0;
        int ok = 1;
        for (int j = 0; j < gt[k].np && ok; j++) {
            UINT64 a, b;
            if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)gt[k].t[2 * j], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)gt[k].t[2 * j + 1], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) ok = 0;
            else if (b > a) sum += b - a;
        }
        if (!ok) continue;
        gt[k].state = 0;
        if (dj.Disjoint || !dj.Frequency) continue;
        const double ms = (double)sum * 1000.0 / (double)dj.Frequency;
        gt_avg = gt_avg > 0 ? gt_avg * 0.9 + ms * 0.1 : ms;
    }
}
static void gt_begin(void) {
    if (gt_ok < 0) gt_init();
    if (!gt_ok || gt_pair) return;
    if (gt[gt_i].state == 0) { gt[gt_i].state = 1; gt[gt_i].np = 0; ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)gt[gt_i].dj); }
    if (gt[gt_i].state != 1 || gt[gt_i].np >= GT_P) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gt[gt_i].t[2 * gt[gt_i].np]);
    gt_pair = 1;
}
static void gt_end(void) {
    if (!gt_pair) return;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gt[gt_i].t[2 * gt[gt_i].np + 1]);
    gt[gt_i].np++;
    gt_pair = 0;
}
static void gt_frame_start(void) { if (gt_ok > 0) gt_read(); }
static void gt_frame_end(void) {
    if (gt_ok <= 0) return;
    gt_end();
    if (gt[gt_i].state == 1) {
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)gt[gt_i].dj);
        gt[gt_i].state = 2;
        gt_i = (gt_i + 1) % GT_N;
    }
}

void hw_present(int sx, int sy, int w, int h, int dx, int dy, int dw, int dh, int smooth, int sync, int tear) {
    if (!S) return;
    gt_frame_start();
    oc_read();
    hw_flush();
    if (NS > 1) {   /* resolve only the shown region (wraps at the VRAM edge: clamp, like the view) */
        int rx = sx * S - 2, ry = sy * S - 2, rw = w * S + 4, rh = h * S + 4;   /* + 2 px: filters read the border */
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > W) rw = W - rx;
        if (ry + rh > H) rh = H - ry;
        gt_begin();
        resolve(rx, ry, rw, rh);
        gt_end();
    }
    gt_frame_end();   /* before the present: a flip waiting for the display must not count as GPU work */
    d3d_present_view(NS > 1 ? rsrv1 : rsrv, W, H, sx * S, sy * S, w * S, h * S, dx, dy, dw, dh, smooth, sync, tear);
    /* RUGRATS_GPUSYNC=1 (debug): wait until the GPU has finished the frame, so RUGRATS_PROF frame times include
       the GPU's drawing (with RUGRATS_NOPRESENT the work is otherwise only queued) */
    static int gsync = -1;
    static ID3D11Query *q;
    if (gsync < 0) {
        gsync = getenv("RUGRATS_GPUSYNC") != NULL;
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        if (gsync) ID3D11Device_CreateQuery(dev, &qd, &q);
    }
    if (gsync && q) {
        static uint64_t n, spins;
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q);
        BOOL done = FALSE;
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q, &done, sizeof done, 0) == S_FALSE) { YieldProcessor(); spins++; }
        if (++n % 1000 == 0) {
            rt_logf("hwgpu: %llu presents, %.1f flushes and %.0f vertices per present, %.0f wait spins\n", (unsigned long long)n,
                    hw_prof_flushes / 1000.0, hw_prof_verts / 1000.0, spins / 1000.0);
            rt_logf("hwgpu: %.0f triangle tests per present\n", hw_prof_tests / 1000.0);
            hw_prof_tests = 0;
            rt_logf("hwgpu: runs per present %.0f (none %.0f write %.0f test %.0f clear %.0f wnt %.0f hud %.0f htest %.0f tw %.0f)\n",
                    hw_prof_runs / 1000.0, hw_prof_dm[0] / 1000.0, hw_prof_dm[1] / 1000.0, hw_prof_dm[2] / 1000.0, hw_prof_dm[3] / 1000.0,
                    hw_prof_dm[4] / 1000.0, hw_prof_dm[5] / 1000.0, hw_prof_dm[6] / 1000.0, hw_prof_dm[7] / 1000.0);
            rt_logf("hwgpu: occlusion boxes per present %.1f, hidden %.1f\n", hw_prof_occl / 1000.0, hw_prof_occl_hid / 1000.0);
            hw_prof_occl = hw_prof_occl_hid = 0;
            rt_logf("hwgpu: vertices per present: none %.0f write %.0f test %.0f clear %.0f wnt %.0f hud %.0f htest %.0f tw %.0f\n", hw_prof_dmv[0] / 1000.0, hw_prof_dmv[1] / 1000.0,
                    hw_prof_dmv[2] / 1000.0, hw_prof_dmv[3] / 1000.0, hw_prof_dmv[4] / 1000.0, hw_prof_dmv[5] / 1000.0, hw_prof_dmv[6] / 1000.0, hw_prof_dmv[7] / 1000.0);
            memset(hw_prof_dmv, 0, sizeof hw_prof_dmv);
            hw_prof_flushes = hw_prof_verts = spins = hw_prof_runs = 0;
            memset(hw_prof_dm, 0, sizeof hw_prof_dm);
        }
    }
}
