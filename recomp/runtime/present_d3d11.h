/* D3D11 presenter (see present_d3d11.c). Falls back to GDI in the host when d3d_ok() is 0. */
#pragma once
#include <windows.h>
#include <stdint.h>

int d3d_init(HWND w);
/* the present calls' `smooth`: bit 0 = bilinear upscaling; D3D_AA(m) adds the anti-aliasing mode (0 off,
   1 downsample, 2 downsample + FXAA; see aa_present) */
#define D3D_AA(m) (((m) & 3) << 1)
int d3d_ok(void);
int d3d_tearing(void);
/* px: pw x ph 0x00RRGGBB, drawn into client rect (dx,dy,dw,dh); sync = vsync interval (0 = none);
   tear = allow tearing when sync is 0 (VRR / lowest latency) */
void d3d_present(const uint32_t *px, int pw, int ph, int dx, int dy, int dw, int dh, int smooth, int sync, int tear);
int d3d_has16(void);
/* PS1 15-bit VRAM window straight to the GPU (no CPU colour conversion); vw x vh wraps */
void d3d_present16(const uint16_t *v, int vw, int vh, int sx, int sy, int w, int h, int dx, int dy, int dw, int dh,
                   int smooth, int sync, int tear);
/* for the hardware renderer (gpu_d3d11.c): the device, context and D3DCompile (NULL without D3D11) */
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Device *d3d_device(void);
struct ID3D11DeviceContext *d3d_context(void);
void *d3d_compiler(void);
/* view: an ID3D11ShaderResourceView of a tw x th RGBA texture; shows its (sx, sy, w, h) region */
void d3d_present_view(void *view, int tw, int th, int sx, int sy, int w, int h, int dx, int dy, int dw, int dh, int smooth,
                      int sync, int tear);
/* on-screen message: w x h 0x00RRGGBB image drawn at the top-left of every frame; px NULL = hide */
void d3d_osd(const uint32_t *px, int w, int h);
/* overlay slot 0 = top-left (messages), 1 = top-right (FPS counter); px NULL = hide */
void d3d_overlay(int slot, const uint32_t *px, int w, int h);
