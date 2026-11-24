/* Windows host: window + GDI presentation, keyboard/XInput, waveOut audio, frame pacing, config. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#include <stdlib.h>
#include <math.h>
#include "hw.h"
#include "present_d3d11.h"
#include "gpu_d3d11.h"
#include "savestate.h"
#define STATE_SLOTS 4
static int state_slot = 1; /* quick save / load slot (F6 cycles, F1 menu row) */
static int show_fps;        /* on-screen FPS counter (F3, F1 row) */
#include "pad_dualsense.h"

void gpu_display(int *x, int *y, int *w, int *h, int *is24, int *off);
const uint16_t *gpu_present_hr(int *x, int *y, int *w, int *h, int *late);
void rt_boot(void);

static HWND wnd;
static uint32_t *fb;          /* 32-bit frame (GDI, menu background, test shots); grows with the internal res */
static size_t fb_cap;         /* pixels allocated */
static int fb_w = 320, fb_h = 240;
static LARGE_INTEGER qpf, last_frame;
static int quit;
static uint8_t keys[256];
static int fullscreen;
static int test_input(void);
static WINDOWPLACEMENT prev_place = { sizeof prev_place };
static char ini_path[1100];
static int menu_open, pad_block;
static int bnd_wait;   /* Control Bindings: 1 = waiting for a key, 2 = for a controller button */
static int confirm_quit, confirm_from_menu, confirm_sel; /* quit prompt: open, opened from the F1 menu, 1 = Yes */
static void confirm_open(int from_menu);
static void menu_key(int vk);
static void menu_mouse(int x, int y, int button, int wheel);
static void menu_draw(HDC dc, int cw, int ch);
static int display_hz; /* refresh rate of the window's monitor */
static void query_hz(void);

/* ---- audio ---------------------------------------------------------- */
#define NBUF 12
static HWAVEOUT wo;
static WAVEHDR whdr[NBUF];
static int16_t wbuf[NBUF][800 * 2];
static int wnext;

static void audio_init(void) {
    WAVEFORMATEX f = { 0 };
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = 44100;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) wo = 0;
}

/* RUGRATS_AUDIOLOG=1: device underruns / drops every 2 s; RUGRATS_WAV=1: SPU output to audio.wav */
static uint32_t au_under, au_drop, au_pushes;
static FILE *au_wav;
static uint32_t au_wav_frames;

static int audio_queued(void) {
    int n = 0;
    for (int i = 0; i < NBUF; i++)
        if ((whdr[i].dwFlags & WHDR_PREPARED) && !(whdr[i].dwFlags & WHDR_DONE)) n++;
    return n;
}

static int audio_queued_frames(void) {
    int n = 0;
    for (int i = 0; i < NBUF; i++)
        if ((whdr[i].dwFlags & WHDR_PREPARED) && !(whdr[i].dwFlags & WHDR_DONE)) n += (int)whdr[i].dwBufferLength / 4;
    return n;
}

/* one buffer to the device (volume applied); dropped only if all NBUF buffers are still queued */
static void audio_write(const int16_t *lr, int frames) {
    WAVEHDR *h = &whdr[wnext];
    if (h->dwFlags & WHDR_PREPARED) {
        if (!(h->dwFlags & WHDR_DONE)) { au_drop++; return; }
        waveOutUnprepareHeader(wo, h, sizeof *h);
    }
    if (frames > 800) frames = 800;
    {   /* RUGRATS_OUTWAV=<file>: raw stereo exactly as sent to the device, before the volume (debug) */
        static FILE *ow; static int oi;
        if (!oi) { const char *e = getenv("RUGRATS_OUTWAV"); oi = 1; if (e) ow = fopen(e, "wb"); }
        if (ow) fwrite(lr, 4, (size_t)frames, ow);
    }
    memcpy(wbuf[wnext], lr, (size_t)frames * 4);
    if (opt.volume < 100) {
        int v = opt.volume < 0 ? 0 : opt.volume;
        for (int i = 0; i < frames * 2; i++) wbuf[wnext][i] = (int16_t)(wbuf[wnext][i] * v / 100);
    }
    memset(h, 0, sizeof *h);
    h->lpData = (LPSTR)wbuf[wnext];
    h->dwBufferLength = frames * 4;
    waveOutPrepareHeader(wo, h, sizeof *h);
    waveOutWrite(wo, h, sizeof *h);
    wnext = (wnext + 1) % NBUF;
}

static void wav_write(const int16_t *lr, int frames) {
    static int init = -1;
    if (init < 0) {
        init = 0;
        if (getenv("RUGRATS_WAV")) {
            char p[1100];
            GetModuleFileNameA(NULL, p, sizeof p);
            char *s = strrchr(p, '\\');
            if (s) strcpy(s + 1, "audio.wav");
            au_wav = fopen(p, "wb+");
            if (au_wav) { static const uint8_t hdr[44] = { 0 }; fwrite(hdr, 1, 44, au_wav); init = 1; }
        }
    }
    if (!au_wav) return;
    fwrite(lr, 4, (size_t)frames, au_wav);
    au_wav_frames += (uint32_t)frames;
    if ((au_pushes & 63) == 0) { /* keep the header valid so the file is usable if the game is killed */
        long pos = ftell(au_wav);
        uint32_t data = au_wav_frames * 4;
        uint8_t h[44];
        memcpy(h, "RIFF", 4); uint32_t v = 36 + data; memcpy(h + 4, &v, 4);
        memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
        uint16_t w = 1; memcpy(h + 20, &w, 2); w = 2; memcpy(h + 22, &w, 2);
        v = 44100; memcpy(h + 24, &v, 4); v = 44100 * 4; memcpy(h + 28, &v, 4);
        w = 4; memcpy(h + 32, &w, 2); w = 16; memcpy(h + 34, &w, 2);
        memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
        fseek(au_wav, 0, SEEK_SET); fwrite(h, 1, 44, au_wav); fseek(au_wav, pos, SEEK_SET);
        fflush(au_wav);
    }
}

void host_audio_push(const int16_t *lr, int frames) {
    au_pushes++;
    wav_write(lr, frames);
    if (!wo) return;
    {
        static int log = -1;
        static DWORD t0;
        extern uint32_t spu_stat_cd_under, spu_stat_cd_over, spu_stat_clip;
        if (log < 0) { log = getenv("RUGRATS_AUDIOLOG") != NULL; t0 = GetTickCount(); }
        if (log && GetTickCount() - t0 >= 2000) {
            static uint32_t last_pc;
            rt_logf("audio: %u pushes, device underruns %u, drops %u, queued %d | cd underruns %u, cd overflows %u,"
                    " clipped %u | presents %u at %d fps\n", au_pushes, au_under, au_drop, audio_queued(),
                    spu_stat_cd_under, spu_stat_cd_over, spu_stat_clip, present_count - last_pc, PRESENT_FPS);
            last_pc = present_count;
            au_pushes = au_under = au_drop = 0;
            spu_stat_cd_under = spu_stat_cd_over = spu_stat_clip = 0;
            t0 = GetTickCount();
        }
    }
    /* Keep ~AU_TARGET frames queued. The game's sample clock and the sound card's never match exactly
       (and frame times jitter), so each chunk is resampled by up to +-0.5% to hold the queue steady.
       After an underrun (PC stalled) the queue is refilled with a little silence once, instead of
       playing every following chunk with a gap in front of it. */
    enum { AU_TARGET = 735 * 4 };
    int qf = audio_queued_frames();
    static int started;
    static int16_t prev[2];
    static double rpos;
    {   /* always log audio gaps (a gap = an audible pop), at most one line per 2 s */
        extern uint32_t hooks_frame(void);
        static uint32_t seen_d, tot_u, tot_d, pushes; static DWORD lt;
        uint32_t u = (started && qf == 0) ? 1 : 0, d = au_drop >= seen_d ? au_drop - seen_d : au_drop;
        seen_d = au_drop;
        if (++pushes > 120) { tot_u += u; tot_d += d; } /* skip the start-up second */
        if ((tot_u || tot_d) && GetTickCount() - lt >= 2000) {
            rt_logf("audio gap: %u device underruns, %u dropped buffers (vblank %u, %d fps)\n", tot_u, tot_d, hooks_frame(),
                    PRESENT_FPS);
            tot_u = tot_d = 0; lt = GetTickCount();
        }
    }
    if (started && qf == 0) {
        au_under++;
        static const int16_t zero[735 * 2];
        for (int i = 0; i < 2; i++) audio_write(zero, 735);
        qf = 735 * 2;
    }
    started = 1;
    double ratio = 1.0 + (double)(AU_TARGET - qf) / AU_TARGET * 0.005;
    if (ratio > 1.005) ratio = 1.005;
    if (ratio < 0.995) ratio = 0.995;
    static int16_t out[800 * 2];
    int n = 0;
    double step = 1.0 / ratio, x = rpos;
    while (x < frames && n < 800) { /* linear interpolation over [prev, lr[0..frames-1]] */
        int i = (int)x;
        double f = x - i;
        const int16_t *a = i == 0 ? prev : &lr[(i - 1) * 2], *b = &lr[i * 2];
        out[n * 2] = (int16_t)(a[0] + (b[0] - a[0]) * f);
        out[n * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
        n++;
        x += step;
    }
    rpos = x - frames;
    if (rpos < 0) rpos = 0;
    prev[0] = lr[(frames - 1) * 2];
    prev[1] = lr[(frames - 1) * 2 + 1];
    audio_write(out, n);
}

/* ---- input ---------------------------------------------------------- */
#define XIN_GUIDE 0x0400 /* unused XInput bit, carries the PS button (menu controller code) */
#define XIN_TOGGLE (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB | XIN_GUIDE) /* opens / closes the menu */
typedef DWORD(WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE *);
static XInputGetState_t xget;

/* Control bindings (F1 > Audio/Input > Control Bindings...). Each PS1 button (action) has one keyboard key and
   one controller button. Controller buttons are "physical" PB_* inputs shared by XInput pads and Sony pads over
   HID (A = Cross, ...); the left stick always counts as the d-pad. L3 / R3 / PS stay reserved for the menu and the
   right stick for the camera. Saved in [bindings]: key_<action> = Windows VK code, pad_<action> = PB name. */
enum { PB_CROSS, PB_CIRCLE, PB_SQUARE, PB_TRIANGLE, PB_L1, PB_R1, PB_L2, PB_R2, PB_SELECT, PB_START,
       PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_N };
static const char *const pb_names[PB_N] = { "A / Cross", "B / Circle", "X / Square", "Y / Triangle", "LB / L1",
    "RB / R1", "LT / L2", "RT / R2", "Back / Share", "Start / Options", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right" };
static const char *const pb_ini[PB_N] = { "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2",
    "select", "start", "up", "down", "left", "right" };
enum { BA_UP, BA_DOWN, BA_LEFT, BA_RIGHT, BA_CROSS, BA_CIRCLE, BA_SQUARE, BA_TRIANGLE, BA_L1, BA_R1, BA_L2, BA_R2,
       BA_START, BA_SELECT, BA_N };
static const struct { const char *name, *ini; int bit, key, pad; } bind_def[BA_N] = {
    { "Up", "up", 4, VK_UP, PB_UP },              { "Down", "down", 6, VK_DOWN, PB_DOWN },
    { "Left", "left", 7, VK_LEFT, PB_LEFT },      { "Right", "right", 5, VK_RIGHT, PB_RIGHT },
    { "Cross", "cross", 14, 'Z', PB_CROSS },      { "Circle", "circle", 13, 'X', PB_CIRCLE },
    { "Square", "square", 15, 'A', PB_SQUARE },   { "Triangle", "triangle", 12, 'S', PB_TRIANGLE },
    { "L1", "l1", 10, 'Q', PB_L1 },               { "R1", "r1", 11, 'E', PB_R1 },
    { "L2", "l2", 8, '1', PB_L2 },                { "R2", "r2", 9, '3', PB_R2 },
    { "Start", "start", 3, VK_RETURN, PB_START }, { "Select", "select", 0, VK_BACK, PB_SELECT },
};
static int bind_key[BA_N], bind_pad[BA_N];

static void bind_defaults(void) {
    for (int a = 0; a < BA_N; a++) { bind_key[a] = bind_def[a].key; bind_pad[a] = bind_def[a].pad; }
}
/* keys the bindings may not take: Esc, Tab (fast-forward), F1-F12 (menu, saves, ...), J/L/I/K (camera) */
static int bind_key_reserved(int vk) {
    return vk == VK_ESCAPE || vk == VK_TAB || (vk >= VK_F1 && vk <= VK_F12) || vk == 'J' || vk == 'L' || vk == 'I' ||
           vk == 'K' || vk <= 0 || vk > 254;
}
/* bind; whatever action held that key / button before gets this action's old one (swap) */
static void bind_set_key(int a, int vk) {
    for (int o = 0; o < BA_N; o++) if (o != a && bind_key[o] == vk) bind_key[o] = bind_key[a];
    bind_key[a] = vk;
}
static void bind_set_pad(int a, int pb) {
    for (int o = 0; o < BA_N; o++) if (o != a && bind_pad[o] == pb) bind_pad[o] = bind_pad[a];
    bind_pad[a] = pb;
}
static void bind_load(const char *ini) {
    bind_defaults();
    for (int a = 0; a < BA_N; a++) {
        char k[32], v[32];
        snprintf(k, sizeof k, "key_%s", bind_def[a].ini);
        int vk = GetPrivateProfileIntA("bindings", k, bind_def[a].key, ini);
        if (!bind_key_reserved(vk)) bind_set_key(a, vk);
        snprintf(k, sizeof k, "pad_%s", bind_def[a].ini);
        GetPrivateProfileStringA("bindings", k, "", v, sizeof v, ini);
        for (int p = 0; p < PB_N; p++) if (!_stricmp(v, pb_ini[p])) bind_set_pad(a, p);
    }
}
static void bind_save(const char *ini) {
    for (int a = 0; a < BA_N; a++) {
        char k[32], v[16];
        snprintf(k, sizeof k, "key_%s", bind_def[a].ini);
        snprintf(v, sizeof v, "%d", bind_key[a]);
        WritePrivateProfileStringA("bindings", k, v, ini);
        snprintf(k, sizeof k, "pad_%s", bind_def[a].ini);
        WritePrivateProfileStringA("bindings", k, pb_ini[bind_pad[a]], ini);
    }
}
static void key_name(int vk, char *out, int n) {
    static const struct { int vk; const char *s; } t[] = {
        { VK_UP, "Up arrow" }, { VK_DOWN, "Down arrow" }, { VK_LEFT, "Left arrow" }, { VK_RIGHT, "Right arrow" },
        { VK_RETURN, "Enter" }, { VK_BACK, "Backspace" }, { VK_SPACE, "Space" }, { VK_SHIFT, "Shift" },
        { VK_CONTROL, "Ctrl" }, { VK_MENU, "Alt" }, { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" },
        { VK_HOME, "Home" }, { VK_END, "End" }, { VK_PRIOR, "Page Up" }, { VK_NEXT, "Page Down" },
        { VK_CAPITAL, "Caps Lock" }, { VK_PAUSE, "Pause" },
    };
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) if (t[i].vk == vk) { snprintf(out, n, "%s", t[i].s); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(out, n, "%c", vk); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, n, "Num %d", vk - VK_NUMPAD0); return; }
    UINT sc = MapVirtualKeyA((UINT)vk, 0 /* MAPVK_VK_TO_VSC */);
    if (!sc || !GetKeyNameTextA((LONG)(sc << 16), out, n)) snprintf(out, n, "Key %d", vk);
}

/* physical controller inputs held (PB_* bits): XInput pad `port`, plus Sony pads over HID on port 0 */
static uint32_t pad_phys(int port) {
    uint32_t p = 0;
    if (xget) {
        XINPUT_STATE st;
        if (xget(port, &st) == ERROR_SUCCESS) {
            WORD w = st.Gamepad.wButtons;
            SHORT lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
            if (w & XINPUT_GAMEPAD_BACK) p |= 1u << PB_SELECT;
            if (w & XINPUT_GAMEPAD_START) p |= 1u << PB_START;
            if ((w & XINPUT_GAMEPAD_DPAD_UP) || ly > 16000) p |= 1u << PB_UP;
            if ((w & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 16000) p |= 1u << PB_RIGHT;
            if ((w & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -16000) p |= 1u << PB_DOWN;
            if ((w & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -16000) p |= 1u << PB_LEFT;
            if (st.Gamepad.bLeftTrigger > 100) p |= 1u << PB_L2;
            if (st.Gamepad.bRightTrigger > 100) p |= 1u << PB_R2;
            if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) p |= 1u << PB_L1;
            if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) p |= 1u << PB_R1;
            if (w & XINPUT_GAMEPAD_Y) p |= 1u << PB_TRIANGLE;
            if (w & XINPUT_GAMEPAD_B) p |= 1u << PB_CIRCLE;
            if (w & XINPUT_GAMEPAD_A) p |= 1u << PB_CROSS;
            if (w & XINPUT_GAMEPAD_X) p |= 1u << PB_SQUARE;
        }
    }
    if (port == 0) { /* DualSense / DualShock 4 over HID (left stick already counts as the d-pad) */
        static const struct { uint32_t ds; int pb; } m[] = {
            { DS_SHARE, PB_SELECT }, { DS_OPTIONS, PB_START }, { DS_UP, PB_UP }, { DS_RIGHT, PB_RIGHT },
            { DS_DOWN, PB_DOWN }, { DS_LEFT, PB_LEFT }, { DS_L2, PB_L2 }, { DS_R2, PB_R2 }, { DS_L1, PB_L1 },
            { DS_R1, PB_R1 }, { DS_TRIANGLE, PB_TRIANGLE }, { DS_CIRCLE, PB_CIRCLE }, { DS_CROSS, PB_CROSS },
            { DS_SQUARE, PB_SQUARE },
        };
        uint32_t d = ds_buttons();
        for (int i = 0; i < (int)(sizeof m / sizeof m[0]); i++) if (d & m[i].ds) p |= 1u << m[i].pb;
    }
    return p;
}

int hooks_golf_holding(void);
int hooks_freecam_engaged(void);
/* the pad as held (active low), without the game-side blocks below: the freecam reads it */
uint16_t host_pad_raw(int port) {
    uint16_t b = 0;
    if (port == 0) b |= (uint16_t)test_input();
    if (port == 0 && GetForegroundWindow() == wnd)
        for (int a = 0; a < BA_N; a++) if (keys[bind_key[a] & 255]) b |= (uint16_t)(1 << bind_def[a].bit);
    uint32_t p = pad_phys(port);
    for (int a = 0; a < BA_N; a++) if (p & (1u << bind_pad[a])) b |= (uint16_t)(1 << bind_def[a].bit);
    if (pad_block && port == 0) b = 0;
    return (uint16_t)~b;
}
uint16_t host_pad_buttons(int port) {
    if (hooks_golf_holding()) return 0xFFFF;     /* Golf Reactions: hole end waits for the line, no input */
    uint16_t b = 0;
    if (port == 0) b |= (uint16_t)test_input();
    if (port == 0 && GetForegroundWindow() == wnd)
        for (int a = 0; a < BA_N; a++) if (keys[bind_key[a] & 255]) b |= (uint16_t)(1 << bind_def[a].bit);
    uint32_t p = pad_phys(port);
    for (int a = 0; a < BA_N; a++) if (p & (1u << bind_pad[a])) b |= (uint16_t)(1 << bind_def[a].bit);
    /* after the menu closes, hide buttons still held from it until everything is released */
    if (pad_block && port == 0) {
        if (b) b = 0;
        else pad_block = 0;
    }
    /* Debug Freecam: the pad flies the camera. Checked after the block release above: returning before it left
       pad_block set for good once the F1 menu closed (Freecam is switched on there), so host_pad_raw / host_rstick
       read nothing and the freecam never moved (2026-11-24). */
    if (port == 0 && hooks_freecam_engaged()) return 0xFFFF;
    return (uint16_t)~b;
}


/* ---- window --------------------------------------------------------- */
static void toggle_fullscreen(void) {
    DWORD style = GetWindowLong(wnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(wnd, &prev_place);
        GetMonitorInfo(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(wnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLong(wnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &prev_place);
        SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = !fullscreen;
    query_hz();
}

static void query_hz(void) {
    MONITORINFOEXA mi = { 0 };
    mi.cbSize = sizeof mi;
    DEVMODEA dm = { 0 };
    dm.dmSize = sizeof dm;
    int hz = 0;
    if (GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        hz = (int)dm.dmDisplayFrequency;
    if (hz <= 1) hz = 0; /* "default" */
    if (hz != display_hz) rt_logf("display: %d Hz\n", hz);
    display_hz = hz;
}

static void window_size(int *w, int *h) {
    *w = opt.widescreen ? 320 * opt.scale * 4 / 3 : 320 * opt.scale;
    *h = 240 * opt.scale;
}

static void resize_window(void) {
    if (fullscreen || !wnd) return;
    int cw, ch;
    window_size(&cw, &ch);
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    ShowWindow(wnd, SW_RESTORE);
    SetWindowPos(wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static void update_title(void) {
    char title[128];
    char lim[32] = "";
    if (present_fps > 0 && present_fps < opt.fps) snprintf(lim, sizeof lim, " (PC limit: %d)", present_fps);
    char md[32] = "";
    if (mods_active()) snprintf(md, sizeof md, " - %d mod%s", mods_active(), mods_active() == 1 ? "" : "s");
    snprintf(title, sizeof title, "Rugrats: Search for Reptar (native) - %d fps%s%s%s", opt.fps, lim, md,
             menu_open ? " - paused (F1)" : "");
    SetWindowTextA(wnd, title);
}

/* letterboxed picture rectangle inside the client area */
int hooks_movie_active(void);
/* 1 = this frame is shown 4:3 inside the 16:9 window: Stretch 2D Off while a movie plays (8008b448: logo movies,
   title attract demos, credits). Movie frames are MDEC pictures uploaded straight into VRAM, which the layer squeeze
   (gpu.c sq_*) can't reach; the picture fills the display area, so showing it 4:3 (black bars) is exact. */
int frame_43(void) { return opt.widescreen && !opt.stretch2d && hooks_movie_active(); }
static void frame_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    double aspect = opt.widescreen && !frame_43() ? 16.0 / 9.0 : 4.0 / 3.0;
    {   /* debug: RUGRATS_FRAMELOG=1 logs each change of the shown aspect */
        static int lg = -1; static double last;
        if (lg < 0) lg = getenv("RUGRATS_FRAMELOG") != NULL;
        if (lg && aspect != last) { rt_logf("frame aspect %s (movie %d)\n", aspect > 1.5 ? "16:9" : "4:3", hooks_movie_active()); last = aspect; }
    }
    *w = cw; *h = (int)(cw / aspect);
    if (*h > ch) { *h = ch; *w = (int)(ch * aspect); }
    *x = (cw - *w) / 2; *y = (ch - *h) / 2;
}

/* frame (letterboxed) into dc */
static void paint_frame(HDC dc, int cw, int ch) {
    int x, y, w, h;
    frame_rect(cw, ch, &x, &y, &w, &h);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = fb_w;
    bi.bmiHeader.biHeight = -fb_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, opt.smooth ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(dc, 0, 0, NULL);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r1 = { 0, 0, cw, y }, r2 = { 0, y + h, cw, ch }, r3 = { 0, y, x, y + h }, r4 = { x + w, y, cw, y + h };
    FillRect(dc, &r1, black); FillRect(dc, &r2, black); FillRect(dc, &r3, black); FillRect(dc, &r4, black);
    StretchDIBits(dc, x, y, w, h, 0, 0, fb_w, fb_h, fb, &bi, DIB_RGB_COLORS, SRCCOPY);
}

/* back buffer (client size) used while the menu is open: no flicker, and test mode can dump it */
static HDC back_dc;
static HBITMAP back_bmp;
static uint32_t *back_px;
static int back_w, back_h;

static void back_ensure(int w, int h) {
    if (back_dc && back_w == w && back_h == h) return;
    if (back_bmp) DeleteObject(back_bmp);
    if (!back_dc) back_dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits;
    back_bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    back_px = bits;
    SelectObject(back_dc, back_bmp);
    back_w = w; back_h = h;
}

/* paused frame, dimmed, with the menu on top, into the back buffer. The game is paused, so the dimmed frame is
   made once and copied each menu frame (the HALFTONE stretch + dim of a full window cost tens of ms at 1440p/4K
   and made the menu sluggish); remade when the window size / aspect / filter changes or the menu reopens. */
static uint32_t *menu_bg;
static int menu_bg_w, menu_bg_h, menu_bg_key = -1;
static void menu_bg_reset(void) { menu_bg_key = -1; }
static void compose_menu(int cw, int ch) {
    back_ensure(cw, ch);
    int key = (opt.widescreen && !frame_43()) | (opt.smooth ? 2 : 0);
    if (!menu_bg || menu_bg_key != key || menu_bg_w != cw || menu_bg_h != ch) {
        paint_frame(back_dc, cw, ch);
        GdiFlush();
        for (int i = 0; i < cw * ch; i++) back_px[i] = (back_px[i] >> 2) & 0x3F3F3F;
        uint32_t *nb = realloc(menu_bg, (size_t)cw * ch * 4);
        if (nb) { menu_bg = nb; memcpy(menu_bg, back_px, (size_t)cw * ch * 4); menu_bg_w = cw; menu_bg_h = ch; menu_bg_key = key; }
    } else {
        GdiFlush();
        memcpy(back_px, menu_bg, (size_t)cw * ch * 4);
    }
    menu_draw(back_dc, cw, ch);
    GdiFlush();
}

/* GDI fallback presentation (no vsync) */
static void paint(HDC dc) {
    RECT rc;
    GetClientRect(wnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;
    if (!menu_open) { paint_frame(dc, cw, ch); return; }
    compose_menu(cw, ch);
    BitBlt(dc, 0, 0, cw, ch, back_dc, 0, 0, SRCCOPY);
}
static void show(void);

/* Mouse cursor: hidden over the game window after CURSOR_IDLE_MS without movement, shown again on any move
   (WM_MOUSEMOVE also fires without movement, e.g. when a window appears, so the position is compared). */
#define CURSOR_IDLE_MS 2000
static DWORD cursor_moved;
static int cursor_hidden;
static POINT cursor_last = { -100000, -100000 };
static int cursor_move(LPARAM lp) { /* 1 = the mouse really moved */
    POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
    if (p.x == cursor_last.x && p.y == cursor_last.y) return 0;
    cursor_last = p;
    cursor_moved = GetTickCount();
    if (cursor_hidden) { cursor_hidden = 0; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
    return 1;
}
static void cursor_tick(void) {
    if (cursor_hidden || GetTickCount() - cursor_moved < CURSOR_IDLE_MS) return;
    cursor_hidden = 1;
    POINT p, q;
    RECT r;
    if (GetCursorPos(&p) && wnd && WindowFromPoint(p) == wnd && (q = p, ScreenToClient(wnd, &q)) &&
        GetClientRect(wnd, &r) && PtInRect(&r, q))
        SetCursor(NULL);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CLOSE: quit = 1; return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { toggle_fullscreen(); return 0; }
        if (wp == VK_F11) { toggle_fullscreen(); return 0; }
        if (wp == VK_F1) { menu_open = !menu_open; if (!menu_open) pad_block = 1; return 0; }
        if (menu_open && bnd_wait && (lp & (1 << 30))) return 0; /* key repeat must not bind the key that started it */
        if (menu_open) { menu_key((int)wp); return 0; }
        if (wp == VK_F3) { show_fps = !show_fps; return 0; }            /* FPS counter */
        if (wp == VK_F5) { state_request(1, state_slot); return 0; }  /* quick save (at the next game frame) */
        if (wp == VK_F7) { state_request(0, state_slot); return 0; }  /* quick load */
        if (wp == VK_F6) {                                              /* next slot */
            state_slot = state_slot % STATE_SLOTS + 1;
            snprintf(state_msg, sizeof state_msg, "Slot %d%s", state_slot, state_slot_exists(state_slot) ? "" : " (empty)");
            state_msg_time = GetTickCount();
            return 0;
        }
        if (wp == VK_F9) { opt.widescreen = !opt.widescreen; return 0; }
        if (wp == VK_F8) { /* cycle frame rate; 60/75/100/150 give perfectly even motion */
            static const int rates[] = { 60, 75, 100, 120, 144, 150, 240 };
            int i = 0;
            while (i < 7 && rates[i] != opt.fps) i++;
            opt.fps = rates[(i + 1) % 7];
            update_title();
            return 0;
        }
        if (wp == VK_ESCAPE) { confirm_open(0); return 0; }                 /* quit, after a Yes/No prompt */
        keys[wp & 255] = 1;
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP: keys[wp & 255] = 0; return 0;
    case WM_KILLFOCUS: memset(keys, 0, sizeof keys); return 0;
    case WM_MOUSEMOVE:
        /* hover selects only on a real move: Windows also sends WM_MOUSEMOVE without one (window shown, cursor
           hidden, ...), which made a parked cursor keep grabbing the row under it (e.g. Reptar 2010 in Level Select) */
        if (cursor_move(lp) && menu_open) menu_mouse((short)LOWORD(lp), (short)HIWORD(lp), 0, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && cursor_hidden) { SetCursor(NULL); return TRUE; }
        break;
    case WM_LBUTTONDOWN: if (menu_open) menu_mouse((short)LOWORD(lp), (short)HIWORD(lp), 1, 0); return 0;
    case WM_RBUTTONDOWN: if (menu_open) menu_mouse((short)LOWORD(lp), (short)HIWORD(lp), 2, 0); return 0;
    case WM_MOUSEWHEEL:
        if (menu_open) {
            POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(h, &p);
            menu_mouse(p.x, p.y, 0, (short)HIWORD(wp) > 0 ? 1 : -1);
        }
        return 0;
    case WM_PAINT:
        if (d3d_ok()) { ValidateRect(h, NULL); show(); return 0; }
        { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint(dc); EndPaint(h, &ps); return 0; }
    case WM_DISPLAYCHANGE: case WM_EXITSIZEMOVE: query_hz(); break;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(h, m, wp, lp);
}

void host_init(void) {
    fb_cap = (size_t)640 * 480;
    fb = calloc(fb_cap, 4);
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "RugratsNative";
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    RegisterClass(&wc);
    int cw, ch;
    window_size(&cw, &ch);
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
    {   /* RUGRATS_WINPOS=<x>,<y> (debug): open the window there (fullscreen then uses that monitor) */
        const char *e = getenv("RUGRATS_WINPOS");
        if (e && sscanf(e, "%d,%d", &wx, &wy) != 2) wx = wy = CW_USEDEFAULT;
    }
    wnd = CreateWindow("RugratsNative", "Rugrats: Search for Reptar (native)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                       wx, wy, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) xi = LoadLibraryA("xinput9_1_0.dll");
    if (xi) xget = (XInputGetState_t)(void *)GetProcAddress(xi, "XInputGetState");
    update_title();
    query_hz();
    ds_start();
    if (!getenv("RUGRATS_GDI")) d3d_init(wnd);
    rt_logf("present: %s, vsync %s\n", d3d_ok() ? "d3d11" : "gdi", opt.vsync ? "on" : "off");
    if (opt.fullscreen) toggle_fullscreen();
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&last_frame);
    audio_init();
}

static void convert_frame(void) {
    int x, y, w, h, is24, off;
    gpu_display(&x, &y, &w, &h, &is24, &off);
    if (w > 640) w = 640;
    if (h > 480) h = 480;
    static uint32_t lut[32768];
    if (!lut[32767]) /* 15-bit BGR -> 0x00RRGGBB */
        for (int p = 0; p < 32768; p++)
            lut[p] = ((uint32_t)(p & 31) << 19) | ((uint32_t)((p >> 5) & 31) << 11) | ((uint32_t)((p >> 10) & 31) << 3);
    /* display off (e.g. between movies): a native-size black frame. Before 2026-11-07 this read the whole GPU picture
       back first (~50 ms at 9x) and cleared/uploaded a 9x-sized frame, which set off the auto frame-rate limiter and
       made the next movies judder at 84-114 fps. */
    if (off) {
        if ((size_t)w * h > fb_cap) { uint32_t *n = realloc(fb, (size_t)w * h * 4); if (!n) return; fb = n; fb_cap = (size_t)w * h; }
        fb_w = w; fb_h = h;
        memset(fb, 0, (size_t)w * h * 4);
        return;
    }
    int s = is24 ? 1 : gpu_scale(); /* 24-bit (movie) frames come from native VRAM */
    size_t need = (size_t)w * s * h * s;
    if (need > fb_cap) {
        uint32_t *n = realloc(fb, need * 4);
        if (!n) { s = 1; } else { fb = n; fb_cap = need; }
    }
    if (!is24 && (s > 1 || gpu_hw_on())) { /* the GPU renderer's target also at 1x (native VRAM is not kept) */
        int hw = 1024 * s, hh = 512 * s;
        fb_w = w * s; fb_h = h * s;
        const uint16_t *hv = gpu_vram_hr();
        for (int yy = 0; yy < fb_h; yy++) {
            uint32_t *d = fb + (size_t)yy * fb_w;
            const uint16_t *src = hv + (size_t)((y * s + yy) % hh) * hw;
            int sx = x * s;
            if (sx + fb_w <= hw) for (int xx = 0; xx < fb_w; xx++) d[xx] = lut[src[sx + xx] & 0x7FFF];
            else for (int xx = 0; xx < fb_w; xx++) d[xx] = lut[src[(sx + xx) % hw] & 0x7FFF];
        }
        return;
    }
    fb_w = w; fb_h = h;
    if (off) { memset(fb, 0, (size_t)w * h * 4); return; }
    for (int yy = 0; yy < h; yy++) {
        uint32_t *d = fb + yy * w;
        int vy = (y + yy) & 511;
        if (!is24) {
            for (int xx = 0; xx < w; xx++) d[xx] = lut[vram[vy * 1024 + ((x + xx) & 1023)] & 0x7FFF];
        } else {
            const uint8_t *src = (const uint8_t *)&vram[vy * 1024];
            for (int xx = 0; xx < w; xx++) {
                int bo = (x * 2 + xx * 3) & 2047;
                uint32_t r = src[bo], gg = src[(bo + 1) & 2047], b = src[(bo + 2) & 2047];
                d[xx] = (r << 16) | (gg << 8) | b;
            }
        }
    }
}

int host_quit_requested(void) { return quit; }

/* test mode: RUGRATS_TEST=<frames>,<every> -> unpaced, dumps BMPs next to the exe, then exits */
static int test_frames, test_every, test_paced; int frame_no; /* RUGRATS_PACED=1: real-time pacing + vsync in test mode */

uint32_t hooks_frame(void);
static void dump_bmp(void) {
    char dir[1024], p[1200];
    static int shot_from = -1;   /* RUGRATS_SHOTFROM=<frame>: no shots before that test frame (debug) */
    if (shot_from < 0) { const char *e = getenv("RUGRATS_SHOTFROM"); shot_from = e ? atoi(e) : 0; }
    if (frame_no < shot_from) return;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    snprintf(p, sizeof p, "%s\\shots", dir);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\shots\\f%05d.bmp", dir, frame_no);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    if (getenv("RUGRATS_SHOTLOG")) rt_logf("shot f%05d vblank %u\n", frame_no, hooks_frame()); /* debug: line runs up by emulated time */    if (getenv("RUGRATS_SHOTRAM")) {  /* debug: also write guest RAM as shots\rNNNNN.bin */
        extern uint8_t ram[];
        char q[1200];
        snprintf(q, sizeof q, "%s\\shots\\r%05d.bin", dir, frame_no);
        FILE *g = fopen(q, "wb");
        if (g) { fwrite(ram, 1, 0x200000, g); fclose(g); }
    }
    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { 0 };
    ih.biSize = sizeof ih; ih.biWidth = fb_w; ih.biHeight = -fb_h; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + fb_w * fb_h * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(fb, 4, (size_t)fb_w * fb_h, f);
    fclose(f);
    /* full VRAM too, for debugging */
    snprintf(p, sizeof p, "%s\\shots\\v%05d.bmp", dir, frame_no);
    f = fopen(p, "wb");
    if (!f) return;
    ih.biWidth = 1024; ih.biHeight = -512;
    fh.bfSize = fh.bfOffBits + 1024 * 512 * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    for (int i = 0; i < 1024 * 512; i++) {
        uint16_t px = vram[i];
        uint32_t c = (((px & 31) << 3) << 16) | ((((px >> 5) & 31) << 3) << 8) | (((px >> 10) & 31) << 3);
        fwrite(&c, 4, 1, f);
    }
    fclose(f);
}

static double test_t60; /* test time in 60 Hz frames */

/* ---- bench mode ------------------------------------------------------------
   RUGRATS_BENCH=<settle>,<measure> (60 Hz frames; implies an unpaced test run). Waits until the gameplay loop of the
   target level runs (RUGRATS_LEVEL=<db>, else the first one, the hub), lets it settle, then records the real time of
   every presented frame for <measure> frames while a fixed walk/turn script plays, saves one screenshot
   (shots/bench_dbNN.bmp) and appends one row to bench.csv next to the exe (or RUGRATS_BENCH_OUT), then exits.
   Exit code 0 = measured, 3 = the level's gameplay never started within RUGRATS_BENCH_TIMEOUT (default 30000). */
extern int hooks_level;
static void exe_dir(char *out, size_t n);
static int bench_settle, bench_measure, bench_level = -1, bench_timeout = 30000;
static int bench_phase;              /* 0 = waiting for gameplay, 1 = settling, 2 = measuring */
static double bench_t0;              /* test time (60 Hz frames) when the phase began */
static float *bench_ms;
static int bench_n;
static uint32_t bench_flips0;
static int bench_shot;               /* 1 = convert + save the next frame as the bench screenshot */

static int bench_cmp(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void bench_finish(const char *status) {
    char dir[1024], p[1200];
    exe_dir(dir, sizeof dir);
    const char *out = getenv("RUGRATS_BENCH_OUT");
    if (out) snprintf(p, sizeof p, "%s", out); else snprintf(p, sizeof p, "%s\\bench.csv", dir);
    double avg = 0, p50 = 0, p95 = 0, p99 = 0, mx = 0;
    if (bench_n > 0) {
        for (int i = 0; i < bench_n; i++) avg += bench_ms[i];
        avg /= bench_n;
        qsort(bench_ms, (size_t)bench_n, sizeof *bench_ms, bench_cmp);
        p50 = bench_ms[bench_n / 2]; p95 = bench_ms[bench_n * 95 / 100]; p99 = bench_ms[bench_n * 99 / 100];
        mx = bench_ms[bench_n - 1];
    }
    extern uint32_t gpu_flips;
    char build[64] = "?";
    { /* build id: exe file time */
        char exe[1024];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        SYSTEMTIME st;
        if (GetFileAttributesExA(exe, GetFileExInfoStandard, &fa) && FileTimeToSystemTime(&fa.ftLastWriteTime, &st))
            snprintf(build, sizeof build, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    FILE *f = fopen(p, "rb");
    int fresh = !f;
    if (f) fclose(f);
    f = fopen(p, "ab");
    if (f) {
        if (fresh)
            fprintf(f, "build,level,status,renderer,internal,fps,present_fps,overclock,widescreen,frames,avg_ms,p50_ms,p95_ms,"
                       "p99_ms,max_ms,avg_fps,p99_fps,game_frames\n");
        fprintf(f, "%s,%d,%s,%s,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%u\n", build,
                bench_level >= 0 ? bench_level : hooks_level, status, gpu_hw_on() ? "gpu" : "cpu", gpu_scale(), opt.fps,
                PRESENT_FPS, opt.overclock, opt.widescreen, bench_n, avg, p50, p95, p99, mx, avg > 0 ? 1000.0 / avg : 0.0,
                p99 > 0 ? 1000.0 / p99 : 0.0, bench_phase == 2 ? gpu_flips - bench_flips0 : 0u);
        fclose(f);
    }
    rt_logf("bench: db %d %s, %d frames, avg %.2f ms, p99 %.2f ms, max %.2f ms -> %s\n",
            bench_level >= 0 ? bench_level : hooks_level, status, bench_n, avg, p99, mx, p);
    exit(strcmp(status, "ok") == 0 ? 0 : 3);
}

/* RUGRATS_STATE_SHOT=<n> (tests): save shots/state_save.bmp / state_load.bmp n presented frames after a quick
   save / load. With the test clock restored by a load, both show the same game moment and must match. */
static int state_shot_at = -1, state_shot_n, state_shot_now;
static char state_shot_name[32];
static void save_named_bmp(const char *name) {
    char dir[1024], p[1200];
    exe_dir(dir, sizeof dir);
    snprintf(p, sizeof p, "%s\\shots", dir);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\shots\\%s.bmp", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { 0 };
    ih.biSize = sizeof ih; ih.biWidth = fb_w; ih.biHeight = -fb_h; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + fb_w * fb_h * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(fb, 4, (size_t)fb_w * fb_h, f);
    fclose(f);
    rt_logf("test: wrote shots\\%s.bmp\n", name);
}
/* called by savestate.c after a save (loaded = 0) or a load (1) */
void host_state_done(int loaded) {
    if (state_shot_at < 0) { const char *e = getenv("RUGRATS_STATE_SHOT"); state_shot_at = e ? atoi(e) : 0; }
    if (state_shot_at <= 0) return;
    state_shot_n = 0;
    snprintf(state_shot_name, sizeof state_shot_name, "state_%s", loaded ? "load" : "save");
}
/* host values kept in a state (tests only: the scripted-input clock, so a load replays the same input) */
void host_state(StateIO *s) {
    st_prefix(s, "host");
    double t = test_t60;
    ST(s, t);
    if (st_loading(s) && test_frames) { test_t60 = t; rt_logf("test: script clock now t60 %.0f\n", t); }
}

static void bench_save_shot(void) {
    char dir[1024], p[1200];
    exe_dir(dir, sizeof dir);
    snprintf(p, sizeof p, "%s\\shots", dir);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\shots\\bench_db%02d.bmp", dir, bench_level >= 0 ? bench_level : hooks_level);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { 0 };
    ih.biSize = sizeof ih; ih.biWidth = fb_w; ih.biHeight = -fb_h; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + fb_w * fb_h * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(fb, 4, (size_t)fb_w * fb_h, f);
    fclose(f);
}

/* once per presented frame; dt_ms = real time since the previous present */
static void bench_step(double dt_ms) {
    if (!bench_measure) return;
    if (bench_shot) { bench_shot = 0; bench_save_shot(); }
    int target = bench_level >= 0 ? bench_level : -1;
    switch (bench_phase) {
    case 0:
        if (hooks_level >= 0 && (target < 0 || hooks_level == target)) {
            bench_phase = 1; bench_t0 = test_t60;
            rt_logf("bench: db %d gameplay at test frame %.0f, settling\n", hooks_level, test_t60);
        } else if (test_t60 > bench_timeout) {
            bench_finish("timeout");
        }
        break;
    case 1:
        if (test_t60 - bench_t0 >= bench_settle) {
            extern uint32_t gpu_flips;
            bench_phase = 2; bench_t0 = test_t60; bench_n = 0; bench_flips0 = gpu_flips;
        }
        break;
    case 2:
        if (bench_n < 1 << 20) bench_ms[bench_n++] = (float)dt_ms;
        if (test_t60 - bench_t0 >= bench_measure / 2 && test_t60 - bench_t0 - 60.0 / PRESENT_FPS < bench_measure / 2)
            bench_shot = 1;
        if (test_t60 - bench_t0 >= bench_measure) bench_finish("ok");
        break;
    }
}

/* bench input: get through menus, intro and hint screens to the level, then a fixed walk/turn loop */
static int bench_input(int t) {
    int f = t % 240, m = 0;
    if (bench_phase == 0) {
        if (f >= 200 && f < 210) m |= 1 << 3;      /* START */
        if (f >= 100 && f < 110) m |= 1 << 14;     /* CROSS */
        return m;
    }
    int u = (int)(test_t60 - bench_t0);
    if (u % 240 >= 100 && u % 240 < 106) m |= 1 << 14; /* CROSS: dismiss hint boxes (jumps in gameplay) */
    int k = u % 600;                                   /* 10 s cycle: forward, forward+left, forward, back+right */
    if (k < 200) m |= 1 << 4;
    else if (k < 300) m |= 1 << 4 | 1 << 7;
    else if (k < 500) m |= 1 << 4;
    else m |= 1 << 6 | 1 << 5;
    return m;
}

static int test_input(void) {
    /* scripted presses so the test gets through menus/hints into gameplay, then walks */
    if (!test_frames) return 0;
    int t = (int)test_t60; /* script runs on 60 Hz time at any (also changing) frame rate */
    if (bench_measure) return bench_input(t);
    {   /* RUGRATS_INPUT=<t60>:<hex buttons>[:<frames held, default 6>],... replaces the default script (tests).
           Buttons (PS1 bits): select 1, start 8, up 10, right 20, down 40, left 80, L2 100, R2 200, L1 400,
           R1 800, triangle 1000, circle 2000, cross 4000, square 8000. */
        static int n = -1, given, at[256], len[256], btn[256];
        if (n < 0) {
            n = 0;
            const char *e = getenv("RUGRATS_INPUT");
            given = e != NULL;
            while (e && *e && n < 256) {
                int a = 0, l = 6, used = 0;
                unsigned b = 0;
                if (sscanf(e, "%d:%x%n", &a, &b, &used) < 2) break;
                e += used;
                if (*e == ':') { int u2 = 0; sscanf(e + 1, "%d%n", &l, &u2); e += 1 + u2; }
                at[n] = a; btn[n] = (int)b; len[n] = l; n++;
                if (*e == ',') e++;
            }
            if (given) rt_logf("test: RUGRATS_INPUT with %d presses\n", n);
        }
        if (given) {
            int m = 0;
            for (int i = 0; i < n; i++) if (t >= at[i] && t < at[i] + len[i]) m |= btn[i];
            return m;
        }
    }
    int f = t % 240, m = 0;
    static int start_until = -1, walk_after;
    if (start_until < 0) {
        const char *e = getenv("RUGRATS_SCRIPT"); /* <start-until>,<walk-after> in 60 Hz frames */
        start_until = 2700; walk_after = 3000;
        if (e) sscanf(e, "%d,%d", &start_until, &walk_after);
    }
    if (f >= 200 && f < 210 && t < start_until) m |= 1 << 3; /* START (until gameplay) */
    if (f >= 100 && f < 110) {                       /* CROSS, or RUGRATS_TESTBTN=<hex mask> */
        const char *e = getenv("RUGRATS_TESTBTN");
        m |= e ? (int)strtol(e, 0, 16) : 1 << 14;
        if (t >= start_until) m &= ~(1 << 3);
    }
    if (t > walk_after && (t / 90) % 2) m |= 1 << 4; /* walk forward in bursts */
    return m;
}

/* Right stick for the camera (hooks.c): x = right, y = up, each -1..1 after a dead zone. Sources: XInput,
   DualSense/DS4 over HID, keys J/L (left/right) and I/K (up/down). Tests: RUGRATS_RSTICK=<t60 from>:<t60 to>:<x>:<y>,... */
void host_rstick(float *x, float *y) {
    float fx = 0, fy = 0;
    if (pad_block) { *x = *y = 0; return; }
    if (test_frames) {
        static int n = -1, from[16], to[16];
        static float sx[16], sy[16];
        if (n < 0) {
            n = 0;
            const char *e = getenv("RUGRATS_RSTICK");
            while (e && *e && n < 16) {
                int used = 0;
                if (sscanf(e, "%d:%d:%f:%f%n", &from[n], &to[n], &sx[n], &sy[n], &used) < 4 || !used) break;
                n++;
                e += used;
                if (*e == ',') e++;
            }
        }
        for (int i = 0; i < n; i++)
            if (test_t60 >= from[i] && test_t60 < to[i]) { fx += sx[i]; fy += sy[i]; }
    }
    if (GetForegroundWindow() == wnd) {
        if (keys['J']) fx -= 1;
        if (keys['L']) fx += 1;
        if (keys['I']) fy += 1;
        if (keys['K']) fy -= 1;
    }
    float px = 0, py = 0;
    if (xget) {
        XINPUT_STATE st;
        if (xget(0, &st) == ERROR_SUCCESS) { px = st.Gamepad.sThumbRX / 32767.0f; py = st.Gamepad.sThumbRY / 32767.0f; }
    }
    {
        int dx, dy;
        ds_rstick(&dx, &dy);
        float qx = dx / 127.0f, qy = -dy / 127.0f;
        if (qx * qx + qy * qy > px * px + py * py) { px = qx; py = qy; }
    }
    /* radial dead zone (worn sticks rest off-centre), rescaled so movement starts at 0 */
    float m = sqrtf(px * px + py * py);
    const float dz = 0.2f;
    if (m > dz) {
        float k = (m > 1 ? 1 : m);
        k = (k - dz) / (1 - dz) / m;
        fx += px * k;
        fy += py * k;
    }
    if (fx > 1) fx = 1;
    if (fx < -1) fx = -1;
    if (fy > 1) fy = 1;
    if (fy < -1) fy = -1;
    *x = fx;
    *y = fy;
}

/* left stick as analog (Debug Freecam movement; the game itself only sees it as the d-pad): XInput or DualSense /
   DS4, whichever is pushed further; same radial dead zone as host_rstick. No keys (those come as d-pad buttons).
   Test: RUGRATS_LSTICK=<from t60>:<to t60>:<x>:<y>[,...] (as RUGRATS_RSTICK). */
void host_lstick(float *x, float *y) {
    float fx = 0, fy = 0;
    *x = *y = 0;
    if (pad_block) return;
    if (test_frames) {
        static int n = -1, from[16], to[16];
        static float sx[16], sy[16];
        if (n < 0) {
            n = 0;
            const char *e = getenv("RUGRATS_LSTICK");
            while (e && *e && n < 16) {
                int used = 0;
                if (sscanf(e, "%d:%d:%f:%f%n", &from[n], &to[n], &sx[n], &sy[n], &used) < 4 || !used) break;
                n++;
                e += used;
                if (*e == ',') e++;
            }
        }
        for (int i = 0; i < n; i++)
            if (test_t60 >= from[i] && test_t60 < to[i]) { fx += sx[i]; fy += sy[i]; }
    }
    float px = 0, py = 0;
    if (xget) {
        XINPUT_STATE st;
        if (xget(0, &st) == ERROR_SUCCESS) { px = st.Gamepad.sThumbLX / 32767.0f; py = st.Gamepad.sThumbLY / 32767.0f; }
    }
    {
        int dx, dy;
        ds_lstick(&dx, &dy);
        float qx = dx / 127.0f, qy = -dy / 127.0f;
        if (qx * qx + qy * qy > px * px + py * py) { px = qx; py = qy; }
    }
    float m = sqrtf(px * px + py * py);
    const float dz = 0.2f;
    if (m > dz) {
        float k = (m > 1 ? 1 : m);
        k = (k - dz) / (1 - dz) / m;
        fx += px * k;
        fy += py * k;
    }
    if (fx > 1) fx = 1;
    if (fx < -1) fx = -1;
    if (fy > 1) fy = 1;
    if (fy < -1) fy = -1;
    *x = fx;
    *y = fy;
}

/* ---- presentation -------------------------------------------------------
   VSync on: if the display refresh equals the frame rate (59/60 Hz counts as 60), Present(1) paces
   the game; otherwise frames are timed by the clock and shown without tearing. (Present(2+) does not
   throttle reliably in windowed flip mode, so multiples like 72 fps on 144 Hz use the clock.)
   VSync off: clock pacing, tearing allowed (lowest latency, works with G-Sync / FreeSync). */
static int vsync_interval(void) {
    if (!d3d_ok() || !opt.vsync || keys[VK_TAB] || (test_frames && !test_paced) || display_hz <= 0 || PRESENT_FPS <= 0) return 0;
    return abs(display_hz - PRESENT_FPS) <= 1 ? 1 : 0;
}

/* the normal case: 15-bit (shadow) VRAM goes straight to a GPU texture, no 32-bit frame on the CPU */
static int direct_frame(void) {
    if (!d3d_has16() || menu_open) return 0;
    int x, y, w, h, is24, off;
    gpu_display(&x, &y, &w, &h, &is24, &off);
    return !is24 && !off;
}

/* quick save / load message (state_msg) for 3 seconds, as a small image the presenter draws on top */
static void osd_update(int ch) {
    static char shown[128];
    static int shown_h;
    static HDC dc;
    static HBITMAP bmp;
    static HFONT font;
    static uint32_t *px;
    static int w, h;
    int on = state_msg[0] && GetTickCount() - state_msg_time < 3000;
    if (!on) { if (shown[0]) { shown[0] = 0; d3d_osd(NULL, 0, 0); } return; }
    int fh = ch / 28 < 14 ? 14 : ch / 28;
    if (!strcmp(shown, state_msg) && shown_h == fh) return;
    snprintf(shown, sizeof shown, "%s", state_msg);
    shown_h = fh;
    if (!dc) dc = CreateCompatibleDC(NULL);
    if (font) DeleteObject(font);
    font = CreateFontA(-fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32A(dc, shown, (int)strlen(shown), &sz);
    w = sz.cx + fh; h = sz.cy + fh / 2;
    if (bmp) DeleteObject(bmp);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp) { shown[0] = 0; return; }
    SelectObject(dc, bmp);
    for (int i = 0; i < w * h; i++) px[i] = 0x202020;
    for (int x = 0; x < w; x++) px[x] = px[(h - 1) * w + x] = 0x8B5CF6; /* purple border like the menu */
    for (int y = 0; y < h; y++) px[y * w] = px[y * w + w - 1] = 0x8B5CF6;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, fh / 2, fh / 4, shown, (int)strlen(shown));
    GdiFlush();
    d3d_osd(px, w, h);
}

/* FPS counter (F3, F1 row, ini [video] fps_counter): frames actually shown per real second, top-right.
   Sized from the window height (about 1/16 of it: ~135 px tall at 4K) so it stays readable on big screens. */
static int fps_value = -1;           /* measured, updated twice a second */
static void fps_count(void) {        /* once per presented frame */
    static LARGE_INTEGER t0;
    static int n;
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    if (!t0.QuadPart) { t0 = now; n = 0; return; }
    n++;
    double dt = (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    if (dt >= 0.5) { fps_value = (int)(n / dt + 0.5); n = 0; t0 = now; }
}
static void fps_update(int ch) {
    static int shown = -2, shown_h;
    static HDC dc;
    static HBITMAP bmp;
    static HFONT big, small;
    static uint32_t *px;
    if (!show_fps || menu_open) { if (shown != -2) { shown = -2; d3d_overlay(1, NULL, 0, 0); } return; }
    int fh = ch / 16 < 20 ? 20 : ch / 16;
    if (fps_value == shown && fh == shown_h) return;
    shown = fps_value; shown_h = fh;
    if (!dc) dc = CreateCompatibleDC(NULL);
    if (big) DeleteObject(big);
    if (small) DeleteObject(small);
    big = CreateFontA(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    small = CreateFontA(-(fh * 2 / 5), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    char num[16];
    if (shown < 0) snprintf(num, sizeof num, "--"); else snprintf(num, sizeof num, "%d", shown);
    SIZE sn, sl;
    SelectObject(dc, big);
    GetTextExtentPoint32A(dc, "888", 3, &sn);  /* fixed width for up to 3 digits: the box doesn't jump around */
    SIZE cur;
    GetTextExtentPoint32A(dc, num, (int)strlen(num), &cur);
    if (cur.cx > sn.cx) sn.cx = cur.cx;
    SelectObject(dc, small);
    GetTextExtentPoint32A(dc, "fps", 3, &sl);
    int pad = fh / 5;
    int w = pad + sn.cx + pad / 2 + sl.cx + pad, h = sn.cy + pad / 2;
    if (bmp) DeleteObject(bmp);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp) { shown = -2; return; }
    SelectObject(dc, bmp);
    for (int i = 0; i < w * h; i++) px[i] = 0x181818;
    SetBkMode(dc, TRANSPARENT);
    /* colour: green at/above the target rate, yellow below it, red below 3/4 of it */
    int target = present_fps > 0 ? present_fps : opt.fps;
    COLORREF col = shown < 0 || shown >= target - 2 ? RGB(120, 230, 120) : shown >= target * 3 / 4 ? RGB(240, 210, 80) : RGB(240, 90, 80);
    SelectObject(dc, big);
    SetTextColor(dc, col);
    SetTextAlign(dc, TA_RIGHT | TA_BASELINE);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    int base = pad / 4 + tm.tmAscent;
    TextOutA(dc, pad + sn.cx, base, num, (int)strlen(num));
    SelectObject(dc, small);
    SetTextColor(dc, RGB(200, 200, 200));
    SetTextAlign(dc, TA_LEFT | TA_BASELINE);
    TextOutA(dc, pad + sn.cx + pad / 2, base, "fps", 3);
    GdiFlush();
    d3d_overlay(1, px, w, h);
}

static void show(void) {
    RECT rc;
    GetClientRect(wnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;
    fps_count();
    {   /* debug: RUGRATS_FLIPLOG=1 logs each present (real ms, display flips so far, vblank) */
        static int fl = -1;
        if (fl < 0) fl = getenv("RUGRATS_FLIPLOG") != NULL;
        if (fl) {
            extern uint32_t gpu_flips;
            static uint32_t flips;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            flips += gpu_flips; gpu_flips = 0;
            rt_logf("present t %.2f flips %u vbl %u cyc %u\n", (double)now.QuadPart * 1000.0 / (double)qpf.QuadPart, flips,
                    hooks_frame(), (unsigned)cpu.cycles);
        }
    }
    if (d3d_ok()) {
        osd_update(ch);
        fps_update(ch);
        int sync = vsync_interval(), tear = !opt.vsync;
        if (menu_open) {
            compose_menu(cw, ch);
            d3d_present(back_px, cw, ch, 0, 0, cw, ch, 0, sync, tear);
        } else if (direct_frame()) {
            int vx, vy, vw, vh, is24, off, x, y, w, h;
            gpu_display(&vx, &vy, &vw, &vh, &is24, &off);
            if (vw > 640) vw = 640;
            if (vh > 480) vh = 480;
            /* the previous frame when it is safe (workers finish it while the game runs on), else this one */
            int late;
            const uint16_t *v = gpu_present_hr(&vx, &vy, &vw, &vh, &late);
            int s = gpu_scale();
            frame_rect(cw, ch, &x, &y, &w, &h);
            const int sm = opt.smooth | D3D_AA(opt.aa);
            if (!v) hw_present(vx, vy, vw, vh, x, y, w, h, sm, sync, tear);
            else d3d_present16(v, 1024 * s, 512 * s, vx * s, vy * s, vw * s, vh * s, x, y, w, h, sm, sync, tear);
        } else {
            int x, y, w, h;
            frame_rect(cw, ch, &x, &y, &w, &h);
            d3d_present(fb, fb_w, fb_h, x, y, w, h, opt.smooth | D3D_AA(opt.aa), sync, tear);
        }
        if (d3d_ok()) return;
    }
    HDC dc = GetDC(wnd);
    paint(dc);
    ReleaseDC(wnd, dc);
}

/* ---- F1 options menu --------------------------------------------------
   The game is paused while it is open: host_present() does not return, so no guest code runs.
   To add a setting, add a row to menu_items[]. */
enum { MI_CHOICE, MI_ACTION };
typedef struct {
    const char *label;
    int kind;
    int *val;                       /* MI_CHOICE: the setting */
    const int *vals; int nvals;     /* allowed values, ascending */
    const char *const *names;       /* text per value, or NULL to use fmt / show */
    const char *fmt;                /* printf format for the value */
    void (*show)(int v, char *out, int n);
    const char *sec, *key;          /* ini location, NULL = not saved */
    void (*apply)(void);            /* called after a change (or the action for MI_ACTION) */
    const char *hint;
    int tab;                        /* page (T_*); T_ACTIONS rows show under every page */
    const char *section;            /* non-NULL: a sub-heading row with this text is drawn above the row */
} MenuItem;
enum { T_DISPLAY, T_VIDEO, T_AUDIO, T_GAME, T_ENH, T_DEBUG, T_COUNT, T_ACTIONS = T_COUNT };
static const char *const tab_names[T_COUNT] = { "Display", "Video", "Audio/Input", "Game", "Enhancements", "Debug" };
/* left edge of tab t (t = T_COUNT: right end) in a bar from a to b; widths follow the label lengths */
static int tab_edge(int t, int a, int b) {
    int tot = 0, cum = 0;
    for (int i = 0; i < T_COUNT; i++) { int w = (int)strlen(tab_names[i]) + 4; tot += w; if (i < t) cum += w; }
    return a + (b - a) * cum / tot;
}

static int want_fs;
static const char *const off_on[] = { "Off", "On" };
static const char *const finallevel_names[] = { "Default", "Enhanced" };
static const char *const shotwalk_names[] = { "Original", "Fixed" };
static const char *const subpx_names[] = { "Off", "Near only", "On" };
static const char *const filt_names[] = { "Sharp", "Smooth" };
static const char *const aa_names[] = { "Off", "Downsample", "Downsample + FXAA" };
static const int v_aa[] = { 0, 1, 2 };
static const char *const aniso_names[] = { "Off", "2x", "4x", "8x", "16x" };
static const int v_aniso[] = { 1, 2, 4, 8, 16 };
static const char *const msaa_names[] = { "Off", "2x", "4x", "8x" };
static const int v_msaa[] = { 1, 2, 4, 8 };
static const char *const rend_names[] = { "CPU", "GPU (D3D11)" };
static const char *const rcam_names[] = { "Off", "On", "On, auto-center" };
static const char *const rcam_inv_names[] = { "Normal", "Inverted" };
static const int v_rcam[] = { 0, 1, 2 };
static const char *const chatter_names[] = { "Original", "Less often", "Rarely", "Off" };
static const int v_chatter[] = { 0, 1, 2, 3 };
static const char *const viewext_names[] = { "Off", "Short", "Medium", "Full" };
static const char *const barsnd_names[] = { "Chime + roar", "Chime only", "Off" };
static const char *const dither_names[] = { "Original", "Off", "Off + full colour" };
static const int v_dither[] = { 1, 0, 2 };   /* ini values: 1 original, 0 off, 2 off + full colour */
static const int v_barsnd[] = { 1, 0, 2 };   /* ini values: 1 original, 0 chime only (old "roar off"), 2 none */
static const int v_golf[] = { 0, 25, 50, 75, 100, 150, 200 };
static void show_golf(int v, char *out, int n) {
    if (v <= 0) snprintf(out, n, "Uncorrected");
    else if (v == 100) snprintf(out, n, "100%% (PS1)");
    else snprintf(out, n, "%d%%", v);
}
static const int v_bool[] = { 0, 1 };
static const int v_scale[] = { 1, 2, 3, 4, 5, 6 };
static const int v_fps[] = { 60, 75, 100, 120, 144, 150, 240 };
static const int v_vol[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static const int v_oc[] = { 1, 2, 3, 4, 6, 8 };
static const int v_res[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };

static void show_scale(int v, char *out, int n) {
    int w = opt.widescreen ? 320 * v * 4 / 3 : 320 * v;
    snprintf(out, n, "%dx  (%d x %d)", v, w, 240 * v);
}
static void ap_window(void) { resize_window(); }
static void ap_fs(void) { if (want_fs != fullscreen) toggle_fullscreen(); opt.fullscreen = want_fs; }
static void ap_title(void) { update_title(); }
static void ap_res(void) { gpu_set_scale(opt.internal); opt.internal = gpu_scale(); }
static void show_res(int v, char *out, int n) {
    const char *tag = v == 9 ? "4K" : v == 6 ? "1440p" : v == 4 ? "~1080p" : NULL;   /* short: must fit the value box */
    if (tag) snprintf(out, n, "%dx  (%s)", v, tag);
    else snprintf(out, n, "%dx  (%d x %d)", v, 320 * v, 240 * v);
}
static void show_vsync(int v, char *out, int n) {
    if (!v) snprintf(out, n, "Off");
    else if (!d3d_ok()) snprintf(out, n, "On (needs D3D11)");
    else if (vsync_interval()) snprintf(out, n, "On (synced, %d Hz)", display_hz);
    else snprintf(out, n, "On (%d Hz display)", display_hz);
}
static void ac_resume(void) { menu_open = 0; }
static void ac_levels(void);
static void ac_mods(void);
static void ac_binds(void);
static void ap_texdump(void) { void tex_set_dump(int); tex_set_dump(opt.tex_dump); }
static void ac_qsave(void) { state_request(1, state_slot); menu_open = 0; pad_block = 1; }
static void ac_qload(void) { state_request(0, state_slot); menu_open = 0; pad_block = 1; }
static const int v_slot[] = { 1, 2, 3, 4 };
static void show_slot(int v, char *out, int n) { snprintf(out, n, "%d%s", v, state_slot_exists(v) ? "  (used)" : "  (empty)"); }
static void ac_quit(void) { confirm_open(1); }
/* Debug page: puzzle pieces / Reptar bars, read from game RAM when the menu opens, written on change (hooks.c
   hooks_dbg_*). Only once a level has run (hooks_level >= 1): before that the progress block is not the player's. */
extern int hooks_level;
int hooks_dbg_pieces(void), hooks_dbg_bars(void);
void hooks_dbg_set_pieces(int n), hooks_dbg_set_bars(int n);
static int dbg_pieces, dbg_bars;
extern int hooks_freecam;   /* Debug Freecam (hooks.c), not saved */
static int v_pieces[13], v_bars[100];
static int dbg_ingame(void) { return hooks_level >= 1; }
static void dbg_sync(void) {
    for (int i = 0; i < 13; i++) v_pieces[i] = i;
    for (int i = 0; i < 100; i++) v_bars[i] = i;
    dbg_pieces = dbg_ingame() ? hooks_dbg_pieces() : 0;
    dbg_bars = dbg_ingame() ? hooks_dbg_bars() : 0;
    if (dbg_bars > 99) dbg_bars = 99;
}
static void ap_pieces(void) { if (dbg_ingame()) hooks_dbg_set_pieces(dbg_pieces); dbg_sync(); }
static void ap_bars(void) { if (dbg_ingame()) hooks_dbg_set_bars(dbg_bars); dbg_sync(); }
static void show_pieces(int v, char *out, int n) {
    if (!dbg_ingame()) snprintf(out, n, "In game only"); else snprintf(out, n, "%d of 12", v);
}
static void show_bars(int v, char *out, int n) {
    if (!dbg_ingame()) snprintf(out, n, "In game only"); else snprintf(out, n, "%d", v);
}

static MenuItem menu_items[] = {
    { "Window size", MI_CHOICE, &opt.scale, v_scale, 6, NULL, NULL, show_scale, "video", "scale", ap_window,
      "Window size in windowed mode (multiple of the PS1's 320x240).", T_DISPLAY },
    { "Fullscreen", MI_CHOICE, &want_fs, v_bool, 2, off_on, NULL, NULL, "video", "fullscreen", ap_fs,
      "Borderless fullscreen. Also F11 or Alt+Enter.", T_DISPLAY },
    { "Widescreen", MI_CHOICE, &opt.widescreen, v_bool, 2, off_on, NULL, NULL, "video", "widescreen", ap_window,
      "16:9 view of the 3D world. Stretch 2D picks how menus, HUD and text are shown.", T_DISPLAY },
    { "Stretch 2D", MI_CHOICE, &opt.stretch2d, v_bool, 2, off_on, NULL, NULL, "video", "stretch2d", NULL,
      "On: menus, HUD and text fill the 16:9 screen (stretched). Off: they keep their original 4:3 shape, centred (GPU renderer).", T_DISPLAY },
    { "Frame rate", MI_CHOICE, &opt.fps, v_fps, 7, NULL, "%d fps", NULL, "video", "fps", ap_title,
      "Gameplay frame rate. 60, 75, 100 and 150 give perfectly even motion.", T_DISPLAY },
    { "VSync", MI_CHOICE, &opt.vsync, v_bool, 2, NULL, NULL, show_vsync, "video", "vsync", NULL,
      "On: no tearing; locks to the display when it runs at the frame rate. Off: lowest lag, G-Sync/FreeSync.", T_DISPLAY },
    { "Internal resolution", MI_CHOICE, &opt.internal, v_res, 9, NULL, NULL, show_res, "video", "internal", ap_res,
      "Sharper 3D and 2D. 4x is about 1080p, 6x 1440p, 9x 4K (2160 lines). Cheap on the GPU renderer; CPU renderer: above 4x needs a strong CPU.", T_VIDEO },
    { "Renderer", MI_CHOICE, &opt.renderer, v_bool, 2, rend_names, NULL, NULL, "video", "renderer", ap_res,
      "GPU: the graphics card draws the picture (fast at any resolution, 1x too). CPU: drawn on the processor (old path).", T_VIDEO },
    { "FPS counter", MI_CHOICE, &show_fps, v_bool, 2, off_on, NULL, NULL, "video", "fps_counter", NULL,
      "Shows the frames per second actually displayed, top-right. Keyboard: F3.", T_VIDEO },
    { "Filter", MI_CHOICE, &opt.smooth, v_bool, 2, filt_names, NULL, NULL, "video", "smooth", NULL,
      "Sharp keeps the original pixels. Smooth blurs them when scaled up.", T_VIDEO },
    { "Anti-aliasing", MI_CHOICE, &opt.aa, v_aa, 3, aa_names, NULL, NULL, "video", "aa", NULL,
      "Downsample: internal resolution above the window is averaged down (smooth edges; try 9x). FXAA also smooths edges (slight blur).", T_VIDEO },
    { "Anisotropic filtering", MI_CHOICE, &opt.aniso, v_aniso, 5, aniso_names, NULL, NULL, "video", "aniso", NULL,
      "Far and slanted floors and walls stop shimmering: squeezed textures are averaged. Close-up texels stay sharp. GPU renderer.", T_VIDEO },
    { "MSAA", MI_CHOICE, &opt.msaa, v_msaa, 4, msaa_names, NULL, NULL, "video", "msaa", ap_res,
      "Smooth polygon edges by multisampling (GPU renderer). Uses a lot of video memory at high internal resolution (4x at 9x: ~2 GB).", T_VIDEO },
    { "Dithering", MI_CHOICE, &opt.dither, v_dither, 3, dither_names, NULL, NULL, "video", "dither", NULL,
      "Removes the PS1's checkered dither pattern. Full colour also removes colour banding (GPU renderer).", T_VIDEO },
    { "Sub-pixel 3D", MI_CHOICE, &opt.subpixel, v_rcam, 3, subpx_names, NULL, NULL, "video", "subpixel", NULL,
      "Smooth 3D (no wobble). Near only: far scenery stays on whole pixels, so its seams stay shut. On: all. Off: original.", T_VIDEO },
    { "Frame overlap", MI_CHOICE, &opt.late_present, v_bool, 2, off_on, NULL, NULL, "video", "overlap", NULL,
      "On: at 2x-4x each frame is shown one frame later so the render threads keep working. More fps, +1 frame lag.", T_VIDEO,
      "Fixes" },
    { "Perspective textures", MI_CHOICE, &opt.perspective, v_bool, 2, off_on, NULL, NULL, "video", "perspective", NULL,
      "On: textures on walls and floors stay straight (no PS1 warping on big near walls). Needs Hide objects behind walls. Off: original.", T_VIDEO },
    { "Hide objects behind walls", MI_CHOICE, &opt.depthfix, v_bool, 2, off_on, NULL, NULL, "video", "depthfix", NULL,
      "Objects behind nearer walls or shelves stay hidden (the PS1's draw order can show them through). Off: original.", T_VIDEO },
    { "Fix doubled-face flicker", MI_CHOICE, &opt.dupfix, v_bool, 2, off_on, NULL, NULL, "video", "dupfix", NULL,
      "Some models draw a face twice; the second copy no longer flickers over parts drawn in between (toy store stands). Off: original.", T_VIDEO },
    { "Fix model sort glitches", MI_CHOICE, &opt.objsort, v_bool, 2, off_on, NULL, NULL, "video", "objsort", NULL,
      "Solid parts of a model hide its parts behind them (toy store box tops no longer get purple wedges). Needs Hide objects behind walls. Off: original.", T_VIDEO },
    { "Volume", MI_CHOICE, &opt.volume, v_vol, 11, NULL, "%d%%", NULL, "audio", "volume", NULL,
      "Master volume.", T_AUDIO, "Audio" },
    { "Music", MI_CHOICE, &opt.vol_music, v_vol, 11, NULL, "%d%%", NULL, "audio", "music", NULL,
      "Background music.", T_AUDIO },
    { "Sound effects", MI_CHOICE, &opt.vol_sfx, v_vol, 11, NULL, "%d%%", NULL, "audio", "sfx", NULL,
      "Effects and ambient sounds.", T_AUDIO },
    { "Dialogue", MI_CHOICE, &opt.vol_voice, v_vol, 11, NULL, "%d%%", NULL, "audio", "dialogue", NULL,
      "Spoken lines in play and in-game cutscenes. Movies follow only the master volume.", T_AUDIO },
    { "Chatter Frequency", MI_CHOICE, &opt.chatter, v_chatter, 4, chatter_names, NULL, NULL, "audio", "chatter", NULL,
      "How often the babies say random lines while you play. Cutscenes are not affected.", T_ENH },
    { "Restored Lines", MI_CHOICE, &opt.voice_restore, v_bool, 2, off_on, NULL, NULL, "audio", "voice_restore", NULL,
      "On: lines the game recorded but never plays come back: in random chatter (unheard first, then repeating), the deli clerks in the pie gallery, Phil's trike target landing.", T_ENH },
    { "Unused Hub Lines", MI_CHOICE, &opt.voice_hub, v_bool, 2, off_on, NULL, NULL, "audio", "voice_hub", NULL,
      "On: in the house between levels, Tommy now and then says one of his unused lines (needs Restored Lines On).", T_ENH },
    { "Golf Reactions", MI_CHOICE, &opt.voice_golf, v_bool, 2, off_on, NULL, NULL, "audio", "voice_golf", NULL,
      "On: mini golf babies react to hole in one, birdie, bogey and over par, and sometimes while the ball rolls. Off: original.", T_ENH },
    { "Reptar Bar Sound", MI_CHOICE, &opt.bar_roar, v_barsnd, 3, barsnd_names, NULL, NULL, "audio", "bar_roar", NULL,
      "The sound when you pick up a Reptar bar: the original chime and roar, the chime alone, or nothing.", T_ENH },
    { "Note Fade-out", MI_CHOICE, &opt.note_fade, v_bool, 2, off_on, NULL, NULL, "audio", "note_fade", NULL,
      "On: music notes the game cuts off instantly fade out over a few milliseconds instead (removes the clicks).", T_ENH },
    { "Extended View Distance", MI_CHOICE, &opt.view_ext, v_chatter, 4, viewext_names, NULL, NULL, "video", "view_ext", NULL,
      "Hedge maze and Reptar 2010: how far rooms seen over walls are drawn. Lower = fewer polygons, faster; "
      "far parts can then be missing. Off: original.", T_ENH },
    { "Smooth Cutscenes", MI_CHOICE, &opt.cut_smooth, v_bool, 2, off_on, NULL, NULL, "video", "cut_smooth", NULL,
      "On: in-engine cutscenes run at your frame rate (motion blended between the disc's 18.75 steps per second; the picture is ~1/20 s behind the sound). Off: original 18.75 fps.", T_ENH },
    { "Override Camera", MI_CHOICE, &opt.rcam_override, v_bool, 2, off_on, NULL, NULL, "input", "rcam_override", NULL,
      "On: the right-stick camera also works in levels where it is normally off (Touchdown Tommy).", T_ENH },
    { "Wall Bonk", MI_CHOICE, &opt.wall_bonk, v_bool, 2, off_on, NULL, NULL, "game", "wall_bonk", NULL,
      "On: walking into a wall bonks (sound, short loss of control). Off: you just stop at the wall.", T_ENH },
    { "Walk After Shooting", MI_CHOICE, &opt.shot_walk, v_bool, 2, shotwalk_names, NULL, NULL, "game", "shot_walk", NULL,
      "Fixed: after firing while walking (Angelica's laser), the walk animation goes back to normal speed. "
      "Original: it keeps playing at double speed until you stop.", T_ENH },
    { "\"Let There Be Light\" Fixes", MI_CHOICE, &opt.light_fixes, v_bool, 2, shotwalk_names, NULL, NULL, "game", "light_fixes", NULL,
      "Fixed: the power is out, so the kitchen ceiling fan stands still and various objects that kept their daytime "
      "colours get night colours; hedges shed leaves instead of stray markers. Original: as on the disc.", T_ENH },
    { "Final Level Behavior", MI_CHOICE, &opt.final_level, v_bool, 2, finallevel_names, NULL, NULL, "game", "final_level", NULL,
      "Enhanced: after the last puzzle piece you stay in the level; the TV in the house (Triangle) starts Reptar 2010; "
      "after the credits you keep your pieces and bars. Default: straight on to the final level.", T_ENH },
    { "Level Select...", MI_ACTION, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, ac_levels,  /* last on its page */
      "Jump to any level (during gameplay). The current level is left as if through a door.", T_ENH },
    { "Right-stick camera", MI_CHOICE, &opt.rcam, v_rcam, 3, rcam_names, NULL, NULL, "input", "rstick_camera", NULL,
      "Right stick (or J/L/I/K) turns the camera around the player. Auto-center: drifts back behind when released.", T_AUDIO, "Input" },
    { "Camera up/down", MI_CHOICE, &opt.rcam_invert, v_bool, 2, rcam_inv_names, NULL, NULL, "input", "rstick_invert", NULL,
      "Normal: stick up looks up. Inverted: stick up looks down.", T_AUDIO },
    { "Control Bindings...", MI_ACTION, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, ac_binds,  /* last on its page */
      "Choose the keyboard key and controller button for each PS1 button.", T_AUDIO },
    { "Real-time game step", MI_CHOICE, &opt.fixed_timestep, v_bool, 2, off_on, NULL, NULL, "video",
      "realtime_step", NULL, "Game time follows real time. Needed for correct speed above 60 fps.", T_GAME },
    { "CPU overclock", MI_CHOICE, &opt.overclock, v_oc, 6, NULL, "%dx", NULL, "video", "overclock", NULL,
      "Virtual PS1 CPU speed. Takes effect after a restart.", T_GAME },
    { "Golf aim speed", MI_CHOICE, &opt.golf_aim, v_golf, 7, NULL, NULL, show_golf, "game", "golf_aim", NULL,
      "How fast left/right turns the aim in the golf holes, same at any frame rate. Uncorrected: faster at higher fps.", T_GAME },
    { "Skip intro", MI_CHOICE, &opt.skip_intro, v_bool, 2, off_on, NULL, NULL, "game", "skip_intro", NULL,
      "Boot straight to the title: skips the legal screen and logo movies. From the next start.", T_GAME },
    { "Discord status", MI_CHOICE, &opt.discord, v_bool, 2, off_on, NULL, NULL, "discord", "enabled", NULL,
      "Shows the game and the current level in your Discord profile (Discord desktop app must be running).", T_GAME },
    { "Save slot", MI_CHOICE, &state_slot, v_slot, 4, NULL, NULL, show_slot, "game", "slot", NULL,
      "Slot for quick save / quick load. Keyboard: F6 next slot.", T_GAME },
    { "Dump textures", MI_CHOICE, &opt.tex_dump, v_bool, 2, off_on, NULL, NULL, "textures", "dump", ap_texdump,
      "For texture pack makers: every texture the game draws is saved once as a PNG named for packs, in "
      "textures_dump next to the game (GPU renderer). See MODDING.md.", T_GAME },
    { "Mods...", MI_ACTION, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, ac_mods,  /* last on its page */
      "Turn installed mods on or off (cheats and RAM patches take effect right away). Add mods with the launcher.", T_GAME },
    { "Puzzle pieces", MI_CHOICE, &dbg_pieces, v_pieces, 13, NULL, NULL, show_pieces, NULL, NULL, ap_pieces,
      "Pieces you have earned. Fewer: unplaced pieces go first. More: new pieces wait in the puzzle screen. 12 opens the "
      "final level. Changes your progress (saved with the next memory card save).", T_DEBUG },
    { "Reptar bars", MI_CHOICE, &dbg_bars, v_bars, 100, NULL, NULL, show_bars, NULL, NULL, ap_bars,
      "Bars you are holding. No bar disappears from a level. At 15 or more the next bar you pick up earns a puzzle "
      "piece (under 12 pieces).", T_DEBUG },
    { "Freecam", MI_CHOICE, &hooks_freecam, v_bool, 2, off_on, NULL, NULL, NULL, NULL, NULL,
      "Detach the camera and fly through the level (the player stands still). Move: d-pad / left stick, look: right "
      "stick or J L I K, up / down: R1 / L1, hold Cross: fast, Square: slow. Gameplay only.", T_DEBUG },
    { "Quick save", MI_ACTION, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, ac_qsave,
      "Save the game right here (during gameplay). Keyboard: F5.", T_ACTIONS },
    { "Quick load", MI_ACTION, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, ac_qload,
      "Go back to the quick save in this slot. Keyboard: F7.", T_ACTIONS },
    { "Resume", MI_ACTION, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, ac_resume, "Back to the game.", T_ACTIONS },
    { "Quit game", MI_ACTION, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, ac_quit, "Close Rugrats.", T_ACTIONS },
};
#define MENU_N ((int)(sizeof menu_items / sizeof menu_items[0]))

static int menu_sel;                 /* index into menu_vis[] */
static int menu_tab;                 /* current page, kept between openings */
static int menu_vis[MENU_N], menu_nvis, menu_nact; /* rows shown: the page's rows, then the actions */
static int lay_x0, lay_x1, lay_y0, lay_rowh, lay_vx0, lay_vx1; /* last drawn layout, for the mouse */
static int lay_ty0, lay_ty1;         /* tab bar */

/* rows on a page / actions: the panel is sized for the longest page so it doesn't jump when switching */
/* an action on a page (Level Select...) gets an empty row above it; a row with a section gets its heading row */
static int item_spaced(const MenuItem *it) { return (it->kind == MI_ACTION && it->tab < T_COUNT) || it->section; }
static int tab_rows(int t) {
    int n = 0;
    for (int i = 0; i < MENU_N; i++) n += menu_items[i].tab == t ? 1 + item_spaced(&menu_items[i]) : 0;
    return n;
}
static void menu_build(void) {
    menu_nvis = menu_nact = 0;
    for (int i = 0; i < MENU_N; i++) if (menu_items[i].tab == menu_tab) menu_vis[menu_nvis++] = i;
    for (int i = 0; i < MENU_N; i++) if (menu_items[i].tab == T_ACTIONS) { menu_vis[menu_nvis++] = i; menu_nact++; }
    if (menu_sel >= menu_nvis) menu_sel = menu_nvis - 1;
    if (menu_sel < 0) menu_sel = 0;
}
static int menu_max_rows(void) {
    int m = 0;
    for (int t = 0; t < T_COUNT; t++) if (tab_rows(t) > m) m = tab_rows(t);
    return m + tab_rows(T_ACTIONS);
}
/* screen row of shown item i: page rows top down (plus spacer rows), actions pinned to the bottom */
static int menu_slot(int i) {
    int page = menu_nvis - menu_nact;
    if (i >= page) return menu_max_rows() - menu_nact + (i - page);
    int s = i;
    for (int k = 0; k <= i; k++) s += item_spaced(&menu_items[menu_vis[k]]);
    return s;
}
static void menu_set_tab(int t) {
    int on_action = menu_sel >= menu_nvis - menu_nact, a = menu_sel - (menu_nvis - menu_nact);
    menu_tab = (t + T_COUNT) % T_COUNT;
    menu_build();
    menu_sel = on_action ? menu_nvis - menu_nact + a : 0; /* stay on the same action, else first row */
}

static int item_index(const MenuItem *it) {
    for (int i = 0; i < it->nvals; i++)   /* exact value first (lists need not be sorted, e.g. v_barsnd) */
        if (it->vals[i] == *it->val) return i;
    for (int i = 0; i < it->nvals; i++)
        if (it->vals[i] >= *it->val) return i;
    return it->nvals - 1;
}

static void item_value_text(const MenuItem *it, char *out, int n) {
    int v = *it->val;
    if (it->names) snprintf(out, n, "%s", it->names[item_index(it)]);
    else if (it->show) it->show(v, out, n);
    else snprintf(out, n, it->fmt ? it->fmt : "%d", v);
}

static void item_change(int dir, int wrap) {
    MenuItem *it = &menu_items[menu_vis[menu_sel]];
    if (it->kind != MI_CHOICE || !dir) return;
    int i = item_index(it), old = *it->val;
    if (dir > 0 && it->vals[i] > old) { /* off-list value from the ini: snap up first */
    } else {
        i += dir;
        if (i < 0) i = wrap ? it->nvals - 1 : 0;
        if (i >= it->nvals) i = wrap ? 0 : it->nvals - 1;
    }
    *it->val = it->vals[i];
    if (*it->val != old && it->apply) it->apply();
}

static void item_activate(void) {
    MenuItem *it = &menu_items[menu_vis[menu_sel]];
    if (it->kind == MI_ACTION) { if (it->apply) it->apply(); }
    else item_change(+1, 1);
}

static void menu_move(int d) { menu_sel = (menu_sel + d + menu_nvis) % menu_nvis; }

/* Quit prompt (Esc in game, or the menu's Quit game row). Shown by the menu's modal loop, so the game is paused.
   No is selected first. Yes: Enter/Space/Z/A/Cross on Yes, or the Y key. No: Esc, Backspace, X, N, B/Circle. */
static void confirm_open(int from_menu) {
    confirm_quit = 1;
    confirm_from_menu = from_menu;
    confirm_sel = 0;
    menu_open = 1;
}
static void confirm_close(int yes) {
    confirm_quit = 0;
    if (yes) { quit = 1; menu_open = 0; }
    else if (!confirm_from_menu) menu_open = 0; /* back to the game */
}
static void confirm_key(int vk) {
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_TAB: case VK_UP: case VK_DOWN: confirm_sel = !confirm_sel; break;
    case VK_RETURN: case VK_SPACE: case 'Z': confirm_close(confirm_sel); break;
    case 'Y': confirm_close(1); break;
    case VK_ESCAPE: case VK_BACK: case 'X': case 'N': confirm_close(0); break;
    }
}

/* Level Select (Enhancements row): a list of the playable levels over the options panel. Going there sets the game's
   own "next level" variable (hooks_level_request), so the current level ends as if through a door and the chosen one
   loads with its normal intro. Level numbers are the game's databases (0x800C5678); left out: 0 menus, 8 Ice Cream
   Mountain (its level file can't be parsed), 19 Be Bad (file missing on the disc), 22 Multiplayer Golf (needs the main
   menu's player setup); see README "Per-level performance test". */
int hooks_gameplay(void);
void hooks_level_request(int db);
static HFONT font_get(HFONT *cache, int *cache_h, int h, int weight);
static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c);
static void text(HDC dc, const char *s, int x0, int y0, int x1, int y1, COLORREF c, UINT align);
/* Look shared by the options panel, level select and quit prompt (same palette as recomp/launcher.py). */
#define UI_BG        RGB(21, 17, 29)
#define UI_FIELD     RGB(35, 28, 49)
#define UI_BORDER    RGB(61, 49, 88)
#define UI_TEXT      RGB(236, 232, 245)
#define UI_MUTED     RGB(165, 155, 189)
#define UI_ACCENT    RGB(139, 92, 246)
#define UI_ACCENT_LO RGB(113, 64, 224)
#define UI_SOFT      RGB(196, 176, 255)
#define UI_WHITE     RGB(255, 255, 255)
static void ui_panel(HDC dc, int x0, int y0, int pw, int ph, int rowh, const char *title);
extern int hooks_level;
static const struct { int db; const char *name; } lvl_list[] = {
    { 1, "Pickles' Home (hub)" }, { 2, "Grandpa's Teeth" }, { 3, "Incident on Aisle 7" }, { 4, "7 Voyages of Cynthia" },
    { 5, "Visitors from Outer Space" }, { 6, "Toy Palace" }, { 7, "Reptar 2010" }, { 9, "The Mysterious Mr. Friend" },
    { 10, "Chuckie's Glasses" }, { 11, "Circus Angelicus" }, { 12, "Let There Be Light" }, { 13, "Cookie Race" },
    { 14, "The Return of Mr. Friend" }, { 15, "Mirrorland" }, { 16, "Training Level" }, { 17, "Gold Rush" },
    { 18, "Egg Hunt" }, { 19, "Be Bad (cut level)" }, { 20, "No More Cookies" }, { 21, "Touchdown Tommy" }, { 23, "Golf: hole 1" },
    { 24, "Golf: hole 2" }, { 25, "Golf: hole 3" }, { 26, "Golf: hole 4" }, { 27, "Golf: hole 5" },
    { 28, "Golf: hole 6" }, { 29, "Golf: hole 7" }, { 30, "Golf: hole 8" }, { 31, "Golf: hole 9" },
};
#define LVL_N ((int)(sizeof lvl_list / sizeof lvl_list[0]))
#define LVL_VIS 12                           /* rows shown at once */
static int lvl_open, lvl_sel, lvl_top;
static const char *lvl_err;                  /* shown in the panel when going is not possible right now */
static int lv_x0, lv_x1, lv_y0, lv_rowh;     /* last drawn list layout, for the mouse */

static void ac_levels(void) {
    lvl_open = 1;
    lvl_err = NULL;
    lvl_sel = 0;
    for (int i = 0; i < LVL_N; i++) if (lvl_list[i].db == hooks_level) lvl_sel = i; /* start on the current level */
    lvl_top = lvl_sel - LVL_VIS / 2;
}
static void lvl_clamp(void) {
    if (lvl_sel < 0) lvl_sel = 0;
    if (lvl_sel >= LVL_N) lvl_sel = LVL_N - 1;
    if (lvl_top > lvl_sel) lvl_top = lvl_sel;
    if (lvl_top < lvl_sel - LVL_VIS + 1) lvl_top = lvl_sel - LVL_VIS + 1;
    if (lvl_top > LVL_N - LVL_VIS) lvl_top = LVL_N - LVL_VIS;
    if (lvl_top < 0) lvl_top = 0;
}
static void lvl_go(void) {
    if (!hooks_gameplay()) { lvl_err = "Start or load a game first (works during gameplay only)."; return; }
    hooks_level_request(lvl_list[lvl_sel].db);
    snprintf(state_msg, sizeof state_msg, "Going to %s", lvl_list[lvl_sel].name);
    state_msg_time = GetTickCount();
    lvl_open = 0;
    menu_open = 0;
    pad_block = 1;
}
static void lvl_move(int d, int wrap) {
    lvl_err = NULL;
    lvl_sel += d;
    if (wrap) lvl_sel = (lvl_sel % LVL_N + LVL_N) % LVL_N;
    lvl_clamp();
}
static void lvl_key(int vk) {
    switch (vk) {
    case VK_UP: lvl_move(-1, 1); break;
    case VK_DOWN: lvl_move(+1, 1); break;
    case VK_LEFT: case VK_PRIOR: case 'Q': lvl_move(-LVL_VIS, 0); break;
    case VK_RIGHT: case VK_NEXT: case 'E': lvl_move(+LVL_VIS, 0); break;
    case VK_HOME: lvl_move(-LVL_N, 0); break;
    case VK_END: lvl_move(+LVL_N, 0); break;
    case VK_RETURN: case VK_SPACE: case 'Z': lvl_go(); break;
    case VK_ESCAPE: case VK_BACK: case 'X': lvl_open = 0; break;   /* back to the options */
    }
}
static void lvl_mouse(int x, int y, int button, int wheel) {
    if (button == 2) { lvl_open = 0; return; }
    if (wheel) { lvl_top -= wheel * 3; if (lvl_top < 0) lvl_top = 0; if (lvl_top > LVL_N - LVL_VIS) lvl_top = LVL_N - LVL_VIS;
                 if (lvl_sel < lvl_top) lvl_sel = lvl_top; if (lvl_sel >= lvl_top + LVL_VIS) lvl_sel = lvl_top + LVL_VIS - 1; return; }
    if (!lv_rowh || x < lv_x0 || x >= lv_x1 || y < lv_y0) return;
    int row = (y - lv_y0) / lv_rowh;
    if (row >= LVL_VIS || lvl_top + row >= LVL_N) return;
    lvl_sel = lvl_top + row;
    lvl_err = NULL;
    if (button == 1) lvl_go();
}
static void lvl_draw(HDC dc, int cw, int ch) {
    static HFONT f_big, f_row, f_small;
    static int h_big, h_row, h_small;
    const COLORREF accent = UI_ACCENT, fg = UI_TEXT, val = UI_SOFT, dim = UI_MUTED, dark = UI_WHITE;
    lvl_clamp();
    int rowh = ch / (LVL_VIS + 9);
    if (rowh > cw / 22) rowh = cw / 22;
    if (rowh < 14) rowh = 14;
    int pw = rowh * 15, ph = rowh * (LVL_VIS + 5);
    if (pw > cw - 8) pw = cw - 8;
    int x0 = (cw - pw) / 2, y0 = (ch - ph) / 2, pad = rowh / 2;
    SelectObject(dc, font_get(&f_big, &h_big, rowh * 3 / 4, FW_BOLD));
    ui_panel(dc, x0, y0, pw, ph, rowh, "Level Select");
    int ry = y0 + rowh * 2;
    lv_x0 = x0; lv_x1 = x0 + pw; lv_y0 = ry; lv_rowh = rowh;
    SelectObject(dc, font_get(&f_row, &h_row, rowh * 11 / 20, FW_SEMIBOLD));
    for (int r = 0; r < LVL_VIS && lvl_top + r < LVL_N; r++) {
        int i = lvl_top + r, y = ry + r * rowh, sel = i == lvl_sel;
        char num[16];
        if (sel) fill(dc, x0 + pad / 2, y + 1, x0 + pw - pad / 2, y + rowh - 1, accent);
        snprintf(num, sizeof num, "%d", lvl_list[i].db);
        text(dc, num, x0 + pad, y, x0 + pad + rowh * 3 / 2, y + rowh, sel ? dark : dim, DT_RIGHT);
        text(dc, lvl_list[i].name, x0 + pad + rowh * 2, y, x0 + pw - pad, y + rowh, sel ? dark : fg, DT_LEFT);
        if (lvl_list[i].db == hooks_level) text(dc, "current", x0 + pad, y, x0 + pw - pad, y + rowh, sel ? dark : val, DT_RIGHT);
    }
    SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
    int sy0 = ry, sy1 = ry + LVL_VIS * rowh;   /* scroll position, a thin bar on the right edge */
    int b0 = sy0 + (sy1 - sy0) * lvl_top / LVL_N, b1 = sy0 + (sy1 - sy0) * (lvl_top + LVL_VIS) / LVL_N;
    fill(dc, x0 + pw - pad / 3 - 2, b0, x0 + pw - pad / 3, b1, dim);
    int hy = sy1 + rowh / 4;
    if (lvl_err) text(dc, lvl_err, x0 + pad, hy, x0 + pw - pad, hy + rowh * 3 / 4, UI_WHITE, DT_CENTER);
    else text(dc, "The level you leave is not saved.", x0 + pad, hy, x0 + pw - pad, hy + rowh * 3 / 4, val, DT_CENTER);
    text(dc, "Up/Down select   Left/Right page   Enter / A / Cross go", x0 + pad, hy + rowh * 3 / 4, x0 + pw - pad,
         hy + rowh * 3 / 2, dim, DT_CENTER);
    text(dc, "Esc / B / Circle back   Mouse: click a level, wheel scrolls", x0 + pad, hy + rowh * 3 / 2,
         x0 + pw - pad, hy + rowh * 9 / 4, dim, DT_CENTER);
}

/* Mods (Game page row "Mods..."): installed mods from mods.c with an on/off switch each. Phase 1 mods are RAM patches,
   so a switch takes effect at the next vblank; the enabled list is written to the ini when this list closes. */
#define MOD_VIS 9
static int mod_open, mod_sel, mod_top, mod_dirty;
static int mv_x0, mv_x1, mv_y0, mv_rowh;
static void ac_mods(void) { mod_open = 1; mod_sel = 0; mod_top = 0; }
static void mod_close(void) {
    mod_open = 0;
    if (mod_dirty && (!test_frames || getenv("RUGRATS_MENU_SAVE"))) mods_save();
    mod_dirty = 0;
    update_title();
}
static void mod_clamp(void) {
    int n = mods_count();
    if (mod_sel >= n) mod_sel = n - 1;
    if (mod_sel < 0) mod_sel = 0;
    if (mod_top > mod_sel) mod_top = mod_sel;
    if (mod_top < mod_sel - MOD_VIS + 1) mod_top = mod_sel - MOD_VIS + 1;
    if (mod_top < 0) mod_top = 0;
}
static void mod_flip(void) { if (mods_count() && mods_toggle(mod_sel)) { mod_dirty = 1; update_title(); } }
static void mod_key(int vk) {
    int n = mods_count();
    switch (vk) {
    case VK_UP: if (n) mod_sel = (mod_sel - 1 + n) % n; break;
    case VK_DOWN: if (n) mod_sel = (mod_sel + 1) % n; break;
    case VK_PRIOR: mod_sel -= MOD_VIS; break;
    case VK_NEXT: mod_sel += MOD_VIS; break;
    case VK_LEFT: case VK_RIGHT: case VK_RETURN: case VK_SPACE: case 'Z': mod_flip(); break;
    case VK_ESCAPE: case VK_BACK: case 'X': mod_close(); break;
    }
    mod_clamp();
}
static void mod_mouse(int x, int y, int button, int wheel) {
    if (button == 2) { mod_close(); return; }
    if (wheel) { mod_sel -= wheel; mod_clamp(); return; }
    if (!mv_rowh || x < mv_x0 || x >= mv_x1 || y < mv_y0) return;
    int row = (y - mv_y0) / mv_rowh;
    if (row >= MOD_VIS || mod_top + row >= mods_count()) return;
    mod_sel = mod_top + row;
    if (button == 1) mod_flip();
}
static void mod_draw(HDC dc, int cw, int ch) {
    static HFONT f_big, f_row, f_small;
    static int h_big, h_row, h_small;
    const COLORREF accent = UI_ACCENT, fg = UI_TEXT, val = UI_SOFT, dim = UI_MUTED, dark = UI_WHITE;
    mod_clamp();
    int n = mods_count();
    int rowh = ch / (MOD_VIS + 13);
    if (rowh > cw / 22) rowh = cw / 22;
    if (rowh < 14) rowh = 14;
    int pw = rowh * 17, ph = rowh * (MOD_VIS + 8);
    if (pw > cw - 8) pw = cw - 8;
    int x0 = (cw - pw) / 2, y0 = (ch - ph) / 2, pad = rowh / 2;
    SelectObject(dc, font_get(&f_big, &h_big, rowh * 3 / 4, FW_BOLD));
    ui_panel(dc, x0, y0, pw, ph, rowh, "Mods");
    int ry = y0 + rowh * 2;
    mv_x0 = x0; mv_x1 = x0 + pw; mv_y0 = ry; mv_rowh = rowh;
    SelectObject(dc, font_get(&f_row, &h_row, rowh * 11 / 20, FW_SEMIBOLD));
    if (!n) {
        text(dc, "No mods installed.", x0 + pad, ry, x0 + pw - pad, ry + rowh, fg, DT_CENTER);
        SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
        text(dc, "Add them with the launcher's Mods page, or put a mod folder in:", x0 + pad, ry + rowh,
             x0 + pw - pad, ry + rowh * 2, dim, DT_CENTER);
        text(dc, mods_dir(), x0 + pad, ry + rowh * 2, x0 + pw - pad, ry + rowh * 3, val, DT_CENTER | DT_PATH_ELLIPSIS);
    }
    for (int r = 0; r < MOD_VIS && mod_top + r < n; r++) {
        int i = mod_top + r, y = ry + r * rowh, sel = i == mod_sel, bad = mods_error(i) != NULL;
        if (sel) fill(dc, x0 + pad / 2, y + 1, x0 + pw - pad / 2, y + rowh - 1, accent);
        const char *st = bad ? "Error" : mods_enabled(i) ? "On" : "Off";
        text(dc, st, x0 + pad, y, x0 + pad + rowh * 2, y + rowh, sel ? dark : bad ? dim : mods_enabled(i) ? val : dim, DT_LEFT);
        text(dc, mods_name(i), x0 + pad + rowh * 2, y, x0 + pw - pad - rowh * 3, y + rowh, sel ? dark : bad ? dim : fg,
             DT_LEFT | DT_END_ELLIPSIS);
        text(dc, mods_version(i), x0 + pw - pad - rowh * 3, y, x0 + pw - pad, y + rowh, sel ? dark : dim, DT_RIGHT);
    }
    if (n > MOD_VIS) {
        int sy0 = ry, sy1 = ry + MOD_VIS * rowh;
        int b0 = sy0 + (sy1 - sy0) * mod_top / n, b1 = sy0 + (sy1 - sy0) * (mod_top + MOD_VIS) / n;
        fill(dc, x0 + pw - pad / 3 - 2, b0, x0 + pw - pad / 3, b1, dim);
    }
    /* details of the selected mod: a card like the options' description box */
    int dy0 = ry + MOD_VIS * rowh + rowh / 4, dy1 = dy0 + rowh * 3;
    SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
    if (n) {
        char head[200], sm[200];
        fill(dc, x0 + pad / 2, dy0, x0 + pw - pad / 2, dy1, UI_FIELD);
        fill(dc, x0 + pad / 2, dy0, x0 + pad / 2 + (rowh / 12 < 2 ? 2 : rowh / 12), dy1, accent);
        snprintf(head, sizeof head, "%s%s%s", mods_author(mod_sel)[0] ? "by " : "", mods_author(mod_sel),
                 mods_author(mod_sel)[0] ? "   " : "");
        mods_summary(mod_sel, sm, sizeof sm);
        strncat(head, sm, sizeof head - strlen(head) - 1);
        text(dc, head, x0 + pad, dy0, x0 + pw - pad, dy0 + rowh * 3 / 4, mods_error(mod_sel) ? UI_WHITE : val,
             DT_LEFT | DT_END_ELLIPSIS);
        RECT r = { x0 + pad, dy0 + rowh * 3 / 4, x0 + pw - pad, dy1 - rowh / 8 };
        SetTextColor(dc, fg);
        DrawTextA(dc, mods_desc(mod_sel), -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    int hy = dy1 + rowh / 6;
    text(dc, "Up/Down select   Enter / A / Cross on/off   Esc / B / Circle back", x0 + pad, hy, x0 + pw - pad,
         hy + rowh * 3 / 4, dim, DT_CENTER);
    text(dc, "Mouse: click switches a mod. Load order and adding mods: launcher.", x0 + pad, hy + rowh * 3 / 4,
         x0 + pw - pad, hy + rowh * 3 / 2, dim, DT_CENTER);
}

/* Control Bindings (Audio/Input page row): one row per PS1 button with a Keyboard and a Controller cell, then
   "Reset to defaults". Enter / A / click on a cell waits for the new key or button; Esc (or L3 / R3 / PS) cancels.
   Written to the ini when the page closes. */
#define BND_ROWS (BA_N + 1)                  /* actions + reset */
static int bnd_open, bnd_sel, bnd_col, bnd_dirty;
static const char *bnd_msg;                  /* short note in the help card (refused key, reset done) */
static int bv_x0, bv_x1, bv_y0, bv_rowh, bv_c1, bv_c2; /* last drawn layout, for the mouse */
static uint32_t bnd_pad_prev;
static void ac_binds(void) { bnd_open = 1; bnd_sel = 0; bnd_col = 0; bnd_wait = 0; bnd_msg = NULL; }
static void bnd_close(void) {
    bnd_open = 0; bnd_wait = 0;
    if (bnd_dirty && (!test_frames || getenv("RUGRATS_MENU_SAVE"))) bind_save(ini_path);
    bnd_dirty = 0;
}
static int bnd_row_slot(int r) { return r + (r == BA_N); }   /* empty row above Reset */
static void bnd_activate(void) {
    bnd_msg = NULL;
    if (bnd_sel == BA_N) { bind_defaults(); bnd_dirty = 1; bnd_msg = "All keys and buttons are back to the defaults."; return; }
    bnd_wait = bnd_col ? 2 : 1;
    bnd_pad_prev = pad_phys(0);              /* only a fresh press counts */
}
static void bnd_key(int vk) {
    if (bnd_wait == 1) {
        if (vk == VK_ESCAPE) { bnd_wait = 0; return; }
        if (bind_key_reserved(vk)) { bnd_msg = "That key is reserved (Esc, Tab, F1-F12, J/L/I/K camera). Try another."; return; }
        bind_set_key(bnd_sel, vk);
        bnd_wait = 0; bnd_dirty = 1; bnd_msg = NULL;
        return;
    }
    if (bnd_wait == 2) { if (vk == VK_ESCAPE) bnd_wait = 0; return; }
    switch (vk) {
    case VK_UP: bnd_sel = (bnd_sel - 1 + BND_ROWS) % BND_ROWS; bnd_msg = NULL; break;
    case VK_DOWN: bnd_sel = (bnd_sel + 1) % BND_ROWS; bnd_msg = NULL; break;
    case VK_LEFT: bnd_col = 0; break;
    case VK_RIGHT: bnd_col = 1; break;
    case VK_RETURN: case VK_SPACE: case 'Z': bnd_activate(); break;
    case VK_ESCAPE: case VK_BACK: case 'X': bnd_close(); break;
    }
}
/* controller while the page is open; down = XInput-style edges from menu_pad */
static void bnd_pad(WORD down) {
    uint32_t p = pad_phys(0), pd = p & ~bnd_pad_prev;
    bnd_pad_prev = p;
    if (bnd_wait) {
        if (down & XIN_TOGGLE) { bnd_wait = 0; return; }   /* L3 / R3 / PS cancel */
        if (bnd_wait == 2 && pd) {
            int pb = 0;
            while (!(pd & (1u << pb))) pb++;
            bind_set_pad(bnd_sel, pb);
            bnd_wait = 0; bnd_dirty = 1; bnd_msg = NULL;
        }
        return;
    }
    if (down & XINPUT_GAMEPAD_DPAD_UP) bnd_key(VK_UP);
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) bnd_key(VK_DOWN);
    if (down & XINPUT_GAMEPAD_DPAD_LEFT) bnd_key(VK_LEFT);
    if (down & XINPUT_GAMEPAD_DPAD_RIGHT) bnd_key(VK_RIGHT);
    if (down & XINPUT_GAMEPAD_A) bnd_activate();
    else if (down & XINPUT_GAMEPAD_B) bnd_close();                       /* back to the options */
    else if (down & (XINPUT_GAMEPAD_START | XIN_TOGGLE)) { bnd_close(); menu_open = 0; }
}
static void bnd_mouse(int x, int y, int button, int wheel) {
    if (bnd_wait) { if (button) bnd_wait = 0; return; }       /* a click cancels the wait */
    if (button == 2) { bnd_close(); return; }
    if (wheel) { bnd_key(wheel > 0 ? VK_UP : VK_DOWN); return; }
    if (!bv_rowh || x < bv_x0 || x >= bv_x1 || y < bv_y0) return;
    int slot = (y - bv_y0) / bv_rowh, row = -1;
    for (int r = 0; r < BND_ROWS; r++) if (bnd_row_slot(r) == slot) row = r;
    if (row < 0) return;
    bnd_sel = row;
    if (row < BA_N) { if (x < bv_c1) return; bnd_col = x >= bv_c2; }
    if (button == 1) bnd_activate();
}
static void bnd_draw(HDC dc, int cw, int ch) {
    static HFONT f_big, f_row, f_small, f_hint;
    static int h_big, h_row, h_small, h_hint;
    const COLORREF accent = UI_ACCENT, fg = UI_TEXT, val = UI_SOFT, dim = UI_MUTED, dark = UI_WHITE;
    int rowh = ch / (BND_ROWS + 13);
    if (rowh > cw / 22) rowh = cw / 22;
    if (rowh < 14) rowh = 14;
    int pw = rowh * 19, ph = rowh * (BND_ROWS + 9);
    if (pw > cw - 8) pw = cw - 8;
    int x0 = (cw - pw) / 2, y0 = (ch - ph) / 2, pad = rowh / 2, lw = rowh / 12 < 2 ? 2 : rowh / 12;
    int g = rowh / 10 < 2 ? 2 : rowh / 10;
    SelectObject(dc, font_get(&f_big, &h_big, rowh * 3 / 4, FW_BOLD));
    ui_panel(dc, x0, y0, pw, ph, rowh, "Control Bindings");
    int c1 = x0 + pw * 30 / 100, c2 = x0 + pw * 64 / 100, c3 = x0 + pw - pad;
    /* column heads */
    int hy0 = y0 + rowh * 3 / 2 + rowh / 6;
    SelectObject(dc, font_get(&f_row, &h_row, rowh * 11 / 20, FW_SEMIBOLD));
    text(dc, "PS1 button", x0 + pad, hy0, c1, hy0 + rowh, accent, DT_LEFT);
    text(dc, "Keyboard", c1, hy0, c2 - g, hy0 + rowh, accent, DT_CENTER);
    text(dc, "Controller", c2 + g, hy0, c3, hy0 + rowh, accent, DT_CENTER);
    fill(dc, x0 + pad / 2, hy0 + rowh, x0 + pw - pad / 2, hy0 + rowh + (lw + 1) / 2, UI_BORDER);
    int ry = hy0 + rowh + rowh / 3;
    bv_x0 = x0; bv_x1 = x0 + pw; bv_y0 = ry; bv_rowh = rowh; bv_c1 = c1; bv_c2 = c2;
    for (int r = 0; r < BND_ROWS; r++) {
        int y = ry + bnd_row_slot(r) * rowh, sel = r == bnd_sel;
        if (r == BA_N) {   /* Reset to defaults: an action row */
            if (sel) fill(dc, x0 + pad / 2, y + 1, x0 + pw - pad / 2, y + rowh - 1, accent);
            text(dc, "Reset to defaults", x0 + pad, y, x0 + pw - pad, y + rowh, sel ? dark : fg, DT_LEFT);
            continue;
        }
        text(dc, bind_def[r].name, x0 + pad, y, c1, y + rowh, sel ? dark : fg, DT_LEFT);
        for (int c = 0; c < 2; c++) {
            int cx0 = c ? c2 + g : c1, cx1 = c ? c3 : c2 - g, on = sel && bnd_col == c;
            char buf[64];
            if (on && bnd_wait) snprintf(buf, sizeof buf, "%s", c ? "Press a button..." : "Press a key...");
            else if (c) snprintf(buf, sizeof buf, "%s", pb_names[bind_pad[r]]);
            else key_name(bind_key[r], buf, sizeof buf);
            fill(dc, cx0, y + g, cx1, y + rowh - g, on ? (bnd_wait ? UI_ACCENT_LO : accent) : UI_FIELD);
            text(dc, buf, cx0, y, cx1, y + rowh, on ? dark : val, DT_CENTER | DT_END_ELLIPSIS);
        }
    }
    /* help card */
    int cy0 = ry + (BND_ROWS + 1) * rowh + rowh / 4, cy1 = cy0 + rowh * 2;
    char hint[200];
    const char *h = hint;
    if (bnd_msg) h = bnd_msg;
    else if (bnd_wait == 1) snprintf(hint, sizeof hint, "Press the keyboard key for %s. Esc cancels.", bind_def[bnd_sel].name);
    else if (bnd_wait == 2)
        snprintf(hint, sizeof hint, "Press the controller button for %s. L3, R3, PS or Esc cancels.", bind_def[bnd_sel].name);
    else if (bnd_sel == BA_N) h = "Put every key and button back to the original layout.";
    else h = "Enter / A: change the selected key or button. Taking one that is already used swaps the two.";
    fill(dc, x0 + pad, cy0, x0 + pw - pad, cy1, UI_FIELD);
    fill(dc, x0 + pad, cy0, x0 + pad + lw, cy1, accent);
    SelectObject(dc, font_get(&f_hint, &h_hint, rowh * 9 / 20, FW_NORMAL));
    {
        RECT m = { x0 + pad + lw + rowh / 3, 0, x0 + pw - pad - rowh / 3, 0 };
        DrawTextA(dc, h, -1, &m, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        int top = cy0 + (rowh * 2 - m.bottom) / 2;
        RECT r = { x0 + pad + lw + rowh / 3, top < cy0 ? cy0 : top, x0 + pw - pad - rowh / 3, cy1 };
        SetTextColor(dc, fg);
        DrawTextA(dc, h, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
    }
    SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
    int ky = cy1 + rowh / 6;
    text(dc, "Up/Down select   Left/Right Keyboard / Controller   Enter / A / Cross change   Esc / B / Circle back",
         x0 + pad, ky, x0 + pw - pad, ky + rowh * 3 / 4, dim, DT_CENTER);
    text(dc, "Menus always use the default controls. L3 / R3 / PS and the right stick can't be bound.", x0 + pad,
         ky + rowh * 3 / 4, x0 + pw - pad, ky + rowh * 3 / 2, dim, DT_CENTER);
}

static void menu_key(int vk) {
    if (confirm_quit) { confirm_key(vk); return; }
    if (lvl_open) { lvl_key(vk); return; }
    if (mod_open) { mod_key(vk); return; }
    if (bnd_open) { bnd_key(vk); return; }
    switch (vk) {
    case VK_UP: menu_move(-1); break;
    case VK_DOWN: menu_move(+1); break;
    case VK_LEFT: item_change(-1, 0); break;
    case VK_RIGHT: item_change(+1, 0); break;
    case 'Q': case VK_PRIOR: menu_set_tab(menu_tab - 1); break;
    case 'E': case VK_NEXT: menu_set_tab(menu_tab + 1); break;
    case VK_TAB: menu_set_tab(menu_tab + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)); break;
    case VK_RETURN: case VK_SPACE: case 'Z': item_activate(); break;
    case VK_ESCAPE: case VK_BACK: case 'X': menu_open = 0; break;
    }
}

static int cq_y0, cq_y1, cq_x[4]; /* quit prompt buttons: No x0..x1, Yes x2..x3 */
static void menu_mouse(int x, int y, int button, int wheel) {
    if (confirm_quit) {
        if (y < cq_y0 || y >= cq_y1) return;
        int b = x >= cq_x[0] && x < cq_x[1] ? 0 : x >= cq_x[2] && x < cq_x[3] ? 1 : -1;
        if (b < 0) return;
        confirm_sel = b;
        if (button == 1) confirm_close(b);
        return;
    }
    if (lvl_open) { lvl_mouse(x, y, button, wheel); return; }
    if (mod_open) { mod_mouse(x, y, button, wheel); return; }
    if (bnd_open) { bnd_mouse(x, y, button, wheel); return; }
    if (!lay_rowh || x < lay_x0 || x >= lay_x1) return;
    if (y >= lay_ty0 && y < lay_ty1) { /* tab bar: click a page, or the wheel flips pages */
        if (wheel) menu_set_tab(menu_tab - wheel);
        else if (button == 1) {
            int a = lay_x0 + lay_rowh / 4, b = lay_x1 - lay_rowh / 4;
            for (int t = 0; t < T_COUNT; t++) if (x < tab_edge(t + 1, a, b) || t == T_COUNT - 1) { menu_set_tab(t); break; }
        }
        return;
    }
    if (y < lay_y0) return;
    int slot = (y - lay_y0) / lay_rowh, row = -1;
    for (int i = 0; i < menu_nvis; i++) if (menu_slot(i) == slot) { row = i; break; }   /* empty rows hit nothing */
    if (row < 0) return;
    menu_sel = row;
    if (wheel) item_change(wheel, 0);
    else if (button == 2) item_change(-1, 0);
    else if (button == 1) {
        if (menu_items[menu_vis[row]].kind == MI_CHOICE && x >= lay_vx0)
            item_change(x < (lay_vx0 + lay_vx1) / 2 ? -1 : +1, 1);
        else item_activate();
    }
}

static HFONT font_get(HFONT *cache, int *cache_h, int h, int weight) {
    if (*cache && *cache_h == h) return *cache;
    if (*cache) DeleteObject(*cache);
    *cache = CreateFontA(-h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    *cache_h = h;
    return *cache;
}

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c) {
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void text(HDC dc, const char *s, int x0, int y0, int x1, int y1, COLORREF c, UINT align) {
    RECT r = { x0, y0, x1, y1 };
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

/* Panel background with a thin border and a purple gradient header band (height rowh * 3/2) carrying the title.
   The caller selects the title font first. */
static void ui_panel(HDC dc, int x0, int y0, int pw, int ph, int rowh, const char *title) {
    int bw = rowh / 16 < 1 ? 1 : rowh / 16, hh = rowh * 3 / 2, x1 = x0 + pw;
    fill(dc, x0 - bw, y0 - bw, x1 + bw, y0 + ph + bw, UI_BORDER);
    fill(dc, x0, y0, x1, y0 + ph, UI_BG);
    int step = pw / 96 < 2 ? 2 : pw / 96;
    for (int x = x0; x < x1; x += step) {   /* 3b1a7e -> a855f7, weighted to the right */
        float t = (float)(x - x0) / pw;
        t *= t;
        fill(dc, x, y0, x + step > x1 ? x1 : x + step, y0 + hh,
             RGB(0x3b + (int)((0xa8 - 0x3b) * t), 0x1a + (int)((0x55 - 0x1a) * t), 0x7e + (int)((0xf7 - 0x7e) * t)));
    }
    int sv = SaveDC(dc);
    IntersectClipRect(dc, x0, y0, x1, y0 + hh);   /* thin rings on the right, cut off by the band */
    HPEN pen = CreatePen(PS_SOLID, bw, RGB(201, 166, 255));
    SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    int cx = x1 - rowh * 5 / 4, cy = y0 + hh / 2;
    for (int k = 0; k < 3; k++) {
        int r = rowh * (10 + 8 * k) / 10;
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    }
    RestoreDC(dc, sv);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    text(dc, title, x0 + rowh * 3 / 4, y0, x1, y0 + hh, UI_WHITE, DT_LEFT);
}

static void menu_draw_panel(HDC dc, int cw, int ch) {
    static HFONT f_title, f_row, f_small;
    static int h_title, h_row, h_small;
    const COLORREF accent = UI_ACCENT, fg = UI_TEXT, val = UI_SOFT, dim = UI_MUTED, dark = UI_WHITE;
    menu_build();
    int R = menu_max_rows(), page = menu_nvis - menu_nact;
    int rowh = ch / (R + 9);
    if (rowh > cw / 22) rowh = cw / 22;
    if (rowh < 14) rowh = 14;
    int pw = rowh * 21;   /* was 17: six tabs (Debug, 2026-11-23) were cramped */
    if (pw > cw - 8) pw = cw - 8;
    int ph = rowh * (R + 7);
    int x0 = (cw - pw) / 2, y0 = (ch - ph) / 2, pad = rowh / 2;
    int lw = rowh / 12 < 2 ? 2 : rowh / 12;   /* accent line on the open tab */
    SelectObject(dc, font_get(&f_title, &h_title, rowh * 3 / 4, FW_BOLD));
    ui_panel(dc, x0, y0, pw, ph, rowh, "Options");
    /* tab bar: the open page is a lighter block with an accent line on top */
    int ty = y0 + rowh * 3 / 2 + rowh / 6;
    lay_ty0 = ty; lay_ty1 = ty + rowh;
    SelectObject(dc, font_get(&f_row, &h_row, rowh * 11 / 20, FW_SEMIBOLD));
    for (int t = 0; t < T_COUNT; t++) {
        int tx0 = tab_edge(t, x0 + pad / 2, x0 + pw - pad / 2), tx1 = tab_edge(t + 1, x0 + pad / 2, x0 + pw - pad / 2);
        if (t == menu_tab) {
            fill(dc, tx0 + 2, ty, tx1 - 2, ty + rowh, UI_FIELD);
            fill(dc, tx0 + 2, ty, tx1 - 2, ty + lw, accent);
        }
        text(dc, tab_names[t], tx0, ty, tx1, ty + rowh, t == menu_tab ? UI_WHITE : dim, DT_CENTER);
    }
    fill(dc, x0 + pad / 2, ty + rowh, x0 + pw - pad / 2, ty + rowh + (lw + 1) / 2, UI_BORDER);
    int ry = ty + rowh + rowh / 3;
    int vx0 = x0 + pw * 52 / 100, vx1 = x0 + pw - pad;
    lay_x0 = x0; lay_x1 = x0 + pw; lay_y0 = ry; lay_rowh = rowh; lay_vx0 = vx0; lay_vx1 = vx1;
    SelectObject(dc, font_get(&f_row, &h_row, rowh * 11 / 20, FW_SEMIBOLD));
    for (int i = 0; i < menu_nvis; i++) {
        const MenuItem *it = &menu_items[menu_vis[i]];
        int y = ry + menu_slot(i) * rowh, sel = i == menu_sel; /* actions pinned to the bottom */
        if (sel) fill(dc, x0 + pad / 2, y + 1, x0 + pw - pad / 2, y + rowh - 1, accent);
        if (i == page) fill(dc, x0 + pad, y - 1, x0 + pw - pad, y, UI_BORDER); /* rule above actions */
        if (it->section) { /* sub-heading in the row above: accent text, then a thin rule to the right edge */
            SIZE sz;
            GetTextExtentPoint32A(dc, it->section, (int)strlen(it->section), &sz);
            text(dc, it->section, x0 + pad, y - rowh, x0 + pw - pad, y, accent, DT_LEFT);
            int lx = x0 + pad + sz.cx + pad / 2, my = y - rowh / 2;
            if (lx < x0 + pw - pad) fill(dc, lx, my, x0 + pw - pad, my + 1, UI_BORDER);
        }
        text(dc, it->label, x0 + pad, y, vx0, y + rowh, sel ? dark : fg, DT_LEFT);
        if (it->kind == MI_CHOICE) {
            char buf[64];
            item_value_text(it, buf, sizeof buf);
            int idx = item_index(it), g = rowh / 10 < 2 ? 2 : rowh / 10;
            COLORREF vc = sel ? dark : val, ac = sel ? dark : dim;
            fill(dc, vx0, y + g, vx1, y + rowh - g, sel ? UI_ACCENT_LO : UI_FIELD);   /* value box, like a dropdown */
            if (idx > 0) text(dc, "<", vx0 + pad / 2, y, vx1, y + rowh, ac, DT_LEFT);
            if (idx < it->nvals - 1) text(dc, ">", vx0, y, vx1 - pad / 2, y + rowh, ac, DT_RIGHT);
            text(dc, buf, vx0, y, vx1, y + rowh, vc, DT_CENTER);
        }
    }
    SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
    int hy = ry + R * rowh + rowh / 4;
    /* description card: up to 3 wrapped lines, centred top to bottom, accent bar on the left */
    {
        int bx0 = x0 + pad, bx1 = x0 + pw - pad, by1 = hy + rowh * 2, in = rowh / 3;
        const char *h = menu_items[menu_vis[menu_sel]].hint;
        static HFONT f_hint;
        static int h_hint;
        fill(dc, bx0, hy, bx1, by1, UI_FIELD);
        fill(dc, bx0, hy, bx0 + lw, by1, accent);
        SelectObject(dc, font_get(&f_hint, &h_hint, rowh * 9 / 20, FW_NORMAL));
        RECT m = { bx0 + lw + in, 0, bx1 - in, 0 };
        DrawTextA(dc, h, -1, &m, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        int th = m.bottom, top = hy + (rowh * 2 - th) / 2;
        if (top < hy) top = hy;
        RECT r = { bx0 + lw + in, top, bx1 - in, by1 };
        SetTextColor(dc, fg);
        DrawTextA(dc, h, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
    }
    int ky = hy + rowh * 2 + rowh / 8;
    text(dc, "Up/Down select   Left/Right change   Enter / A / Cross ok   Esc / B / Circle / F1 close", x0 + pad,
         ky, x0 + pw - pad, ky + rowh * 3 / 4, dim, DT_CENTER);
    text(dc, "Page: Q / E, Tab, PgUp / PgDn, LB / RB, L1 / R1, or click a tab", x0 + pad,
         ky + rowh * 3 / 4, x0 + pw - pad, ky + rowh * 3 / 2, dim, DT_CENTER);
}

static void confirm_draw(HDC dc, int cw, int ch) {
    static HFONT f_big, f_btn, f_small;
    static int h_big, h_btn, h_small;
    const COLORREF accent = UI_ACCENT, fg = UI_TEXT, dim = UI_MUTED, dark = UI_WHITE, btn = UI_FIELD;
    int rowh = ch / 16;
    if (rowh > cw / 22) rowh = cw / 22;
    if (rowh < 14) rowh = 14;
    int pw = rowh * 11, ph = rowh * 11 / 2;
    if (pw > cw - 8) pw = cw - 8;
    int x0 = (cw - pw) / 2, y0 = (ch - ph) / 2, pad = rowh / 2;
    SelectObject(dc, font_get(&f_big, &h_big, rowh * 3 / 4, FW_BOLD));
    ui_panel(dc, x0, y0, pw, ph, rowh, "Quit Rugrats?");
    SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
    text(dc, "Progress since your last save will be lost.", x0 + pad, y0 + rowh * 7 / 4, x0 + pw - pad,
         y0 + rowh * 5 / 2, fg, DT_CENTER);
    int by = y0 + rowh * 3, bw = pw * 3 / 10, gapw = pw / 10;
    int nx = x0 + pw / 2 - gapw / 2 - bw, yx = x0 + pw / 2 + gapw / 2;
    cq_y0 = by; cq_y1 = by + rowh * 6 / 5;
    cq_x[0] = nx; cq_x[1] = nx + bw; cq_x[2] = yx; cq_x[3] = yx + bw;
    SelectObject(dc, font_get(&f_btn, &h_btn, rowh * 11 / 20, FW_SEMIBOLD));
    for (int b = 0; b < 2; b++) {
        int sel = confirm_sel == b, bx = b ? yx : nx;
        fill(dc, bx, cq_y0, bx + bw, cq_y1, sel ? accent : btn);
        text(dc, b ? "Yes, quit" : "No", bx, cq_y0, bx + bw, cq_y1, sel ? dark : fg, DT_CENTER);
    }
    SelectObject(dc, font_get(&f_small, &h_small, rowh * 2 / 5, FW_NORMAL));
    text(dc, "Enter / A / Cross choose     Esc / B / Circle cancel", x0 + pad, y0 + ph - rowh * 4 / 5,
         x0 + pw - pad, y0 + ph - rowh / 5, dim, DT_CENTER);
}

static void menu_draw(HDC dc, int cw, int ch) {
    if ((!confirm_quit || confirm_from_menu) && !lvl_open && !mod_open && !bnd_open) menu_draw_panel(dc, cw, ch);
    if (lvl_open && !confirm_quit) lvl_draw(dc, cw, ch);
    if (mod_open && !confirm_quit) mod_draw(dc, cw, ch);
    if (bnd_open && !confirm_quit) bnd_draw(dc, cw, ch);
    if (confirm_quit) confirm_draw(dc, cw, ch);
}

static void menu_save(void) {
    if (test_frames && !getenv("RUGRATS_MENU_SAVE")) return; /* tests must not touch the user's ini */
    for (int i = 0; i < MENU_N; i++) {
        const MenuItem *it = &menu_items[i];
        if (!it->key) continue;
        char buf[16];
        snprintf(buf, sizeof buf, "%d", *it->val);
        WritePrivateProfileStringA(it->sec, it->key, buf, ini_path);
    }
}

/* controller: either stick click opens/closes; d-pad or left stick navigates with key repeat */
static int pad_inject = -1;  /* menu pad test (RUGRATS_MENU_PADTEST): >= 0 replaces the real controllers */
static WORD xin_buttons(void) {
    XINPUT_STATE st;
    WORD w = 0;
    if (pad_inject >= 0) return (WORD)pad_inject;
    if (xget && xget(0, &st) == ERROR_SUCCESS) {
        w = st.Gamepad.wButtons;
        if (st.Gamepad.sThumbLY > 16000) w |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -16000) w |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > 16000) w |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -16000) w |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
    uint32_t d = ds_buttons(); /* Sony pads in Xbox terms: Cross = A, Circle = B, Options = Start */
    if (d & DS_UP) w |= XINPUT_GAMEPAD_DPAD_UP;
    if (d & DS_DOWN) w |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (d & DS_LEFT) w |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (d & DS_RIGHT) w |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (d & DS_CROSS) w |= XINPUT_GAMEPAD_A;
    if (d & DS_CIRCLE) w |= XINPUT_GAMEPAD_B;
    if (d & DS_OPTIONS) w |= XINPUT_GAMEPAD_START;
    if (d & DS_L1) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (d & DS_R1) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (d & DS_L3) w |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (d & DS_R3) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (d & DS_PS) w |= XIN_GUIDE;
    return w;
}
static WORD xin_prev_game;
static int menu_test_frame = -1;

static void menu_pad(void) {
    static WORD prev;
    static DWORD next_rep;
    WORD w = xin_buttons(), down = w & ~prev;
    const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
                      XINPUT_GAMEPAD_DPAD_RIGHT;
    DWORD now = GetTickCount();
    if (down & dirs) next_rep = now + 350;
    else if ((w & dirs) && now >= next_rep) { down |= w & dirs; next_rep = now + 90; }
    prev = w;
    if (confirm_quit) {
        if (down & dirs) confirm_sel = !confirm_sel;
        if (down & XINPUT_GAMEPAD_A) confirm_close(confirm_sel);
        else if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XIN_TOGGLE)) confirm_close(0);
        return;
    }
    if (lvl_open) {
        if (down & XINPUT_GAMEPAD_DPAD_UP) lvl_key(VK_UP);
        if (down & XINPUT_GAMEPAD_DPAD_DOWN) lvl_key(VK_DOWN);
        if (down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_LEFT_SHOULDER)) lvl_key(VK_PRIOR);
        if (down & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_SHOULDER)) lvl_key(VK_NEXT);
        if (down & XINPUT_GAMEPAD_A) lvl_go();
        else if (down & XINPUT_GAMEPAD_B) lvl_open = 0;              /* back to the options */
        else if (down & (XINPUT_GAMEPAD_START | XIN_TOGGLE)) { lvl_open = 0; menu_open = 0; }
        return;
    }
    if (mod_open) {
        if (down & XINPUT_GAMEPAD_DPAD_UP) mod_key(VK_UP);
        if (down & XINPUT_GAMEPAD_DPAD_DOWN) mod_key(VK_DOWN);
        if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) mod_key(VK_PRIOR);
        if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) mod_key(VK_NEXT);
        if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) mod_key(VK_RETURN);
        else if (down & XINPUT_GAMEPAD_B) mod_close();                /* back to the options */
        else if (down & (XINPUT_GAMEPAD_START | XIN_TOGGLE)) { mod_close(); menu_open = 0; }
        return;
    }
    if (bnd_open) { bnd_pad(down); return; }
    if (down & XINPUT_GAMEPAD_DPAD_UP) menu_key(VK_UP);
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) menu_key(VK_DOWN);
    if (down & XINPUT_GAMEPAD_DPAD_LEFT) menu_key(VK_LEFT);
    if (down & XINPUT_GAMEPAD_DPAD_RIGHT) menu_key(VK_RIGHT);
    if (down & XINPUT_GAMEPAD_A) item_activate();
    if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) menu_set_tab(menu_tab - 1);
    if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) menu_set_tab(menu_tab + 1);
    if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XIN_TOGGLE)) menu_open = 0;
}

static void write_bmp(const char *name, const uint32_t *px, int w, int h) {
    char dir[1024], p[1200];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    snprintf(p, sizeof p, "%s\\shots", dir);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\shots\\%s.bmp", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { 0 };
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + w * h * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

/* RUGRATS_MENU=<frame>: in test mode, open the menu at that frame, step through it and dump shots */
static int shot_pad_n(void) { static int n = 100; return n++; }
static void menu_test_step(int step) {
    /* RUGRATS_MENU_PADTEST=1: controller navigation through menu_pad (injected states, press + release), plus a
       WM_MOUSEMOVE that repeats the last position (no real move) which must not change the selection */
    if (getenv("RUGRATS_MENU_PADTEST")) {
        enum { U = XINPUT_GAMEPAD_DPAD_UP, D = XINPUT_GAMEPAD_DPAD_DOWN, L = XINPUT_GAMEPAD_DPAD_LEFT,
               R = XINPUT_GAMEPAD_DPAD_RIGHT, A = XINPUT_GAMEPAD_A, B = XINPUT_GAMEPAD_B,
               RS = XINPUT_GAMEPAD_RIGHT_SHOULDER, MV = -2, LV = -3, SH = -4 };
        static const int ps[] = { D, 0, D, 0, SH, LV, MV, 0, D, 0, D, 0, MV, MV, 0, SH, U, 0, RS, 0, U, 0, MV, SH,
                                  B, 0, D, 0, D, 0, MV, MV, U, 0, SH, RS, 0, D, 0, SH };
        static int xy_set, mx, my;
        int ns = (int)(sizeof ps / sizeof ps[0]);
        if (step == 0) {
            XINPUT_STATE st; uint32_t d = ds_buttons();
            rt_logf("menu pad test: real pads now xinput %s buttons %04x, sony %u pads buttons %08x\n",
                    xget && xget(0, &st) == ERROR_SUCCESS ? "yes" : "no",
                    xget && xget(0, &st) == ERROR_SUCCESS ? st.Gamepad.wButtons : 0, (unsigned)ds_count(), (unsigned)d);
        }
        pad_inject = 0;
        if (step >= ns) { pad_inject = -1; menu_open = 0; return; }
        int k = ps[step];
        if (k == LV) { menu_set_tab(T_ENH); ac_levels(); }
        else if (k == MV) {
            if (!xy_set && lvl_open) {   /* park the "cursor" on the 7th visible row */
                xy_set = 1; mx = (lv_x0 + lv_x1) / 2; my = lv_y0 + lv_rowh * 6 + lv_rowh / 2;
            }
            SendMessage(wnd, WM_MOUSEMOVE, 0, MAKELPARAM(mx, my));
        } else if (k == SH) {
            char n[32]; snprintf(n, sizeof n, "menu%d", shot_pad_n());
            if (back_px) write_bmp(n, back_px, back_w, back_h);
        } else pad_inject = k;
        rt_logf("menu pad test: step %d in %d -> lvl_open %d lvl_sel %d (%s) | tab %d menu_sel %d\n", step, k, lvl_open,
                lvl_sel, lvl_list[lvl_sel].name, menu_tab, menu_sel);
        return;
    }
    static const int script[] = { 0, VK_DOWN, VK_DOWN, VK_DOWN, 0, VK_RIGHT, 0, VK_UP, VK_UP, VK_RIGHT, 0,
                                  VK_LEFT, VK_DOWN, VK_DOWN, VK_LEFT, 'E', 0, 'E', 0, VK_UP, 'E', 0, VK_NEXT, 0,
                                  VK_ESCAPE };
    static int shot;
    /* RUGRATS_MENU_LVL=<db>: open Level Select, select that level, one shot (menuN), then Enter = go */
    const char *lv = getenv("RUGRATS_MENU_LVL");
    if (lv) {
        if (step == 0) { menu_set_tab(T_ENH); ac_levels(); return; }
        if (step == 1) { for (int i = 0; i < LVL_N; i++) if (lvl_list[i].db == atoi(lv)) lvl_sel = i; lvl_clamp(); return; }
        if (step == 2) {
            char n[32]; snprintf(n, sizeof n, "menu%d", shot++);
            if (back_px) write_bmp(n, back_px, back_w, back_h);
            rt_logf("menu test: %s level select on db %d\n", n, lvl_list[lvl_sel].db);
            return;
        }
        if (step == 3) { lvl_key(VK_RETURN); if (lvl_err) rt_logf("menu test: level select refused: %s\n", lvl_err); return; }
        menu_open = 0;
        return;
    }
    /* RUGRATS_MENU_BIND=1: Control Bindings: shots of the page, waiting, a refused key, a swap, the pad column */
    if (getenv("RUGRATS_MENU_BIND")) {
        static const int bs[] = { -2, 0, VK_DOWN, VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN, 0, VK_F5, 0, 'C', 0,
                                  VK_RETURN, 'X', -3, VK_RIGHT, VK_RETURN, 0, VK_ESCAPE, -4 };
        int ns2 = (int)(sizeof bs / sizeof bs[0]);
        if (step >= ns2) { menu_open = 0; return; }
        int k = bs[step];
        if (k == -2) { menu_set_tab(T_AUDIO); ac_binds(); return; }
        if (k == -3 || k == -4) {
            char kc[16], kx[16];
            key_name(bind_key[BA_CROSS], kc, sizeof kc); key_name(bind_key[BA_CIRCLE], kx, sizeof kx);
            rt_logf("menu test: bindings cross=%s circle=%s pad cross=%s\n", kc, kx, pb_ini[bind_pad[BA_CROSS]]);
            if (k == -3) return;
        }
        if (k > 0) { menu_key(k); return; }
        char n[32]; snprintf(n, sizeof n, "menu%d", shot++);
        if (back_px) write_bmp(n, back_px, back_w, back_h);
        rt_logf("menu test: %s bindings sel %d col %d wait %d\n", n, bnd_sel, bnd_col, bnd_wait);
        if (k == -4) bnd_close();
        return;
    }
    /* RUGRATS_MENU_MODS=1: open Mods, shot, switch the first mod, shot, close */
    if (getenv("RUGRATS_MENU_MODS")) {
        if (step == 0) { menu_set_tab(T_GAME); ac_mods(); return; }
        if (step == 2) mod_key(VK_RETURN);
        if (step == 1 || step == 3) {
            char n[32]; snprintf(n, sizeof n, "menu%d", shot++);
            if (back_px) write_bmp(n, back_px, back_w, back_h);
            rt_logf("menu test: %s mods page, active %d\n", n, mods_active());
            return;
        }
        if (step == 4) { mod_close(); return; }
        if (step > 4) menu_open = 0;
        return;
    }
    /* RUGRATS_MENU_KEYS=<keys>: one key per step, U D L R = arrows, 0 = shot (menuN), then close; with
       RUGRATS_MENU_TAB=<page> that page is shown first (e.g. TAB=5 KEYS=0RR0D0LL0 for the Debug page) */
    const char *mk = getenv("RUGRATS_MENU_KEYS");
    if (mk) {
        const char *t = getenv("RUGRATS_MENU_TAB");
        if (step == 0) { if (t) menu_set_tab(atoi(t)); return; }
        if (step - 1 >= (int)strlen(mk)) { menu_open = 0; return; }
        const char c = mk[step - 1];
        if (c == '0') {
            char n[32]; snprintf(n, sizeof n, "menu%d", shot++);
            if (back_px) write_bmp(n, back_px, back_w, back_h);
            rt_logf("menu test: %s tab %d row %d\n", n, menu_tab, menu_sel);
        } else menu_key(c == 'U' ? VK_UP : c == 'D' ? VK_DOWN : c == 'L' ? VK_LEFT : VK_RIGHT);
        return;
    }
    /* RUGRATS_MENU_ESC=1: the quit prompt instead (No shot, Yes shot, then =1 cancels with Esc, =2 quits) */
    static const int esc_cancel[] = { 0, VK_RIGHT, 0, VK_ESCAPE }, esc_quit[] = { 0, VK_RIGHT, 0, VK_RETURN };
    const char *esc = getenv("RUGRATS_MENU_ESC");
    const int *sc = script;
    int ns = (int)(sizeof script / sizeof script[0]);
    if (esc) { sc = atoi(esc) == 2 ? esc_quit : esc_cancel; ns = 4; }
    /* RUGRATS_MENU_TAB=<page>: one shot of that page, then close (no option changed) */
    static const int tab_shot[] = { -1, 0, VK_ESCAPE };
    const char *tb = getenv("RUGRATS_MENU_TAB");
    if (tb) {   /* RUGRATS_MENU_ROW=<n> with it: select that row of the page first */
        const char *rw = getenv("RUGRATS_MENU_ROW");
        if (step == 0) { menu_set_tab(atoi(tb)); if (rw) { menu_sel = atoi(rw); menu_build(); } return; }
        sc = tab_shot; ns = 3;
    }
    if (step >= ns) { menu_open = 0; return; }
    if (sc[step]) { menu_key(sc[step]); return; }
    char n[32];
    snprintf(n, sizeof n, "menu%d", shot++);
    if (back_px) write_bmp(n, back_px, back_w, back_h);
    rt_logf("menu test: %s fps=%d scale=%d\n", n, opt.fps, opt.scale);
}

static void menu_run(void) {
    convert_frame(); /* the menu draws over the 32-bit frame */
    menu_bg_reset();
    want_fs = fullscreen;
    dbg_sync();
    menu_sel = 0;
    lvl_open = 0;
    mod_open = 0;
    bnd_open = 0;
    bnd_wait = 0;
    menu_build();
    if (wo) waveOutPause(wo);
    update_title();
    rt_logf("menu open\n");
    MSG msg;
    LARGE_INTEGER mnext;
    QueryPerformanceCounter(&mnext);
    for (int step = 0; menu_open && !quit; step++) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        cursor_tick();
        menu_pad();
        if (!menu_open) break;
        show();
        if (test_frames) menu_test_step(step);
        else if (!vsync_interval()) {
            /* pace to the game's frame rate (was Sleep(15) on top of the present: ~30 fps or less) */
            LONGLONG per = qpf.QuadPart * 1001 / ((LONGLONG)(PRESENT_FPS > 0 ? PRESENT_FPS : 60) * 1000);
            LARGE_INTEGER now;
            mnext.QuadPart += per;
            for (;;) {
                QueryPerformanceCounter(&now);
                LONGLONG left = mnext.QuadPart - now.QuadPart;
                if (left <= 0) break;
                if (left * 1000 / qpf.QuadPart > 2) Sleep(1);
            }
            if (now.QuadPart - mnext.QuadPart > qpf.QuadPart / 10) mnext = now; /* fell behind: resync */
        }                                   /* vsync on: Present() already waits for the display */
        {   /* debug: RUGRATS_MENULOG=1 logs the menu frame rate once a second */
            static int ml = -1; static LARGE_INTEGER t0; static int n;
            if (ml < 0) ml = getenv("RUGRATS_MENULOG") != NULL;
            if (ml) {
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                if (!t0.QuadPart) t0 = now;
                n++;
                if (now.QuadPart - t0.QuadPart >= qpf.QuadPart) {
                    rt_logf("menu: %.1f fps (target %d)\n", n * (double)qpf.QuadPart / (double)(now.QuadPart - t0.QuadPart), PRESENT_FPS);
                    t0 = now; n = 0;
                }
            }
        }
    }
    menu_open = 0;
    if (mod_open) mod_close();   /* F1 / Start closed everything: still save the mod list */
    if (bnd_open) bnd_close();   /* ... and the bindings */
    menu_save();
    if (wo) waveOutRestart(wo);
    memset(keys, 0, sizeof keys);
    pad_block = 1;
    xin_prev_game = xin_buttons(); /* a held stick click must not reopen it */
    update_title();
    QueryPerformanceCounter(&last_frame);
    rt_logf("menu closed\n");
}

/* Auto frame rate. Each presented frame advances the PS1 by 1/fps of a second, so if the PC can't
   draw opt.fps frames per real second the whole game (and its audio) slows down and the sound card
   runs dry. Instead, present fewer frames: game time follows real time at any rate (time-step hook),
   so speed and audio stay right. Goes back up when there is headroom again. */
static void auto_rate(LONGLONG slept) {
    static LARGE_INTEGER prev;
    static double avg_dt, avg_busy;
    static int n, last_opt, last_hz;
    /* display cap (2026-11-12): fullscreen presents can't go faster than the display's refresh here, even with
       vsync off. A rate above it then "fails" with frames arriving at exactly the display rate; busy time includes
       that wait, so 0.92 / busy landed BELOW the display (130 -> 110 on 120 Hz) and every 3 s of headroom it climbed
       back past it: 110 -> 120 -> 130 -> 110 forever (user: drops to 110 at 4K / 120 Hz). Now such a failure settles
       on the display rate and no step-up goes past it again (until the fps option or the display changes). */
    static int hz_cap;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last_opt != opt.fps || last_hz != display_hz) { /* user changed the rate / display: start over */
        if (present_fps) { present_fps = 0; update_title(); }
        last_opt = opt.fps;
        last_hz = display_hz;
        hz_cap = 0;
        n = 0;
    }
    double dt = prev.QuadPart ? (double)(now.QuadPart - prev.QuadPart) / qpf.QuadPart : 0;
    double busy = dt - (double)slept / qpf.QuadPart;
    prev = now;
    {   /* RUGRATS_DTLOG=1 (debug): every 600 frames, a histogram of real frame times and busy times (1 ms buckets) */
        static int dl = -1, nn, hd[24], hb[24];
        if (dl < 0) dl = getenv("RUGRATS_DTLOG") != NULL;
        if (dl && dt > 0) {
            int a = (int)(dt * 1000.0), b = (int)(busy * 1000.0);
            hd[a < 0 ? 0 : a > 23 ? 23 : a]++; hb[b < 0 ? 0 : b > 23 ? 23 : b]++;
            if (++nn == 600) {
                char s1[256] = "", s2[256] = "";
                for (int i = 0; i < 24; i++) {
                    if (hd[i]) snprintf(s1 + strlen(s1), sizeof s1 - strlen(s1), " %d:%d", i, hd[i]);
                    if (hb[i]) snprintf(s2 + strlen(s2), sizeof s2 - strlen(s2), " %d:%d", i, hb[i]);
                }
                rt_logf("dt %d fps | GPU %.2f ms | frame ms%s | busy ms%s\n", PRESENT_FPS, hw_gpu_ms(), s1, s2);
                nn = 0; memset(hd, 0, sizeof hd); memset(hb, 0, sizeof hb);
            }
        }
    }
    if (dt <= 0 || dt > 0.25 || keys[VK_TAB]) { n = 0; return; } /* loading hitch, menu, fast-forward */
    int cur = PRESENT_FPS;
    double frame = 1.001 / cur;
    /* one slow present (a stall) counts as at most 2 frames, so only SUSTAINED slowness lowers the rate. A single
       ~50 ms stall used to lift the average past the threshold and drop 120 -> 84 fps for the next ~10 s. */
    if (dt > 2 * frame) dt = 2 * frame;
    if (busy > 2 * frame) busy = 2 * frame;
    avg_dt = n ? avg_dt * 0.96 + dt * 0.04 : dt;
    avg_busy = n ? avg_busy * 0.96 + busy * 0.04 : busy;
    n++;
    /* Refresh-rate ladder (2026-11-16, user: still rough at 4K / 120 Hz). When the target is the display's refresh
       rate (fps = Hz, or above it once hz_cap found the display holds presents to its refresh), the old steps (120 ->
       115 -> 99 -> 82) could not line up with a fixed 120 Hz display: frames came 8 / 10 / 16 ms apart (judder), and
       since busy includes the wait for the display, every lower rate looked MORE loaded, so it kept stepping down,
       then jumped back to 120 and started over (user's log; reproduced on the 4K TV with RUGRATS_MSAA=8). Now only
       even shares of the refresh rate are used (120, 60, 40 on 120 Hz): stay at the refresh rate through short misses
       and drop one share only when about half the frames miss for 2 s (see "over" below); go back up when the measured work (CPU busy at
       the lower rate, and GPU time from timestamp queries, hw_gpu_ms) fits 80% of the faster frame. A drop within
       10 s of going up doubles the wait before the next try (3 s up to 30 s), so it can't cycle; a minute at the
       full rate resets it. RUGRATS_LADDER=0 = the old steps. Rates below the refresh rate (fps < Hz) keep the steps. */
    {
        static int ladder = -1;
        static LONGLONG t_up;
        static double hold = 3;
        static int over;
        if (ladder < 0) { const char *e = getenv("RUGRATS_LADDER"); ladder = e ? atoi(e) != 0 : 1; }
        const int base = hz_cap ? hz_cap : (display_hz > 30 && abs(opt.fps - display_hz) <= 1) ? opt.fps : 0;
        if (ladder && base) {
            int div = (base + cur / 2) / cur;
            if (div < 1) div = 1;
            /* drop only when about half the frames miss the refresh (avg > 1.5 frames, i.e. below ~80 fps at 120 Hz) for
               2 s in a row (2026-11-16, user: "keeping the game at 60 much too often"; their log: drops at avg 10 ms =
               ~1 in 5 frames late with GPU 3 ms, where 120 with a few late frames is smoother than a locked 60) */
            over = n == 1 ? 0 : avg_dt > frame * 1.5 ? over + 1 : 0;
            if (n > cur && over > 2 * cur && base / (div + 1) >= 30) {
                const int nf = base / (div + 1);
                const double since_up = t_up ? (double)(now.QuadPart - t_up) / qpf.QuadPart : 1e9;
                if (since_up < 10.0) hold = hold * 2 > 30 ? 30 : hold * 2;
                rt_logf("fps: PC can't keep up with %d, presenting %d (1/%d of the %d Hz display; game speed and audio stay at "
                        "100%%; frame %.2f ms, busy %.2f ms, GPU %.2f ms; next try after %.0f s)\n", cur, nf, div + 1,
                        display_hz, avg_dt * 1000.0, avg_busy * 1000.0, hw_gpu_ms(), hold);
                present_fps = nf >= opt.fps ? 0 : nf;
                update_title();
                n = 0;
            } else if (div > 1 && n > hold * cur) {
                const int up = base / (div - 1);
                const double gpu = hw_gpu_ms() / 1000.0, work = avg_busy > gpu ? avg_busy : gpu;
                if (work < 0.8 * 1.001 / up) {
                    present_fps = up >= opt.fps ? 0 : up;
                    t_up = now.QuadPart;
                    rt_logf("fps: headroom again, presenting %d (busy %.2f ms, GPU %.2f ms)\n", PRESENT_FPS, avg_busy * 1000.0,
                            gpu * 1000.0);
                    update_title();
                }
                n = 0;
            } else if (div == 1 && t_up && (double)(now.QuadPart - t_up) / qpf.QuadPart > 60.0) {
                hold = 3;   /* a minute at the full rate: quick retries again */
                t_up = 0;
            }
            return;
        }
    }
    if (n > cur && avg_dt > frame * 1.06 && cur > 30) { /* behind for ~1 s */
        int nf = (int)(0.92 / avg_busy);
        if (display_hz > 30 && cur > display_hz && 1.0 / avg_dt >= display_hz * 0.97) {   /* held the display rate */
            hz_cap = display_hz;
            nf = display_hz;
            rt_logf("fps: %d is above what the %d Hz display shows, presenting %d\n", cur, display_hz, nf);
        } else {
            if (nf >= cur) nf = cur - 5;
            if (nf < 30) nf = 30;
            rt_logf("fps: PC can't keep up with %d, presenting %d (game speed and audio stay at 100%%; frame %.2f ms, busy %.2f ms)\n",
                    cur, nf, avg_dt * 1000.0, avg_busy * 1000.0);
        }
        present_fps = nf >= opt.fps ? 0 : nf;
        update_title();
        n = 0;
    } else if (present_fps && n > 3 * cur && !(hz_cap && cur >= hz_cap)) { /* 3 s of headroom: try higher */
        int up = cur + 10;
        /* recover in one step after a hitch: straight to what the measured work allows (0.8 margin, as the check
           below), but not past the display rate on the way up (beyond it only in +10 steps). Used to be +10 per 3 s:
           a short hitch at 144 kept the game at 123-143 for ~9 s. */
        int fit = (int)(0.8 * 1.001 / avg_busy);
        if (fit > up) up = display_hz > cur && fit > display_hz ? display_hz : fit;
        if (up > opt.fps) up = opt.fps;
        if (hz_cap && up > hz_cap) up = hz_cap;
        if (avg_busy < 0.8 * 1.001 / up) {
            present_fps = up >= opt.fps ? 0 : up;
            rt_logf("fps: headroom again, presenting %d\n", PRESENT_FPS);
            update_title();
        }
        n = 0;
    }
}

void host_present(void) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (quit) { rt_logf("quit\n"); exit(0); }
    cursor_tick();
    {
        static int prof = -1;
        static uint64_t t_conv, t_show, t_all;
        static LARGE_INTEGER t_last;
        extern uint64_t gpu_prof_sync_n, gpu_prof_sync_t, gpu_prof_late_n;
        if (prof < 0) prof = getenv("RUGRATS_PROF") != NULL;
        LARGE_INTEGER a, b, c;
        QueryPerformanceCounter(&a);
        if (prof && t_last.QuadPart) t_all += (uint64_t)(a.QuadPart - t_last.QuadPart);
        /* 32-bit frame only when something needs it (GDI, movies, test shots, the menu) */
        if (!direct_frame() || bench_shot || state_shot_now || (test_frames && (frame_no + 1) % test_every == 0)) convert_frame();
        if (state_shot_now) { state_shot_now = 0; save_named_bmp(state_shot_name); }
        QueryPerformanceCounter(&b);
        {   /* RUGRATS_LAGTEST=<frame from>:<frame to>:<percent>[:<ms, default 9>],... (debug, limiter tests): that
               share of frames spends the extra ms before the present (9 = misses the next refresh at 120 Hz) */
            static int ln = -1, lf[8], lt[8], lp[8], lm[8];
            if (ln < 0) {
                ln = 0;
                const char *e = getenv("RUGRATS_LAGTEST");
                while (e && *e && ln < 8) {
                    int used = 0;
                    if (sscanf(e, "%d:%d:%d%n", &lf[ln], &lt[ln], &lp[ln], &used) < 3 || !used) break;
                    e += used; lm[ln] = 9;
                    if (*e == ':') { used = 0; sscanf(e + 1, "%d%n", &lm[ln], &used); e += 1 + used; }
                    ln++; if (*e == ',') e++;
                }
            }
            for (int i = 0; i < ln; i++)
                if (frame_no >= lf[i] && frame_no < lt[i] && (int)((unsigned)frame_no * 37u % 100u) < lp[i]) {
                    LARGE_INTEGER s, t;
                    QueryPerformanceCounter(&s);
                    do QueryPerformanceCounter(&t); while ((t.QuadPart - s.QuadPart) * 1000 < lm[i] * qpf.QuadPart);
                    break;
                }
        }
        show();
        QueryPerformanceCounter(&c);
        {   /* RUGRATS_SLOWLOG=<ms> (debug): each frame over that many ms (default 11), split into game work since the
               last present (guest code, software raster) and show (D3D draws + Present) */
            static double lim = -1;
            if (lim < 0) { const char *e = getenv("RUGRATS_SLOWLOG"); lim = e ? (atof(e) > 0 ? atof(e) : 11) : 0; }
            if (lim > 0 && t_last.QuadPart) {
                const double k = 1000.0 / (double)qpf.QuadPart, all = (c.QuadPart - t_last.QuadPart) * k;
                if (all > lim) rt_logf("slow frame %u: %.1f ms = game %.1f + convert %.1f + show %.1f\n", frame_no, all,
                                       (a.QuadPart - t_last.QuadPart) * k, (b.QuadPart - a.QuadPart) * k, (c.QuadPart - b.QuadPart) * k);
            }
        }
        {   /* RUGRATS_FRAMEPROF=<from>,<to> (debug): per present in that frame range, game ms, game frames (pacing calls),
               GTE projections, polygons, list sends and their ms */
            extern unsigned fp_rtp, fp_poly, fp_pace, fp_dma_n; extern double fp_dma_ms;
            static int f0 = -2, f1;
            if (f0 == -2) { const char *e = getenv("RUGRATS_FRAMEPROF"); f0 = -1; if (e) sscanf(e, "%d,%d", &f0, &f1); }
            if (f0 >= 0 && (int)frame_no >= f0 && (int)frame_no <= f1 && t_last.QuadPart) {
                const double k = 1000.0 / (double)qpf.QuadPart;
                rt_logf("fp %u: game %.2f show %.2f | pace %u rtp %u poly %u sends %u (%.2f ms)\n", frame_no,
                        (a.QuadPart - t_last.QuadPart) * k, (c.QuadPart - b.QuadPart) * k, fp_pace, fp_rtp, fp_poly, fp_dma_n, fp_dma_ms);
            }
            fp_rtp = fp_poly = fp_pace = fp_dma_n = 0; fp_dma_ms = 0;
        }
        t_last = c;
        t_conv += (uint64_t)(b.QuadPart - a.QuadPart);
        t_show += (uint64_t)(c.QuadPart - b.QuadPart);
        if (prof && frame_no % 300 == 299) {
            double ms = 1000.0 / (double)qpf.QuadPart / 300;
            rt_logf("prof: frame %.2f ms | convert %.2f | show %.2f | sync wait %.2f (%.1f/frame) | late %.0f%%\n", t_all * ms,
                    t_conv * ms, t_show * ms, gpu_prof_sync_t * ms, gpu_prof_sync_n / 300.0, gpu_prof_late_n / 3.0);
            t_conv = t_show = t_all = 0;
            gpu_prof_sync_n = gpu_prof_sync_t = gpu_prof_late_n = 0;
        }
    }
    int synced = vsync_interval() > 0;
    frame_no++;
    {
        WORD w = xin_buttons();
        if ((w & XIN_TOGGLE) && !(xin_prev_game & XIN_TOGGLE)) menu_open = 1;
        xin_prev_game = w;
    }
    test_t60 += 60.0 / PRESENT_FPS;
    if (test_frames) { /* RUGRATS_STATE=save<slot>@<t60>,load<slot>@<t60>,...: scripted quick saves/loads (tests) */
        static char ops[16][8]; static int at[16], slot[16], nops = -1;
        if (nops < 0) {
            nops = 0;
            const char *e = getenv("RUGRATS_STATE");
            while (e && *e && nops < 16) {
                char op[8] = ""; int sl = 1, t = 0, used = 0;
                if (sscanf(e, "%4[a-z]%d@%d%n", op, &sl, &t, &used) < 3 || !used) break;
                snprintf(ops[nops], sizeof ops[nops], "%s", op); slot[nops] = sl; at[nops] = t; nops++;
                e += used;
                if (*e == ',') e++;
            }
        }
        for (int i = 0; i < nops; i++)
            if (at[i] >= 0 && test_t60 >= at[i]) {
                rt_logf("test: %s slot %d at test frame %.0f\n", ops[i], slot[i], test_t60);
                state_request(!strcmp(ops[i], "save"), slot[i]);
                at[i] = -1;
            }
    }
    if (state_shot_at > 0 && ++state_shot_n == state_shot_at) state_shot_now = 1; /* RUGRATS_STATE_SHOT */
    if (test_frames) { /* RUGRATS_SWITCH=<t60>:<internal>:<renderer>,...: change resolution/renderer live (tests) */
        static int n = -1, at[8], res[8], rend[8];
        if (n < 0) {
            n = 0;
            const char *e = getenv("RUGRATS_SWITCH");
            while (e && *e && n < 8) {
                int used = 0;
                if (sscanf(e, "%d:%d:%d%n", &at[n], &res[n], &rend[n], &used) < 3 || !used) break;
                n++;
                e += used;
                if (*e == ',') e++;
            }
        }
        for (int i = 0; i < n; i++)
            if (at[i] >= 0 && test_t60 >= at[i]) {
                opt.internal = res[i]; opt.renderer = rend[i];
                ap_res();
                rt_logf("test: switch to %dx %s at t60 %.0f\n", gpu_scale(), gpu_hw_on() ? "gpu" : "cpu", test_t60);
                at[i] = -1;
            }
    }
    if (bench_measure) { /* real time per presented frame (guest + drawing + present) */
        static LARGE_INTEGER prev;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = prev.QuadPart ? (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)qpf.QuadPart : 0;
        prev = now;
        bench_step(dt);
    }
    if (test_frames && frame_no == menu_test_frame) {
        if (getenv("RUGRATS_MENU_ESC")) confirm_open(0);
        else menu_open = 1;
    }
    if (menu_open) menu_run();
    if (test_frames) {
        static int shot_from = -1; /* RUGRATS_SHOTFROM=<frame>: no shots before that frame (debug) */
        if (shot_from < 0) { const char *e = getenv("RUGRATS_SHOTFROM"); shot_from = e ? atoi(e) : 0; }
        if (frame_no % test_every == 0 && frame_no >= shot_from) dump_bmp();
        if (frame_no >= test_frames) { rt_logf("test done at frame %d\n", frame_no); exit(0); }
        if (!test_paced) return;
    }
    /* pace to 59.94 Hz unless fast-forward (Tab held) */
    LONGLONG slept = 0;
    if (synced) {
        QueryPerformanceCounter(&last_frame); /* Present() waited for the display */
    } else if (!keys[VK_TAB]) {
        /* NTSC vblank is 59.94 Hz; presentation rate is opt.fps scaled the same way */
        LONGLONG target = last_frame.QuadPart + qpf.QuadPart * 1001 / ((LONGLONG)PRESENT_FPS * 1000);
        LARGE_INTEGER now;
        LARGE_INTEGER s0;
        QueryPerformanceCounter(&s0);
        for (;;) {
            QueryPerformanceCounter(&now);
            LONGLONG left = target - now.QuadPart;
            if (left <= 0) break;
            if (left * 1000 / qpf.QuadPart > 2) Sleep(1);
        }
        slept = now.QuadPart - s0.QuadPart;
        if (now.QuadPart - target > qpf.QuadPart / 10) last_frame = now; /* fell behind: resync */
        else last_frame.QuadPart = target;
    } else {
        QueryPerformanceCounter(&last_frame);
    }
    auto_rate(slept);
}

/* ---- config + entry --------------------------------------------------- */
static void exe_dir(char *out, size_t n) {
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    if (s) *s = 0;
}

static void load_config(void) {
    char dir[1024];
    const char *ini = ini_path;
    exe_dir(dir, sizeof dir);
    snprintf(ini_path, sizeof ini_path, "%s\\rugrats.ini", dir);
    opt.volume = GetPrivateProfileIntA("audio", "volume", 100, ini);
    opt.vol_music = GetPrivateProfileIntA("audio", "music", 100, ini);
    opt.vol_sfx = GetPrivateProfileIntA("audio", "sfx", 100, ini);
    opt.vol_voice = GetPrivateProfileIntA("audio", "dialogue", 100, ini);
    { int *vv[3] = { &opt.vol_music, &opt.vol_sfx, &opt.vol_voice };
      for (int i = 0; i < 3; i++) { if (*vv[i] < 0) *vv[i] = 0; if (*vv[i] > 100) *vv[i] = 100; } }
    opt.fullscreen = GetPrivateProfileIntA("video", "fullscreen", 0, ini);
    opt.vsync = GetPrivateProfileIntA("video", "vsync", 1, ini);
    opt.smooth = GetPrivateProfileIntA("video", "smooth", 0, ini);
    opt.aa = GetPrivateProfileIntA("video", "aa", 1, ini);   /* 0 off, 1 downsample (default), 2 + FXAA */
    if (opt.aa < 0 || opt.aa > 2) opt.aa = 1;
    opt.aniso = GetPrivateProfileIntA("video", "aniso", 16, ini);   /* 1 off, 2 / 4 / 8 / 16 (default) */
    if (opt.aniso < 1 || opt.aniso > 16) opt.aniso = 16;
    opt.msaa = GetPrivateProfileIntA("video", "msaa", 1, ini);   /* 1 off (default), 2 / 4 / 8 */
    if (opt.msaa < 1 || opt.msaa > 8) opt.msaa = 1;
    opt.internal = GetPrivateProfileIntA("video", "internal", 2, ini);
    opt.subpixel = GetPrivateProfileIntA("video", "subpixel", 1, ini); /* 0 off, 1 near only (default), 2 on */
    if (opt.subpixel < 0 || opt.subpixel > 2) opt.subpixel = 1;
    opt.renderer = GetPrivateProfileIntA("video", "renderer", 1, ini) != 0;
    opt.late_present = GetPrivateProfileIntA("video", "overlap", 0, ini) != 0;
    show_fps = GetPrivateProfileIntA("video", "fps_counter", 0, ini) != 0;
    opt.rcam = GetPrivateProfileIntA("input", "rstick_camera", 1, ini);
    if (opt.rcam < 0 || opt.rcam > 2) opt.rcam = 1;
    opt.rcam_invert = GetPrivateProfileIntA("input", "rstick_invert", 0, ini) != 0;
    opt.golf_aim = GetPrivateProfileIntA("game", "golf_aim", 100, ini);
    opt.chatter = GetPrivateProfileIntA("audio", "chatter", 0, ini);
    if (opt.chatter < 0 || opt.chatter > 3) opt.chatter = 0;
    opt.note_fade = GetPrivateProfileIntA("audio", "note_fade", 1, ini) != 0;
    opt.voice_restore = GetPrivateProfileIntA("audio", "voice_restore", 1, ini) != 0;
    opt.voice_hub = GetPrivateProfileIntA("audio", "voice_hub", 1, ini) != 0;
    opt.voice_golf = GetPrivateProfileIntA("audio", "voice_golf", 1, ini) != 0;
    opt.view_ext = GetPrivateProfileIntA("video", "view_ext", 2, ini);   /* default Medium (user 2026-11-18) */
    if (opt.view_ext < 0 || opt.view_ext > 3) opt.view_ext = 2;
    opt.bar_roar = GetPrivateProfileIntA("audio", "bar_roar", 1, ini);
    if (opt.bar_roar < 0 || opt.bar_roar > 2) opt.bar_roar = 1;
    opt.wall_bonk = GetPrivateProfileIntA("game", "wall_bonk", 1, ini) != 0;
    opt.shot_walk = GetPrivateProfileIntA("game", "shot_walk", 1, ini) != 0;
    opt.cut_smooth = GetPrivateProfileIntA("video", "cut_smooth", 1, ini) != 0;
    /* "Let There Be Light" Fixes; an ini from before the merge (2026-11-19) may only have the old Ceiling Fan key */
    opt.light_fixes = GetPrivateProfileIntA("game", "light_fixes", GetPrivateProfileIntA("game", "fan_still", 1, ini), ini) != 0;
    opt.fan_still = opt.night_furn = opt.hedge_leaves = opt.kitchen_arch = 1;
    opt.final_level = GetPrivateProfileIntA("game", "final_level", 1, ini) != 0;
    opt.rcam_override = GetPrivateProfileIntA("input", "rcam_override", 0, ini) != 0;
    bind_load(ini);
    if (opt.golf_aim < 0) opt.golf_aim = 0;
    if (opt.golf_aim > 400) opt.golf_aim = 400;
    opt.skip_intro = GetPrivateProfileIntA("game", "skip_intro", 0, ini) != 0;
    opt.tex_dump = GetPrivateProfileIntA("textures", "dump", 0, ini) != 0;
    opt.discord = GetPrivateProfileIntA("discord", "enabled", 1, ini) != 0;
    GetPrivateProfileStringA("discord", "client_id", "", opt.discord_id, sizeof opt.discord_id, ini);
    GetPrivateProfileStringA("discord", "image", "", opt.discord_image, sizeof opt.discord_image, ini);
    state_slot = GetPrivateProfileIntA("game", "slot", 1, ini);
    if (state_slot < 1 || state_slot > STATE_SLOTS) state_slot = 1;
    GetPrivateProfileStringA("game", "disc", "", opt.disc, sizeof opt.disc, ini);
    opt.widescreen = GetPrivateProfileIntA("video", "widescreen", 1, ini);
    opt.stretch2d = GetPrivateProfileIntA("video", "stretch2d", 1, ini) != 0;
    opt.widecull = GetPrivateProfileIntA("video", "widecull", 1, ini) != 0;
    opt.depthfix = GetPrivateProfileIntA("video", "depthfix", 1, ini) != 0;
    opt.dupfix = GetPrivateProfileIntA("video", "dupfix", 1, ini) != 0;
    opt.objsort = GetPrivateProfileIntA("video", "objsort", 1, ini) != 0;
    opt.perspective = GetPrivateProfileIntA("video", "perspective", 1, ini) != 0;
    opt.dither = GetPrivateProfileIntA("video", "dither", 1, ini);
    if (opt.dither < 0 || opt.dither > 2) opt.dither = 1;
    opt.scale = GetPrivateProfileIntA("video", "scale", 3, ini);
    opt.fps = GetPrivateProfileIntA("video", "fps", 60, ini);
    if (opt.fps < 30) opt.fps = 30;
    if (opt.fps > 360) opt.fps = 360;
    opt.overclock = GetPrivateProfileIntA("video", "overclock", 4, ini);
    opt.fixed_timestep = GetPrivateProfileIntA("video", "realtime_step", 1, ini);
    if (opt.scale < 1) opt.scale = 1;
    snprintf(opt.saves, sizeof opt.saves, "%s\\saves", dir);
    CreateDirectoryA(opt.saves, NULL);
}

/* RUGRATS_SAMPLE=<seconds> (debug profiler): after that delay, a thread samples the main thread's instruction pointer
   every ~1 ms; at exit "samples.txt" in the exe folder gets "<rva hex> <count>" lines (map with an unstripped link). */
static HANDLE smp_thread_main;
static uint32_t *smp_rva;
static int smp_n, smp_cap = 1 << 20, smp_delay;
static char smp_path[1100];
static DWORD WINAPI smp_run(LPVOID p) {
    (void)p;
    Sleep((DWORD)smp_delay * 1000);
    const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    timeBeginPeriod(1);
    while (smp_n < smp_cap) {
        Sleep(1);
        if (SuspendThread(smp_thread_main) == (DWORD)-1) break;
        CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(smp_thread_main, &c)) smp_rva[smp_n++] = (uint32_t)(c.Rip - base);
        ResumeThread(smp_thread_main);
    }
    return 0;
}
static void smp_dump(void) {
    FILE *f = fopen(smp_path, "w");
    if (!f) return;
    for (int i = 0; i < smp_n; i++) fprintf(f, "%x\n", smp_rva[i]);
    fclose(f);
}
static void smp_start(const char *dir) {
    const char *e = getenv("RUGRATS_SAMPLE");
    if (!e) return;
    smp_delay = atoi(e);
    smp_rva = malloc(sizeof *smp_rva * (size_t)smp_cap);
    snprintf(smp_path, sizeof smp_path, "%s\\samples.txt", dir);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &smp_thread_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
    atexit(smp_dump);
    CreateThread(NULL, 0, smp_run, NULL, 0, NULL);
}

int main(int argc, char **argv) {
    char dir[1024], logp[1100];
    exe_dir(dir, sizeof dir);
    snprintf(logp, sizeof logp, "%s\\rugrats.log", dir);
    rt_log = fopen(logp, "w");
    smp_start(dir);
    load_config();
    mods_init(dir, ini_path);
    {
        const char *e;
        if ((e = getenv("RUGRATS_FPS"))) opt.fps = atoi(e);
        if ((e = getenv("RUGRATS_OC"))) opt.overclock = atoi(e);
        if ((e = getenv("RUGRATS_STEP"))) opt.fixed_timestep = atoi(e);
        if ((e = getenv("RUGRATS_RES"))) opt.internal = atoi(e);
        if ((e = getenv("RUGRATS_SUBPX"))) { opt.subpixel = atoi(e); if (opt.subpixel < 0 || opt.subpixel > 2) opt.subpixel = 1; }
        if ((e = getenv("RUGRATS_RENDERER"))) opt.renderer = atoi(e) != 0;
        if ((e = getenv("RUGRATS_WIDECULL"))) opt.widecull = atoi(e) != 0;
        if ((e = getenv("RUGRATS_STRETCH2D"))) opt.stretch2d = atoi(e) != 0;
        if ((e = getenv("RUGRATS_DEPTHFIX"))) opt.depthfix = atoi(e) != 0;
        if ((e = getenv("RUGRATS_DUPFIX"))) opt.dupfix = atoi(e) != 0;
        if ((e = getenv("RUGRATS_OBJSORT"))) opt.objsort = atoi(e) != 0;
        if ((e = getenv("RUGRATS_PERSP"))) opt.perspective = atoi(e) != 0;
        if ((e = getenv("RUGRATS_MSAA"))) { opt.msaa = atoi(e); if (opt.msaa < 1 || opt.msaa > 8) opt.msaa = 1; }
        if ((e = getenv("RUGRATS_ANISO"))) { opt.aniso = atoi(e); if (opt.aniso < 1 || opt.aniso > 16) opt.aniso = 1; }
        if ((e = getenv("RUGRATS_AA"))) { opt.aa = atoi(e); if (opt.aa < 0 || opt.aa > 2) opt.aa = 1; }
        if ((e = getenv("RUGRATS_DITHER"))) { opt.dither = atoi(e); if (opt.dither < 0 || opt.dither > 2) opt.dither = 1; }
        if ((e = getenv("RUGRATS_OVERLAP"))) opt.late_present = atoi(e) != 0;
        if ((e = getenv("RUGRATS_RCAM"))) opt.rcam = atoi(e);
        if ((e = getenv("RUGRATS_GOLFAIM"))) opt.golf_aim = atoi(e);
        if ((e = getenv("RUGRATS_CHATTER"))) opt.chatter = atoi(e) & 3;
        if ((e = getenv("RUGRATS_NOTEFADE"))) opt.note_fade = atoi(e) != 0;
        if ((e = getenv("RUGRATS_RESTORE"))) opt.voice_restore = atoi(e) != 0;
        if ((e = getenv("RUGRATS_HUBLINES"))) opt.voice_hub = atoi(e) != 0;
        if ((e = getenv("RUGRATS_GOLFVOX"))) opt.voice_golf = atoi(e) != 0;
        if ((e = getenv("RUGRATS_VIEWEXT"))) opt.view_ext = atoi(e) & 3;
        if ((e = getenv("RUGRATS_BARROAR"))) { opt.bar_roar = atoi(e); if (opt.bar_roar < 0 || opt.bar_roar > 2) opt.bar_roar = 1; }
        if ((e = getenv("RUGRATS_RCAMOVR"))) opt.rcam_override = atoi(e) != 0;
        if ((e = getenv("RUGRATS_WALLBONK"))) opt.wall_bonk = atoi(e) != 0;
        if ((e = getenv("RUGRATS_SHOTWALK"))) opt.shot_walk = atoi(e) != 0;
        if ((e = getenv("RUGRATS_CUTSMOOTHOPT"))) opt.cut_smooth = atoi(e) != 0;
        if ((e = getenv("RUGRATS_LIGHTFIXES"))) opt.light_fixes = atoi(e) != 0;
        if ((e = getenv("RUGRATS_FANSTILL"))) opt.fan_still = atoi(e) != 0;     /* debug: one part alone */
        if ((e = getenv("RUGRATS_NIGHTFURN"))) opt.night_furn = atoi(e) != 0;
        if ((e = getenv("RUGRATS_HEDGELEAVES"))) opt.hedge_leaves = atoi(e) != 0;
        if ((e = getenv("RUGRATS_KITCHENARCH"))) opt.kitchen_arch = atoi(e) != 0;
        if ((e = getenv("RUGRATS_FINALLEVEL"))) opt.final_level = atoi(e) != 0;
        if ((e = getenv("RUGRATS_VOLS"))) sscanf(e, "%d,%d,%d", &opt.vol_music, &opt.vol_sfx, &opt.vol_voice); /* music,sfx,dialogue */
        if ((e = getenv("RUGRATS_SKIPINTRO"))) opt.skip_intro = atoi(e) != 0;
        const char *t = getenv("RUGRATS_TEST");
        if (t && sscanf(t, "%d,%d", &test_frames, &test_every) >= 1 && test_every <= 0) test_every = 60;
        test_paced = getenv("RUGRATS_PACED") != NULL;
        if ((e = getenv("RUGRATS_BENCH")) && sscanf(e, "%d,%d", &bench_settle, &bench_measure) == 2 && bench_measure > 0) {
            if (!test_frames) { test_frames = 1 << 30; test_every = 1 << 30; } /* unpaced test run, no shots */
            if ((e = getenv("RUGRATS_LEVEL"))) bench_level = atoi(e);
            if ((e = getenv("RUGRATS_BENCH_TIMEOUT"))) bench_timeout = atoi(e);
            bench_ms = malloc(sizeof(float) << 20);
            if (!bench_ms) bench_measure = 0;
        } else {
            bench_measure = 0;
        }
        if ((e = getenv("RUGRATS_MENU"))) menu_test_frame = atoi(e);
    }
    if (argc > 1) snprintf(opt.disc, sizeof opt.disc, "%s", argv[1]);
    if (!opt.disc[0]) {
        MessageBoxA(NULL, "Set disc= in rugrats.ini to your Rugrats (USA) .bin file.", "Rugrats", MB_ICONERROR);
        return 1;
    }
    host_init();
    gpu_init();
    gpu_set_scale(opt.internal);
    opt.internal = gpu_scale();
    dma_init();
    spu_init();
    cd_init(opt.disc);
    { void mods_disc_init(void); mods_disc_init(); }   /* needs the disc: replaced files */
    { void mods_tex_init(int); mods_tex_init(opt.tex_dump); }   /* texture packs (texpack.c) */
    bios_init();
    { void discord_start(void); discord_start(); }
    rt_boot();
    return 0;
}
