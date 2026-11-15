/* Texture packs (mod API 3, 2026-11-13). Spec for authors: recomp/MODDING.md "Texture packs".

   A texture is a block of a texture page as draws use it: the smallest power-of-two span, at least 64 and at most
   256 texels, aligned within the page (u and v separately), that holds the draw's uv box (after the texture
   window). Level textures come as one big upload of 64x64 tiles with a palette each; blocks give one small picture
   per tile and palette. Its name is a 64-bit FNV-1a hash of the block: the bytes w(u16) h(u16) followed by the
   w x h 16-bit VRAM words (w = width in words: texels / 4, / 2, / 1 for 4, 8, 15-bit), row by row, little-endian.
   4/8-bit textures also carry the palette: FNV-1a 64 of the 16 / 256 CLUT entries the draw uses. File names:
       <block hash>.png                  any palette (and every 15-bit texture)
       <block hash>-<palette hash>.png   only with that palette (wins over the generic one)
   (16 lowercase hex digits each). The PNG may have any size (up to 4096): it is stretched over the block; alpha
   < 50% = see-through (as texel 0), the PS1's semi-transparency bit still comes from the original texel.
   THE HASH AND THE BLOCK RULE ARE PART OF THE PACK FORMAT: never change them.

   How: when the D3D11 renderer queues a textured triangle (gpu_d3d11.c hw_add -> tex_tag), its block is hashed
   (cached per block; uploads / fills / copies bump a generation counter per 16x16-word cell, which invalidates the
   cache), the pack entry is found (exact palette first), and the vertex gets the record id + block rect. The pixel
   shader maps the texel position inside the block onto the record's rect in a texture atlas (Texture2DArray of
   4096x4096 RGBA8 slices, shelf packed, never evicted) and samples it bilinear. Blocks inside the current drawing
   area are never replaced (render-to-texture). CPU renderer: no replacements. PNGs are decoded with Windows' WIC
   (no image library in the repo) the first time they are drawn.

   Dump mode (F1 Game > "Dump textures", ini [textures] dump, RUGRATS_TEXDUMP=1): each texture drawn with a palette
   not dumped yet is written once to textures_dump\ next to the exe under its pack name (15-bit: <hash>.png;
   4/8-bit: <hash>-<palette>.png, palette applied). Colours are the PS1's 5-bit values x 8 (0..248), so an unedited
   dump used as a pack looks exactly like the original. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hw.h"
#include "gpu_d3d11.h"
#include "present_d3d11.h"
#include "savestate.h"

extern uint16_t vram[512 * 1024];

/* ---- hashing: part of the pack format ---- */
#define FNV0 0xCBF29CE484222325ull
static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    while (n--) { h ^= *p++; h *= 0x100000001B3ull; }
    return h;
}
static uint64_t hash_rect(int x, int y, int w, int h) {
    uint8_t d[4] = { (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8) };
    uint64_t k = fnv(FNV0, d, 4);
    for (int r = 0; r < h; r++) k = fnv(k, (const uint8_t *)(vram + (y + r) * 1024 + x), (size_t)w * 2);
    return k;
}
static uint64_t hash_clut(int cx, int cy, int n) {
    uint64_t k = FNV0;
    for (int i = 0; i < n; i++) { uint16_t c = vram[(cy & 511) * 1024 + ((cx + i) & 1023)]; k = fnv(k, (const uint8_t *)&c, 2); }
    return k;
}

/* ---- pack index ---- */
typedef struct { uint64_t hash, clut; int any, order, rec; char *path; } Entry;   /* rec: 0 not loaded, -1 failed */
static Entry *ent;
static int n_ent, cap_ent, order_n;
static int dump_on;
static char dump_dir[1100];

static int hexval(const char *s, int n, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint64_t)d;
    }
    *out = v;
    return 1;
}

/* a pack folder (mods.c, enabled mods in load order: later folders win). Returns the number of textures. */
int tex_add_folder(const char *dir) {
    char pat[1200];
    snprintf(pat, sizeof pat, "%s\\*.png", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int n = 0, bad = 0;
    order_n++;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *f = fd.cFileName;
        size_t l = strlen(f);
        uint64_t hs, cl = 0;
        int any;
        if (l == 20 && hexval(f, 16, &hs)) any = 1;
        else if (l == 37 && f[16] == '-' && hexval(f, 16, &hs) && hexval(f + 17, 16, &cl)) any = 0;
        else { bad++; continue; }
        if (n_ent == cap_ent) {
            cap_ent = cap_ent ? cap_ent * 2 : 256;
            Entry *ne = realloc(ent, (size_t)cap_ent * sizeof *ent);
            if (!ne) break;
            ent = ne;
        }
        Entry *e = &ent[n_ent++];
        e->hash = hs; e->clut = cl; e->any = any; e->order = order_n; e->rec = 0;
        size_t pl = strlen(dir) + l + 2;
        e->path = malloc(pl);
        if (e->path) snprintf(e->path, pl, "%s\\%s", dir, f);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (bad) rt_logf("textures: %s: %d .png files skipped (names must be <16 hex>.png or <16 hex>-<16 hex>.png)\n", dir, bad);
    return n;
}

static int ent_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    if (x->any != y->any) return x->any - y->any;           /* exact palettes first */
    if (x->clut != y->clut) return x->clut < y->clut ? -1 : 1;
    return y->order - x->order;                             /* later pack first */
}
static int ent_first(uint64_t h) {   /* first entry with this hash, or -1 */
    int lo = 0, hi = n_ent;
    while (lo < hi) { int m = (lo + hi) / 2; if (ent[m].hash < h) lo = m + 1; else hi = m; }
    return lo < n_ent && ent[lo].hash == h ? lo : -1;
}
static Entry *ent_find(uint64_t h, uint64_t clut, int indexed) {
    int i = ent_first(h);
    if (i < 0) return NULL;
    Entry *gen = NULL;
    for (; i < n_ent && ent[i].hash == h; i++) {
        if (ent[i].any) { if (!gen) gen = &ent[i]; }
        else if (indexed && ent[i].clut == clut) return &ent[i];
    }
    return gen;
}

void tex_init(int dump) {
    if (n_ent) {
        qsort(ent, (size_t)n_ent, sizeof *ent, ent_cmp);
        int w = 0;   /* drop the same name from earlier packs (the later pack sorted first) */
        for (int i = 0; i < n_ent; i++) {
            if (w && ent[w - 1].hash == ent[i].hash && ent[w - 1].any == ent[i].any && ent[w - 1].clut == ent[i].clut) {
                free(ent[i].path);
                continue;
            }
            ent[w++] = ent[i];
        }
        n_ent = w;
        rt_logf("textures: %d replacement textures from enabled mods (GPU renderer only)\n", n_ent);
    }
    const char *e = getenv("RUGRATS_TEXDUMP");
    dump_on = e ? atoi(e) != 0 : dump;
    char exe[1024];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(dump_dir, sizeof dump_dir, "%s\\textures_dump", exe);
    if (dump_on) rt_logf("textures: dump mode on -> %s\n", dump_dir);
}
void tex_set_dump(int on) { dump_on = on; if (on) rt_logf("textures: dump mode on -> %s\n", dump_dir); }
int tex_count(void) { return n_ent; }

/* ---- WIC (PNG read / write) ---- */
static IWICImagingFactory *wic;
static int wic_init(void) {
    static int tried;
    if (wic || tried) return wic != NULL;
    tried = 1;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;   /* S_FALSE / RPC_E_CHANGED_MODE: COM is already up on this thread, fine */
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic))) {
        wic = NULL;
        rt_logf("textures: Windows Imaging Component not available, no texture packs\n");
    }
    return wic != NULL;
}
static void widen(const char *s, WCHAR *w, int n) { if (!MultiByteToWideChar(CP_ACP, 0, s, -1, w, n)) w[0] = 0; }

static uint8_t *png_load(const char *path, UINT *w, UINT *h) {
    if (!wic_init()) return NULL;
    WCHAR wp[1200];
    widen(path, wp, 1200);
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICBitmapSource *cv = NULL;
    uint8_t *buf = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(wic, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppRGBA, (IWICBitmapSource *)fr, &cv)) &&
        SUCCEEDED(IWICBitmapSource_GetSize(cv, w, h)) && *w && *h && *w <= 4096 && *h <= 4096 &&
        (buf = malloc((size_t)*w * *h * 4)) != NULL) {
        if (FAILED(IWICBitmapSource_CopyPixels(cv, NULL, *w * 4, *w * *h * 4, buf))) { free(buf); buf = NULL; }
    }
    if (cv) IWICBitmapSource_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    return buf;
}

static int png_save(const char *path, const uint8_t *bgra, UINT w, UINT h) {
    if (!wic_init()) return 0;
    WCHAR wp[1200];
    widen(path, wp, 1200);
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IPropertyBag2 *pb = NULL;
    WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
    int ok = SUCCEEDED(IWICImagingFactory_CreateStream(wic, &st)) &&
             SUCCEEDED(IWICStream_InitializeFromFilename(st, wp, GENERIC_WRITE)) &&
             SUCCEEDED(IWICImagingFactory_CreateEncoder(wic, &GUID_ContainerFormatPng, NULL, &enc)) &&
             SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache)) &&
             SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fe, &pb)) &&
             SUCCEEDED(IWICBitmapFrameEncode_Initialize(fe, pb)) &&
             SUCCEEDED(IWICBitmapFrameEncode_SetSize(fe, w, h)) &&
             SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fe, &f)) && IsEqualGUID(&f, &GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fe, h, w * 4, w * h * 4, (BYTE *)bgra)) &&
             SUCCEEDED(IWICBitmapFrameEncode_Commit(fe)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc));
    if (pb) IPropertyBag2_Release(pb);
    if (fe) IWICBitmapFrameEncode_Release(fe);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    return ok;
}

/* ---- atlas + records (GPU) ---- */
#define ATLAS 4096
#define SLICES_MAX 8
#define REC_MAX 4096
static ID3D11Texture2D *atlas;
static ID3D11ShaderResourceView *atlas_srv;
static int slices;
static struct { int x, y, h; } shelf[SLICES_MAX];   /* open shelf per slice: cursor x, its top, its height */
static float recs[REC_MAX * 2][4];                  /* per record: atlas rect (normalized), {slice, half texel} */
static int n_rec = 1;                               /* 0 = no replacement */
static ID3D11Buffer *rec_buf;
static ID3D11ShaderResourceView *rec_srv;
static ID3D11SamplerState *samp;
static int rec_dirty, atlas_full_logged;

static int atlas_grow(void) {
    ID3D11Device *dev = d3d_device();
    ID3D11DeviceContext *ctx = d3d_context();
    if (!dev || !ctx || slices >= SLICES_MAX) return 0;
    D3D11_TEXTURE2D_DESC t = { 0 };
    t.Width = t.Height = ATLAS; t.MipLevels = 1; t.ArraySize = (UINT)slices + 1;
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *nt = NULL;
    ID3D11ShaderResourceView *ns = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &t, NULL, &nt))) return 0;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)nt, NULL, &ns))) { ID3D11Texture2D_Release(nt); return 0; }
    for (int s = 0; s < slices; s++)
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)nt, (UINT)s, 0, 0, 0, (ID3D11Resource *)atlas, (UINT)s, NULL);
    if (atlas_srv) ID3D11ShaderResourceView_Release(atlas_srv);
    if (atlas) ID3D11Texture2D_Release(atlas);
    atlas = nt; atlas_srv = ns;
    shelf[slices].x = shelf[slices].y = shelf[slices].h = 0;
    slices++;
    if (!samp) {
        D3D11_SAMPLER_DESC sd = { 0 };
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(dev, &sd, &samp);
    }
    if (!rec_buf) {
        D3D11_BUFFER_DESC bd = { sizeof recs, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0 };
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &rec_buf);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = { 0 };
        vd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.NumElements = REC_MAX * 2;
        if (rec_buf) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)rec_buf, &vd, &rec_srv);
    }
    rt_logf("textures: atlas now %d slice%s of %dx%d\n", slices, slices == 1 ? "" : "s", ATLAS, ATLAS);
    return samp && rec_srv;
}
static int atlas_alloc(int w, int h, int *sl, int *x, int *y) {
    w += 2; h += 2;   /* 1 px border: bilinear never reaches a neighbour */
    for (int pass = 0; pass < 2; pass++) {
        for (int s = 0; s < slices; s++) {
            if (shelf[s].x + w <= ATLAS && h <= shelf[s].h) { *sl = s; *x = shelf[s].x + 1; *y = shelf[s].y + 1; shelf[s].x += w; return 1; }
            int ny = shelf[s].y + shelf[s].h;   /* new shelf below the open one */
            if (ny + h <= ATLAS && w <= ATLAS) {
                shelf[s].y = ny; shelf[s].h = h; shelf[s].x = w;
                *sl = s; *x = 1; *y = ny + 1;
                return 1;
            }
        }
        if (pass == 0 && !atlas_grow()) break;
    }
    return 0;
}

static void load_entry(Entry *e) {
    if (e->rec) return;
    e->rec = -1;
    if (n_rec >= REC_MAX || !e->path) return;
    UINT w, h;
    uint8_t *px = png_load(e->path, &w, &h);
    if (!px) { rt_logf("textures: can't read %s (PNG up to 4096 x 4096)\n", e->path); return; }
    int sl, x, y;
    if (!atlas_alloc((int)w, (int)h, &sl, &x, &y)) {
        if (!atlas_full_logged++) rt_logf("textures: atlas full (%d x %d x %d), further textures are left out\n", ATLAS, ATLAS, SLICES_MAX);
        free(px);
        return;
    }
    /* border: repeat the edge pixels, so clamped bilinear samples stay inside this picture */
    uint8_t *b = malloc((size_t)(w + 2) * (h + 2) * 4);
    if (!b) { free(px); return; }
    for (UINT r = 0; r < h + 2; r++) {
        UINT sr = r == 0 ? 0 : r > h ? h - 1 : r - 1;
        uint8_t *d = b + (size_t)r * (w + 2) * 4;
        memcpy(d + 4, px + (size_t)sr * w * 4, (size_t)w * 4);
        memcpy(d, d + 4, 4);
        memcpy(d + (size_t)(w + 1) * 4, d + (size_t)w * 4, 4);
    }
    D3D11_BOX box = { (UINT)x - 1, (UINT)y - 1, 0, (UINT)x + w + 1, (UINT)y + h + 1, 1 };
    ID3D11DeviceContext_UpdateSubresource(d3d_context(), (ID3D11Resource *)atlas, (UINT)sl, &box, b, (w + 2) * 4, 0);
    free(b);
    free(px);
    int r = n_rec++;
    recs[r * 2][0] = (float)x / ATLAS; recs[r * 2][1] = (float)y / ATLAS;
    recs[r * 2][2] = (float)w / ATLAS; recs[r * 2][3] = (float)h / ATLAS;
    recs[r * 2 + 1][0] = (float)sl; recs[r * 2 + 1][1] = 0.5f / ATLAS; recs[r * 2 + 1][2] = 0.5f / ATLAS; recs[r * 2 + 1][3] = 0;
    rec_dirty = 1;
    e->rec = r;
    rt_logf("textures: loaded %s (%ux%u) -> atlas slice %d\n", strrchr(e->path, '\\') ? strrchr(e->path, '\\') + 1 : e->path, w, h, sl);
}

/* gpu_d3d11.c hw_flush: before the draws */
void tex_bind(void *context) {
    ID3D11DeviceContext *ctx = context;
    if (n_rec <= 1 || !atlas_srv || !rec_srv) return;
    if (rec_dirty) { ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)rec_buf, 0, NULL, recs, 0, 0); rec_dirty = 0; }
    ID3D11ShaderResourceView *v[2] = { atlas_srv, rec_srv };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 2, 2, v);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &samp);
}

/* ---- VRAM change tracking: a generation counter per 16x16-word cell, bumped by uploads, fills, copies ---- */
static uint32_t gen[32][64], epoch;
static void bump(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > 1024 || y + h > 512) { epoch++; return; }   /* wraps: everything */
    for (int cy = y >> 4; cy <= (y + h - 1) >> 4; cy++)
        for (int cx = x >> 4; cx <= (x + w - 1) >> 4; cx++) gen[cy][cx]++;
}
static uint64_t gsum(int x, int y, int w, int h) {
    uint64_t s = (uint64_t)epoch << 40;
    for (int cy = y >> 4; cy <= (y + h - 1) >> 4; cy++)
        for (int cx = x >> 4; cx <= (x + w - 1) >> 4; cx++) s += gen[cy][cx];
    return s;
}
void tex_upload(int x, int y, int w, int h) { bump(x, y, w, h); }   /* gpu.c: CPU->VRAM upload written */
void tex_fill(int x, int y, int w, int h) { bump(x, y, w, h); }
void tex_copy(int sx, int sy, int dx, int dy, int w, int h) { (void)sx; (void)sy; bump(dx, dy, w, h); }

/* content hash cache (blocks and palettes): key -> {generation sum, hash} */
#define HC_N 8192
static struct { uint64_t key, g, h; } hc[HC_N];
static uint64_t cached(uint64_t key, int x, int y, int w, int h, int pal) {
    uint64_t g = gsum(x, y, w, h);
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 51) & (HC_N - 1);
    for (int n = 0; n < 8; n++, i = (i + 1) & (HC_N - 1)) {
        if (hc[i].key == key && hc[i].g == g) return hc[i].h;
        if (hc[i].key == key || !hc[i].key) break;
    }
    uint64_t v = pal ? hash_clut(x, y, w) : hash_rect(x, y, w, h);
    hc[i].key = key; hc[i].g = g; hc[i].h = v;
    return v;
}

/* ---- dump ---- */
static uint64_t dumped[16384];   /* open addressing on hash ^ palette; 0 = empty */
static int dump_seen(uint64_t k) {
    if (!k) k = 1;
    for (uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 50), n = 0; n < 16384; i = (i + 1) & 16383, n++) {
        if (dumped[i] == k) return 1;
        if (!dumped[i]) { dumped[i] = k; return 0; }
    }
    return 1;   /* table full: stop dumping */
}
static void dump(int bx, int by, int bw, int bh, uint64_t hash, int depth, int cx, int cy, uint64_t clut) {
    if (dump_seen(hash ^ (depth < 2 ? clut * 31 + 7 : 0))) return;
    int k = depth == 0 ? 4 : depth == 1 ? 2 : 1, w = bw * k, h = bh;
    uint8_t *b = malloc((size_t)w * h * 4);
    if (!b) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint16_t word = vram[(by + y) * 1024 + bx + x / k], t;
            if (depth == 0) t = vram[(cy & 511) * 1024 + ((cx + ((word >> ((x & 3) * 4)) & 15)) & 1023)];
            else if (depth == 1) t = vram[(cy & 511) * 1024 + ((cx + ((word >> ((x & 1) * 8)) & 255)) & 1023)];
            else t = word;
            uint8_t *p = b + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(((t >> 10) & 31) * 8); p[1] = (uint8_t)(((t >> 5) & 31) * 8); p[2] = (uint8_t)((t & 31) * 8);
            p[3] = t ? 255 : 0;
        }
    char path[1300];
    CreateDirectoryA(dump_dir, NULL);
    if (depth < 2) snprintf(path, sizeof path, "%s\\%016llx-%016llx.png", dump_dir, (unsigned long long)hash, (unsigned long long)clut);
    else snprintf(path, sizeof path, "%s\\%016llx.png", dump_dir, (unsigned long long)hash);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        if (png_save(path, b, (UINT)w, (UINT)h)) rt_logf("textures: dumped %s (%dx%d, %d-bit at %d,%d)\n", strrchr(path, '\\') + 1, w, h,
                                                         depth == 0 ? 4 : depth == 1 ? 8 : 15, bx, by);
        else rt_logf("textures: can't write %s\n", path);
    }
    free(b);
}

/* smallest aligned power-of-two span (at least 64 texels, at most the 256 of a page) holding a..b */
static int span(int a, int b, int *start) {
    for (int p = 64; p <= 256; p <<= 1)
        if (a / p == b / p) { *start = a / p * p; return p; }
    return 0;
}

/* RUGRATS_TEXLOG=1: every 600 tagged triangles, why they were (not) replaced */
static uint32_t why[8];
static void why_log(int k) {
    static int on = -1;
    if (on < 0) on = getenv("RUGRATS_TEXLOG") != NULL;
    if (!on) return;
    why[k]++;
    if (++why[7] % 600 == 0) rt_logf("texlog: depth3/fill %u, uvbox %u, span %u, drawarea %u, no entry %u, replaced %u\n",
                                     why[0], why[1], why[2], why[3], why[4], why[5]);
}

/* ---- per triangle (gpu_d3d11.c hw_add): which replacement, if any. dx0..dy1 = the drawing area (inclusive). ---- */
void tex_tag(HwVert *t, int dx0, int dy0, int dx1, int dy1) {
    t[0].rep = t[1].rep = t[2].rep = 0;
    t[0].rrect = t[1].rrect = t[2].rrect = 0;
    if (!n_ent && !dump_on) return;
    const uint32_t fl = t[0].flags;
    if (!(fl & HWF_TEX) || ((fl >> 12) & 3)) return;
    const int depth = (int)(fl & 3);
    if (depth == 3) { why_log(0); return; }
    const int k = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    const uint32_t lim = t[0].uvlim, tp = t[0].tpage;
    int u0, u1, v0, v1;
    if (fl & HWF_RECT) {   /* sprites: start corner + size (the quad's largest u, v) */
        int w = 0, h = 0;
        for (int i = 0; i < 3; i++) { if ((int)t[i].u > w) w = (int)t[i].u; if ((int)t[i].v > h) h = (int)t[i].v; }
        if (w <= 0 || h <= 0) return;
        int su = (int)(lim & 255), sv = (int)((lim >> 8) & 255);
        if (fl & HWF_FLIPX) { u0 = su - w + 1; u1 = su; } else { u0 = su; u1 = su + w - 1; }
        if (fl & HWF_FLIPY) { v0 = sv - h + 1; v1 = sv; } else { v0 = sv; v1 = sv + h - 1; }
    } else {
        u0 = (int)(lim & 255); v0 = (int)((lim >> 8) & 255); u1 = (int)((lim >> 16) & 255); v1 = (int)(lim >> 24);
    }
    int twmx = (int)((tp >> 8) & 31), twmy = (int)((tp >> 13) & 31), twox = (int)((tp >> 18) & 31), twoy = (int)((tp >> 23) & 31);
    if (twmx) { int M = twmx * 8, O = (twox & twmx) * 8; u0 = O; u1 = O | (~M & 255); }
    if (twmy) { int M = twmy * 8, O = (twoy & twmy) * 8; v0 = O; v1 = O | (~M & 255); }
    if (u0 < 0 || v0 < 0 || u1 > 255 || v1 > 255 || u0 > u1 || v0 > v1) { why_log(1); return; }
    int bu, bv;
    const int pu = span(u0, u1, &bu), pv = span(v0, v1, &bv);
    if (!pu || !pv) { why_log(2); return; }
    const int tpx = (int)(tp & 15) * 64, tpy = (int)((tp >> 4) & 1) * 256;
    const int bx = tpx + bu / k, by = tpy + bv, bw = pu / k, bh = pv;
    if (bx + bw > 1024 || by + bh > 512) return;
    if (bx <= dx1 && bx + bw > dx0 && by <= dy1 && by + bh > dy0) {   /* drawn this frame: not a stable texture */
        why_log(3);
        static int dl;
        if (getenv("RUGRATS_TEXLOG") && dl++ < 5) rt_logf("texlog: block %d,%d %dx%d in drawing area %d,%d-%d,%d\n", bx, by, bw, bh, dx0, dy0, dx1, dy1);
        return;
    }
    const uint64_t hash = cached((uint64_t)bx | (uint64_t)by << 10 | (uint64_t)bw << 19 | (uint64_t)bh << 29 | 1ull << 40,
                                 bx, by, bw, bh, 0);
    const int f = n_ent ? ent_first(hash) : -1;
    if (f < 0 && !dump_on) return;
    const int cx = (int)(t[0].clut & 0xFFFF), cy = (int)(t[0].clut >> 16), nc = depth == 0 ? 16 : 256;
    uint64_t clut = 0;
    if (depth < 2 && (dump_on || (f >= 0 && !ent[f].any)) && cx + nc <= 1024)   /* exact names sort first */
        clut = cached((uint64_t)cx | (uint64_t)cy << 10 | (uint64_t)nc << 19 | 2ull << 40, cx, cy & 511, nc, 1, 1);
    if (dump_on) dump(bx, by, bw, bh, hash, depth, cx, cy, clut);
    Entry *e = f >= 0 ? ent_find(hash, clut, depth < 2) : NULL;
    if (!e) { why_log(4); return; }
    why_log(5);
    if (!e->rec) load_entry(e);
    if (e->rec <= 0) return;
    const uint32_t rep = (uint32_t)e->rec | (uint32_t)bw << 16;
    const uint32_t rr = (uint32_t)bx | (uint32_t)by << 10 | (uint32_t)bh << 19;
    for (int i = 0; i < 3; i++) { t[i].rep = rep; t[i].rrect = rr; }
}

/* save states: VRAM is replaced wholesale on load, so every cached hash is stale */
void tex_state(StateIO *s) { if (st_loading(s)) epoch++; }
