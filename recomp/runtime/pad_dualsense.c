/* Native Sony controller input through the Windows HID API (no drivers, no Steam needed).
   Supported: DualSense (054C:0CE6), DualSense Edge (054C:0DF2), DualShock 4 (054C:05C4, 09CC, 0BA0).
   One scanner thread looks for controllers every 2 s; each controller gets a reader thread that keeps
   its latest button state. Only buttons and the left stick are used (the PS1 pad is digital). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pad_dualsense.h"

void rt_logf(const char *fmt, ...);

#define MAX_PADS 4
typedef struct {
    int used;           /* slot taken (reader running) */
    int live;           /* has delivered a report */
    uint32_t buttons;
    int rx, ry;         /* right stick, 0..255 (128 = centre) */
    HANDLE h;
    char path[512];
    const char *name;
    int bt;
} Pad;

static Pad pads[MAX_PADS];
static SRWLOCK lock = SRWLOCK_INIT;
static int dslog;

static const char *model(USHORT pid) {
    switch (pid) {
    case 0x0CE6: return "DualSense";
    case 0x0DF2: return "DualSense Edge";
    case 0x05C4: case 0x09CC: return "DualShock 4";
    case 0x0BA0: return "DualShock 4 (USB adapter)";
    }
    return NULL;
}

/* Two report layouts share the same button bytes:
   "full" (DualSense USB 0x01 / BT 0x31):         LX LY RX RY L2 R2 cnt B0 B1 B2
   "ds4"  (DS4 all modes, DualSense BT 0x01 short): LX LY RX RY B0 B1 B2 L2 R2
   B0: low nibble = d-pad hat (0 = up, clockwise, 8 = none), 0x10 square, 0x20 cross, 0x40 circle, 0x80 triangle
   B1: 0x01 L1, 0x02 R1, 0x04 L2, 0x08 R2, 0x10 share/create, 0x20 options, 0x40 L3, 0x80 R3
   B2: 0x01 PS, 0x02 touchpad click */
static int parse(const uint8_t *r, int n, int dualsense, int bt, uint32_t *out, int *rs) {
    int o, full;
    if (n < 10) return 0;
    if (r[0] == 0x31 && dualsense && n >= 12) { o = 2; full = 1; }           /* DualSense Bluetooth, full */
    else if (r[0] == 0x11 && !dualsense && n >= 12) { o = 3; full = 0; }     /* DS4 Bluetooth, full */
    else if (r[0] == 0x01 && dualsense && !bt && n >= 32) { o = 1; full = 1; } /* DualSense USB */
    else if (r[0] == 0x01) { o = 1; full = 0; }                              /* DS4 USB, or Bluetooth basic */
    else return 0;
    int lx = r[o], ly = r[o + 1];
    rs[0] = r[o + 2];
    rs[1] = r[o + 3];
    const uint8_t *b = full ? r + o + 7 : r + o + 4;
    uint32_t m = 0;
    static const uint32_t hat[8] = { DS_UP, DS_UP | DS_RIGHT, DS_RIGHT, DS_DOWN | DS_RIGHT, DS_DOWN,
                                     DS_DOWN | DS_LEFT, DS_LEFT, DS_UP | DS_LEFT };
    if ((b[0] & 15) < 8) m |= hat[b[0] & 15];
    if (b[0] & 0x10) m |= DS_SQUARE;
    if (b[0] & 0x20) m |= DS_CROSS;
    if (b[0] & 0x40) m |= DS_CIRCLE;
    if (b[0] & 0x80) m |= DS_TRIANGLE;
    if (b[1] & 0x01) m |= DS_L1;
    if (b[1] & 0x02) m |= DS_R1;
    if (b[1] & 0x04) m |= DS_L2;
    if (b[1] & 0x08) m |= DS_R2;
    if (b[1] & 0x10) m |= DS_SHARE;
    if (b[1] & 0x20) m |= DS_OPTIONS;
    if (b[1] & 0x40) m |= DS_L3;
    if (b[1] & 0x80) m |= DS_R3;
    if (b[2] & 0x01) m |= DS_PS;
    if (b[2] & 0x02) m |= DS_TOUCH;
    /* left stick as d-pad (same threshold as XInput: about half way) */
    if (ly < 64) m |= DS_UP;
    if (ly > 192) m |= DS_DOWN;
    if (lx < 64) m |= DS_LEFT;
    if (lx > 192) m |= DS_RIGHT;
    *out = m;
    return 1;
}

static DWORD WINAPI reader(LPVOID arg) {
    Pad *p = arg;
    int dualsense = strstr(p->name, "DualSense") != NULL;
    PHIDP_PREPARSED_DATA pp;
    HIDP_CAPS caps = { 0 };
    if (HidD_GetPreparsedData(p->h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
    int len = caps.InputReportByteLength ? caps.InputReportByteLength : 128;
    uint8_t *buf = calloc(1, len + 1);
    uint32_t last = 0xFFFFFFFF;
    int reports = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(p->h, buf, len, &got, NULL) || got == 0) break; /* unplugged / switched off */
        uint32_t m;
        int rs[2];
        if (!parse(buf, (int)got, dualsense, p->bt, &m, rs)) continue;
        if (!reports++) {
            rt_logf("pad: %s connected (%s), report 0x%02x, %lu bytes\n", p->name, p->bt ? "Bluetooth" : "USB",
                    buf[0], got);
        }
        if (dslog && m != last) rt_logf("pad: %s buttons %05x\n", p->name, m);
        last = m;
        AcquireSRWLockExclusive(&lock);
        p->buttons = m;
        p->rx = rs[0];
        p->ry = rs[1];
        p->live = 1;
        ReleaseSRWLockExclusive(&lock);
    }
    rt_logf("pad: %s disconnected\n", p->name);
    free(buf);
    AcquireSRWLockExclusive(&lock);
    CloseHandle(p->h);
    p->h = NULL;
    p->live = 0;
    p->buttons = 0;
    p->used = 0;
    ReleaseSRWLockExclusive(&lock);
    return 0;
}

static int is_open(const char *path) {
    int r = 0;
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < MAX_PADS; i++) r |= pads[i].used && !_stricmp(pads[i].path, path);
    ReleaseSRWLockShared(&lock);
    return r;
}

static void scan(void) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        char raw[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + 512];
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)raw;
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, sizeof raw, NULL, NULL)) continue;
        const char *path = det->DevicePath;
        if (!strstr(path, "054c") && !strstr(path, "054C")) continue; /* Sony vendor id in the path */
        if (is_open(path)) continue;
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (dslog) rt_logf("pad: scan %s -> %s (err %lu)\n", path, h == INVALID_HANDLE_VALUE ? "no open" : "open",
                           h == INVALID_HANDLE_VALUE ? GetLastError() : 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = { sizeof at };
        const char *name = HidD_GetAttributes(h, &at) && at.VendorID == 0x054C ? model(at.ProductID) : NULL;
        if (!name) { CloseHandle(h); continue; }
        Pad *slot = NULL;
        AcquireSRWLockExclusive(&lock);
        for (int k = 0; k < MAX_PADS && !slot; k++)
            if (!pads[k].used) slot = &pads[k];
        if (slot) {
            memset(slot, 0, sizeof *slot);
            slot->used = 1;
            slot->h = h;
            slot->name = name;
            slot->bt = strstr(path, "{00001124") != NULL || strstr(path, "bth") != NULL || strstr(path, "BTH") != NULL;
            snprintf(slot->path, sizeof slot->path, "%s", path);
        }
        ReleaseSRWLockExclusive(&lock);
        if (!slot) { CloseHandle(h); break; }
        HANDLE t = CreateThread(NULL, 0, reader, slot, 0, NULL);
        if (t) CloseHandle(t);
    }
    SetupDiDestroyDeviceInfoList(set);
}

static DWORD WINAPI scanner(LPVOID arg) {
    (void)arg;
    for (;;) {
        scan();
        Sleep(2000);
    }
    return 0;
}

void ds_start(void) {
    dslog = getenv("RUGRATS_DSLOG") != NULL;
    if (getenv("RUGRATS_NODS")) return;
    HANDLE t = CreateThread(NULL, 0, scanner, NULL, 0, NULL);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
}

uint32_t ds_buttons(void) {
    uint32_t m = 0;
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i].live) m |= pads[i].buttons;
    ReleaseSRWLockShared(&lock);
    return m;
}

/* right stick of the controller pushed furthest, -128..127 each (x right, y down); 0 when none */
void ds_rstick(int *x, int *y) {
    int bx = 0, by = 0, best = -1;
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i].live) {
            int dx = pads[i].rx - 128, dy = pads[i].ry - 128, d = dx * dx + dy * dy;
            if (d > best) { best = d; bx = dx; by = dy; }
        }
    ReleaseSRWLockShared(&lock);
    *x = bx;
    *y = by;
}

int ds_count(void) {
    int n = 0;
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < MAX_PADS; i++) n += pads[i].live;
    ReleaseSRWLockShared(&lock);
    return n;
}
