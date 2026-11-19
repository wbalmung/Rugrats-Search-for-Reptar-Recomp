/* D3D11 presentation: flip-model swap chain, one textured triangle, optional vsync / tearing (VRR).
   The host hands over a 32-bit 0x00RRGGBB image and the rectangle to draw it in. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "present_d3d11.h"

void rt_logf(const char *fmt, ...);

static HWND hwnd;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain1 *sc;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex;
static ID3D11ShaderResourceView *srv;
static ID3D11VertexShader *vs, *vs_reg;
static ID3D11PixelShader *ps_rgba;
static ID3D11Buffer *reg_cb;
static pD3DCompile d3d_comp;
static ID3D11PixelShader *ps, *ps16;
static ID3D11Texture2D *tex16;
static ID3D11ShaderResourceView *srv16;
static int t16_w, t16_h, has16;
static ID3D11SamplerState *samp[2];
static int tex_w, tex_h, sc_w, sc_h, ok, tearing;
static UINT sc_flags;
/* anti-aliasing (aa_present): area downscale, FXAA into an intermediate target of the source size */
static ID3D11PixelShader *ps_area, *ps_fxaa;
static ID3D11Buffer *aa_cb;
static ID3D11Texture2D *fx_tex;
static ID3D11RenderTargetView *fx_rtv;
static ID3D11ShaderResourceView *fx_srv;
static int fx_w, fx_h;

static const char shader_src[] =
    "struct V { float4 p : SV_Position; float2 t : TEXCOORD0; };\n"
    "V vs_main(uint id : SV_VertexID) {\n"
    "  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.t = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "Texture2D tx : register(t0); SamplerState ss : register(s0);\n"
    "float4 ps_main(V i) : SV_Target { return float4(tx.Sample(ss, i.t).rgb, 1); }\n"
    /* PS1 15-bit pixels uploaded as B5G5R5A1: red/blue land swapped; scaled like the CPU path (v << 3) */
    "float4 ps16_main(V i) : SV_Target { return float4(tx.Sample(ss, i.t).bgr * (248.0 / 255.0), 1); }\n"
    /* hardware renderer target: a region (cbuffer: uv origin, uv size) of the hi-res VRAM, 5-bit values / 31 */
    "cbuffer R : register(b0) { float4 reg; };\n"
    "V vs_reg(uint id : SV_VertexID) {\n"
    "  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
    "  o.t = reg.xy + t * reg.zw; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps_rgba(V i) : SV_Target { return float4(tx.Sample(ss, i.t).rgb * (248.0 / 255.0), 1); }\n"
    /* anti-aliasing (aa_present). sa = source region in texels (x, y, w, h); sb = destination size (w, h) and texture
       size; sm.x = smooth (upscaled axes), sm.y = colour conversion of the plain path (0 rgb, 1 bgr * 248/255,
       2 rgb * 248/255) */
    "cbuffer A : register(b1) { float4 sa; float4 sb; uint4 sm; };\n"
    "float3 cv(float3 c) { return sm.y == 1u ? c.bgr * (248.0 / 255.0) : sm.y == 2u ? c * (248.0 / 255.0) : c; }\n"
    "float3 ld(int x, int y) { return cv(tx.Load(int3(int2(sa.xy) + int2(clamp(x, 0, int(sa.z) - 1), clamp(y, 0, int(sa.w) - 1)), 0)).rgb); }\n"
    /* one axis: s = source position of the pixel centre, r = source texels per screen pixel. r > 1: area (box)
       filter over the pixel's footprint; else nearest (sharp) or bilinear (smooth), like the plain path */
    "void rng(float s, float r, out int k0, out int k1) {\n"
    "  if (r > 1.0) { k0 = int(floor(s - 0.5 * r)); k1 = int(ceil(s + 0.5 * r)) - 1; }\n"
    "  else if (sm.x == 0u) { k0 = k1 = int(floor(s)); }\n"
    "  else { k0 = int(floor(s - 0.5)); k1 = k0 + 1; }\n"
    "  k1 = min(k1, k0 + 31); }\n"
    "float wt(int k, float s, float r) {\n"
    "  if (r > 1.0) return max(0.0, min(float(k) + 1.0, s + 0.5 * r) - max(float(k), s - 0.5 * r));\n"
    "  if (sm.x == 0u) return 1.0;\n"
    "  return max(0.0, 1.0 - abs(float(k) + 0.5 - s)); }\n"
    /* averaged in linear light (gamma 2.2), so thin bright/dark edges keep their weight */
    "float4 ps_area(V i) : SV_Target {\n"
    "  float2 r = sa.zw / sb.xy, s = i.t * sa.zw;\n"
    "  int x0, x1, y0, y1; rng(s.x, r.x, x0, x1); rng(s.y, r.y, y0, y1);\n"
    "  float3 acc = 0; float tw = 0;\n"
    "  [loop] for (int y = y0; y <= y1; y++) { float wy = wt(y, s.y, r.y);\n"
    "    [loop] for (int x = x0; x <= x1; x++) { float w = wy * wt(x, s.x, r.x);\n"
    "      acc += pow(max(ld(x, y), 0.0), 2.2) * w; tw += w; } }\n"
    "  return float4(pow(max(acc / max(tw, 1e-6), 0.0), 1.0 / 2.2), 1); }\n"
    /* FXAA (own implementation of the usual luma edge search) at source resolution into an intermediate
       target; p = source texel coordinates (continuous), clamped inside the region */
    "float3 fs(float2 p) { return cv(tx.SampleLevel(ss, (sa.xy + clamp(p, 0.5, sa.zw - 0.5)) / sb.zw, 0).rgb); }\n"
    "float lu(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
    "float fl(float2 p) { return lu(fs(p)); }\n"
    "static const float FQ[12] = { 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0 };\n"
    "float4 ps_fxaa(V i) : SV_Target {\n"
    "  float2 p = i.p.xy;\n"
    "  float3 cM = fs(p); float lM = lu(cM);\n"
    "  float lN = fl(p + float2(0, -1)), lS = fl(p + float2(0, 1)), lE = fl(p + float2(1, 0)), lW = fl(p + float2(-1, 0));\n"
    "  float mx = max(lM, max(max(lN, lS), max(lE, lW))), mn = min(lM, min(min(lN, lS), min(lE, lW))), rg = mx - mn;\n"
    "  if (rg < max(0.0312, mx * 0.125)) return float4(cM, 1);\n"
    "  float lNW = fl(p + float2(-1, -1)), lNE = fl(p + float2(1, -1)), lSW = fl(p + float2(-1, 1)), lSE = fl(p + float2(1, 1));\n"
    "  float lNS = lN + lS, lWE = lW + lE;\n"
    "  float eH = abs(-2 * lW + lNW + lSW) + 2 * abs(-2 * lM + lNS) + abs(-2 * lE + lNE + lSE);\n"
    "  float eV = abs(-2 * lN + lNW + lNE) + 2 * abs(-2 * lM + lWE) + abs(-2 * lS + lSW + lSE);\n"
    "  bool hor = eH >= eV;\n"   /* horizontal edge: blend across it vertically */
    "  float l1 = hor ? lN : lW, l2 = hor ? lS : lE, g1 = l1 - lM, g2 = l2 - lM;\n"
    "  bool st1 = abs(g1) >= abs(g2); float gs = 0.25 * max(abs(g1), abs(g2));\n"
    "  float stp = st1 ? -1.0 : 1.0, la = st1 ? 0.5 * (l1 + lM) : 0.5 * (l2 + lM);\n"
    "  float2 c = p, off = hor ? float2(1, 0) : float2(0, 1);\n"
    "  if (hor) c.y += 0.5 * stp; else c.x += 0.5 * stp;\n"
    "  float2 u1 = c - off, u2 = c + off;\n"
    "  float e1 = fl(u1) - la, e2 = fl(u2) - la; bool d1 = abs(e1) >= gs, d2 = abs(e2) >= gs;\n"
    "  [loop] for (int k = 0; k < 12 && !(d1 && d2); k++) {\n"
    "    if (!d1) { u1 -= off * FQ[k]; e1 = fl(u1) - la; d1 = abs(e1) >= gs; }\n"
    "    if (!d2) { u2 += off * FQ[k]; e2 = fl(u2) - la; d2 = abs(e2) >= gs; } }\n"
    "  float t1 = hor ? p.x - u1.x : p.y - u1.y, t2 = hor ? u2.x - p.x : u2.y - p.y;\n"
    "  bool n1 = t1 < t2; float dm = min(t1, t2), len = t1 + t2;\n"
    "  float po = ((n1 ? e1 : e2) < 0) != (lM < la) ? 0.5 - dm / len : 0.0;\n"
    "  float lA = (2 * (lNS + lWE) + lNW + lNE + lSW + lSE) / 12.0, sp = saturate(abs(lA - lM) / rg);\n"
    "  float sp2 = (-2 * sp + 3) * sp * sp; po = max(po, sp2 * sp2 * 0.5);\n"   /* sub-pixel part 0.5: keeps textures crisper than 0.75 */
    "  if (hor) p.y += po * stp; else p.x += po * stp;\n"
    "  return float4(fs(p), 1); }\n";

static int compile(pD3DCompile comp, const char *entry, const char *target, ID3DBlob **out) {
    ID3DBlob *err = NULL;
    HRESULT hr = comp(shader_src, sizeof shader_src - 1, "present", NULL, NULL, entry, target, 0, 0, out, &err);
    if (err) {
        rt_logf("d3d11: shader %s: %s\n", entry, (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    return SUCCEEDED(hr);
}

static void make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
        ID3D11Texture2D_Release(bb);
    }
}

/* frames DXGI may queue ahead of the display (1..3); RUGRATS_LATENCY overrides (test) */
static int present_latency(void) {
    static int l = 0;
    if (!l) { const char *e = getenv("RUGRATS_LATENCY"); l = e ? atoi(e) : 1; if (l < 1) l = 1; if (l > 3) l = 3; }
    return l;
}

int d3d_init(HWND w) {
    hwnd = w;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    pD3DCompile comp = dc ? (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!comp) { rt_logf("d3d11: d3dcompiler_47.dll missing, using GDI\n"); return 0; }

    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 3,
                                   D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 3,
                               D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { rt_logf("d3d11: no device (%08lx), using GDI\n", hr); return 0; }

    IDXGIDevice1 *xd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *fac = NULL;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&xd);
    if (xd) {
        IDXGIDevice1_SetMaximumFrameLatency(xd, (UINT)present_latency()); /* 1: Present waits for the previous frame */
        IDXGIDevice1_GetAdapter(xd, &ad);
    }
    if (ad) IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac);
    if (!fac) { rt_logf("d3d11: no DXGI 1.2 factory, using GDI\n"); return 0; }
    {
        IDXGIFactory5 *f5 = NULL;
        BOOL t = FALSE;
        if (SUCCEEDED(IDXGIFactory2_QueryInterface(fac, &IID_IDXGIFactory5, (void **)&f5))) {
            if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t))) t = FALSE;
            IDXGIFactory5_Release(f5);
        }
        tearing = t ? 1 : 0;
    }
    sc_flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    DXGI_SWAP_CHAIN_DESC1 d = { 0 };
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = (UINT)present_latency() + 1;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Flags = sc_flags;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)dev, hwnd, &d, NULL, NULL, &sc);
    if (FAILED(hr)) { /* pre-Win10 */
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.Flags = sc_flags = 0;
        tearing = 0;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)dev, hwnd, &d, NULL, NULL, &sc);
    }
    if (FAILED(hr)) { rt_logf("d3d11: no swap chain (%08lx), using GDI\n", hr); return 0; }
    IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER); /* host handles Alt+Enter */
    IDXGIFactory2_Release(fac);
    IDXGIAdapter_Release(ad);
    IDXGIDevice1_Release(xd);

    ID3DBlob *bv = NULL, *bp = NULL;
    if (!compile(comp, "vs_main", "vs_4_0", &bv) || !compile(comp, "ps_main", "ps_4_0", &bp)) return 0;
    d3d_comp = comp;
    {
        ID3DBlob *a = NULL, *b = NULL;
        if (compile(comp, "vs_reg", "vs_4_0", &a) && compile(comp, "ps_rgba", "ps_4_0", &b)) {
            ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(a), ID3D10Blob_GetBufferSize(a), NULL, &vs_reg);
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps_rgba);
            D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            ID3D11Device_CreateBuffer(dev, &bd, NULL, &reg_cb);
        }
        if (a) ID3D10Blob_Release(a);
        if (b) ID3D10Blob_Release(b);
    }
    {
        ID3DBlob *b16 = NULL;
        UINT fs = 0;
        if (SUCCEEDED(ID3D11Device_CheckFormatSupport(dev, DXGI_FORMAT_B5G5R5A1_UNORM, &fs)) &&
            (fs & D3D11_FORMAT_SUPPORT_TEXTURE2D) && (fs & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) &&
            compile(comp, "ps16_main", "ps_4_0", &b16)) {
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b16), ID3D10Blob_GetBufferSize(b16), NULL,
                                           &ps16);
            ID3D10Blob_Release(b16);
            has16 = ps16 != NULL;
        }
    }
    {   /* anti-aliasing shaders; missing = Anti-aliasing works as Off */
        ID3DBlob *a = NULL, *b = NULL;
        if (compile(comp, "ps_area", "ps_4_0", &a) && compile(comp, "ps_fxaa", "ps_4_0", &b)) {
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(a), ID3D10Blob_GetBufferSize(a), NULL, &ps_area);
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps_fxaa);
            D3D11_BUFFER_DESC bd = { 48, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            ID3D11Device_CreateBuffer(dev, &bd, NULL, &aa_cb);
        }
        if (a) ID3D10Blob_Release(a);
        if (b) ID3D10Blob_Release(b);
        if (!ps_area || !ps_fxaa || !aa_cb) rt_logf("d3d11: anti-aliasing shaders missing, anti-aliasing off\n");
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(bv), ID3D10Blob_GetBufferSize(bv), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(bp), ID3D10Blob_GetBufferSize(bp), NULL, &ps);
    ID3D10Blob_Release(bv);
    ID3D10Blob_Release(bp);
    for (int i = 0; i < 2; i++) {
        D3D11_SAMPLER_DESC s = { 0 };
        s.Filter = i ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(dev, &s, &samp[i]);
    }
    if (!vs || !ps || !samp[0] || !samp[1]) return 0;
    make_rtv();
    RECT r;
    GetClientRect(hwnd, &r);
    sc_w = r.right; sc_h = r.bottom;
    ok = rtv != NULL;
    rt_logf("d3d11: ok, flip %s, tearing %s\n", d.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "discard" : "sequential",
            tearing ? "supported" : "no");
    return ok;
}

int d3d_ok(void) { return ok; }
ID3D11Device *d3d_device(void) { return ok ? dev : NULL; }
ID3D11DeviceContext *d3d_context(void) { return ok ? ctx : NULL; }
void *d3d_compiler(void) { return (void *)d3d_comp; }
int d3d_has16(void) { return ok && has16; }
int d3d_tearing(void) { return tearing; }

static int upload(const uint32_t *px, int w, int h) {
    if (!tex || tex_w != w || tex_h != h) {
        if (srv) { ID3D11ShaderResourceView_Release(srv); srv = NULL; }
        if (tex) { ID3D11Texture2D_Release(tex); tex = NULL; }
        D3D11_TEXTURE2D_DESC t = { 0 };
        t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DYNAMIC;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        t.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &tex))) return 0;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv);
        tex_w = w; tex_h = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    for (int y = 0; y < h; y++) memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, px + (size_t)y * w, (size_t)w * 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)tex, 0);
    return 1;
}

static int begin_frame(void) {
    RECT r;
    GetClientRect(hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return 0; /* minimized */
    if (r.right != sc_w || r.bottom != sc_h) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
        IDXGISwapChain1_ResizeBuffers(sc, 0, r.right, r.bottom, DXGI_FORMAT_UNKNOWN, sc_flags);
        make_rtv();
        sc_w = r.right; sc_h = r.bottom;
    }
    return rtv != NULL;
}

/* overlays: small 32-bit images drawn over every presented frame while on.
   Slot 0 = message box (top-left), slot 1 = FPS counter (top-right). */
#define OSD_SLOTS 2
static struct { ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; int w, h, on; } osd[OSD_SLOTS];
void d3d_overlay(int slot, const uint32_t *px, int w, int h) {
    if (!ok || slot < 0 || slot >= OSD_SLOTS) return;
    if (!px || w <= 0 || h <= 0) { osd[slot].on = 0; return; }
    if (!osd[slot].tex || osd[slot].w != w || osd[slot].h != h) {
        if (osd[slot].srv) { ID3D11ShaderResourceView_Release(osd[slot].srv); osd[slot].srv = NULL; }
        if (osd[slot].tex) { ID3D11Texture2D_Release(osd[slot].tex); osd[slot].tex = NULL; }
        D3D11_TEXTURE2D_DESC t = { 0 };
        t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &osd[slot].tex))) { osd[slot].on = 0; return; }
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)osd[slot].tex, NULL, &osd[slot].srv);
        osd[slot].w = w; osd[slot].h = h;
    }
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)osd[slot].tex, 0, NULL, px, (UINT)w * 4, 0);
    osd[slot].on = 1;
}
void d3d_osd(const uint32_t *px, int w, int h) { d3d_overlay(0, px, w, h); }

static void draw_present(ID3D11VertexShader *vshader, ID3D11PixelShader *shader, ID3D11ShaderResourceView *view, int dx, int dy, int dw, int dh,
                         int smooth, int sync, int tear) {
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    D3D11_VIEWPORT vp = { (float)dx, (float)dy, (float)dw, (float)dh, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_RSSetState(ctx, NULL); /* the hardware renderer leaves scissor / blend state behind */
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_VSSetShader(ctx, vshader, NULL, 0);
    if (vshader == vs_reg) ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &reg_cb);
    ID3D11DeviceContext_PSSetShader(ctx, shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &view);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &samp[(smooth & 1) ? 1 : 0]);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    for (int i = 0; i < OSD_SLOTS; i++) {
        if (!osd[i].on || !osd[i].srv) continue;
        int margin = sc_h / 60 > 16 ? sc_h / 60 : 16;
        float x = i == 1 ? (float)(sc_w - osd[i].w - margin) : (float)margin; /* slot 1 top-right, others top-left */
        D3D11_VIEWPORT ov = { x, (float)margin, (float)osd[i].w, (float)osd[i].h, 0, 1 };
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &ov);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &osd[i].srv);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &samp[0]);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
    }
    static int nopresent = -1; /* RUGRATS_NOPRESENT=1 (debug): skip Present to measure the CPU-side frame limit */
    if (nopresent < 0) nopresent = getenv("RUGRATS_NOPRESENT") != NULL;
    if (nopresent) { ID3D11DeviceContext_Flush(ctx); return; }
    UINT flags = (tear && tearing && !sync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = IDXGISwapChain1_Present(sc, (UINT)sync, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        rt_logf("d3d11: device lost (%08lx), falling back to GDI\n", hr);
        ok = 0;
    }
}

/* Anti-aliasing (smooth bits 1-2 = D3D_AA mode): the (sx, sy, w, h) region of a tw x th view, drawn into
   (dx, dy, dw, dh); conv = the plain path's colour conversion (see cv in the shader). Returns 0 when the plain
   path should draw (mode Off, or Downsample with nothing to shrink): that path stays exactly as before.
   1 Downsample: an axis the window shrinks gets an area (box) filter over every source pixel in the screen pixel
   (supersampling from a high internal resolution); an axis it grows keeps Sharp / Smooth.
   2 Downsample + FXAA: FXAA at source resolution first (intermediate target), then the same scaling. */
static int aa_present(ID3D11ShaderResourceView *view, int conv, int tw, int th, int sx, int sy, int w, int h, int dx, int dy,
                      int dw, int dh, int smooth, int sync, int tear) {
    const int aa = (smooth >> 1) & 3;
    smooth &= 1;
    if (!aa || !ps_area || !ps_fxaa || !aa_cb || dw <= 0 || dh <= 0) return 0;
    const int down = w > dw || h > dh;
    if (aa == 1 && !down) return 0;
    struct { float sa[4], sb[4]; uint32_t sm[4]; } c = { { (float)sx, (float)sy, (float)w, (float)h }, { 0 }, { (uint32_t)smooth, (uint32_t)conv, 0, 0 } };
    ID3D11ShaderResourceView *none = NULL;
    if (aa >= 2) {
        if (!fx_tex || fx_w != w || fx_h != h) {
            if (fx_srv) { ID3D11ShaderResourceView_Release(fx_srv); fx_srv = NULL; }
            if (fx_rtv) { ID3D11RenderTargetView_Release(fx_rtv); fx_rtv = NULL; }
            if (fx_tex) { ID3D11Texture2D_Release(fx_tex); fx_tex = NULL; }
            fx_w = fx_h = 0;
            D3D11_TEXTURE2D_DESC t = { 0 };
            t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1;
            t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            t.SampleDesc.Count = 1;
            t.Usage = D3D11_USAGE_DEFAULT;
            t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &fx_tex))) return 0;
            ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)fx_tex, NULL, &fx_rtv);
            ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)fx_tex, NULL, &fx_srv);
            if (!fx_rtv || !fx_srv) return 0;
            fx_w = w; fx_h = h;
        }
        c.sb[0] = (float)w; c.sb[1] = (float)h; c.sb[2] = (float)tw; c.sb[3] = (float)th;
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)aa_cb, 0, NULL, &c, 0, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &fx_rtv, NULL);
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_RSSetState(ctx, NULL);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps_fxaa, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &aa_cb);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &view);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &samp[1]);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
        view = fx_srv; conv = 0; tw = w; th = h; sx = sy = 0;
        c.sa[0] = c.sa[1] = 0; c.sm[1] = 0;
        if (!down) { draw_present(vs, ps, fx_srv, dx, dy, dw, dh, smooth, sync, tear); return 1; }
    }
    c.sb[0] = (float)dw; c.sb[1] = (float)dh; c.sb[2] = (float)tw; c.sb[3] = (float)th;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)aa_cb, 0, NULL, &c, 0, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &aa_cb);
    draw_present(vs, ps_area, view, dx, dy, dw, dh, smooth, sync, tear);
    return 1;
}

void d3d_present(const uint32_t *px, int pw, int ph, int dx, int dy, int dw, int dh, int smooth, int sync, int tear) {
    if (!ok || !begin_frame() || pw <= 0 || ph <= 0 || !upload(px, pw, ph)) return;
    if (aa_present(srv, 0, pw, ph, 0, 0, pw, ph, dx, dy, dw, dh, smooth, sync, tear)) return;
    draw_present(vs, ps, srv, dx, dy, dw, dh, smooth, sync, tear);
}

/* 16-bit path: a w x h window at (sx, sy) of a wrapping vw x vh VRAM image goes straight to the GPU */
void d3d_present16(const uint16_t *v, int vw, int vh, int sx, int sy, int w, int h, int dx, int dy, int dw, int dh,
                   int smooth, int sync, int tear) {
    if (!ok || !has16 || !begin_frame() || w <= 0 || h <= 0) return;
    if (!tex16 || t16_w != w || t16_h != h) {
        if (srv16) { ID3D11ShaderResourceView_Release(srv16); srv16 = NULL; }
        if (tex16) { ID3D11Texture2D_Release(tex16); tex16 = NULL; }
        D3D11_TEXTURE2D_DESC t = { 0 };
        t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_B5G5R5A1_UNORM;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DYNAMIC;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        t.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &tex16))) return;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex16, NULL, &srv16);
        t16_w = w; t16_h = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)tex16, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for (int y = 0; y < h; y++) {
        const uint16_t *row = v + (size_t)((sy + y) % vh) * vw;
        uint16_t *d = (uint16_t *)((uint8_t *)m.pData + (size_t)y * m.RowPitch);
        if (sx + w <= vw) memcpy(d, row + sx, (size_t)w * 2);
        else for (int x = 0; x < w; x++) d[x] = row[(sx + x) % vw];
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)tex16, 0);
    if (aa_present(srv16, 1, w, h, 0, 0, w, h, dx, dy, dw, dh, smooth, sync, tear)) return;
    draw_present(vs, ps16, srv16, dx, dy, dw, dh, smooth, sync, tear);
}

/* a w x h region at (sx, sy) of a tw x th texture on this device (the hardware renderer's hi-res VRAM) */
void d3d_present_view(void *view, int tw, int th, int sx, int sy, int w, int h, int dx, int dy, int dw, int dh, int smooth,
                      int sync, int tear) {
    if (!ok || !vs_reg || !ps_rgba || !reg_cb || !begin_frame() || w <= 0 || h <= 0) return;
    if (aa_present((ID3D11ShaderResourceView *)view, 2, tw, th, sx, sy, w, h, dx, dy, dw, dh, smooth, sync, tear)) return;
    float r[4] = { (float)sx / tw, (float)sy / th, (float)w / tw, (float)h / th };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)reg_cb, 0, NULL, r, 0, 0);
    draw_present(vs_reg, ps_rgba, (ID3D11ShaderResourceView *)view, dx, dy, dw, dh, smooth, sync, tear);
}
