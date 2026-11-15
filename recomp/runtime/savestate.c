/* Save states that survive future builds.

   The guest runs as native C calls, so a snapshot can't include the host call stack. Instead a state is
   only taken at one well-defined point: the start of the frame pacing call (8001ceb4) during gameplay, when
   the guest call chain is exactly
       boot 800977e8 -> main 800185f8 -> main loop 80017c0c -> level loop 80018c38 -> 8001d3e8 -> 8001d00c
   (checked by walking the guest stack with each function's known frame layout). Everything else is guest
   state: RAM, CPU/GTE registers and the emulated devices, written as named fields (savestate.h).

   Loading discards the host stack (longjmp to state_boot), restores the guest state and re-enters the chain
   bottom-up: dispatch 8001ceb4, and every time a function returns, continue at the guest's return address
   (cpu.r[31]). Those return addresses are recompiler entry points (RESUME_POINTS in tools/recomp.py). All of
   this uses guest addresses only, which the disc fixes, so states load in any future build that keeps the
   resume points and the field names.

   File: <saves>\rugrats_slotN.state = header + records { u16 name length, name, u32 size, data }. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "hw.h"
#include "savestate.h"

#define STATE_MAGIC "RUGRATS-STATE\r\n"  /* 16 bytes with the terminating 0 */
#define STATE_VERSION 1                   /* bump only for incompatible header changes */
#define RESUME_PC 0x8001ceb4u

/* the guest call chain at the safe point: {function, frame size, ra offset, expected return address} */
static const struct { uint32_t fn, frame, ra_off, ret; } chain[] = {
    { 0x8001d00c, 0x18, 0x14, 0x8001d400 },   /* 8001d00c, called from 8001d3e8 */
    { 0x8001d3e8, 0x18, 0x14, 0x80019038 },   /* 8001d3e8, called from the level loop */
    { 0x80018c38, 0x30, 0x2c, 0x80018208 },   /* level loop, called from the main loop */
    { 0x80017c0c, 0x150, 0x14c, 0x80018828 }, /* main loop, called from main */
    { 0x800185f8, 0x30, 0x28, 0x8009788c },   /* main, called from the boot entry */
};
#define CHAIN_N ((int)(sizeof chain / sizeof chain[0]))

typedef struct {
    char magic[16];
    uint32_t version, header_size;
    uint32_t game_crc;         /* CRC32 of the game's EXE (same disc = same game code) */
    uint32_t resume_pc;
    uint32_t chain_ret[8];     /* expected return addresses, for diagnostics */
    uint32_t nrec;
    uint32_t reserved[7];
    char build[32];            /* build that wrote it (information only) */
} Header;

struct StateIO {
    int loading;
    char prefix[32];
    /* saving */
    uint8_t *buf; size_t len, cap; uint32_t nrec;
    /* loading */
    const uint8_t *data; size_t size;
    struct Rec { const char *name; uint16_t nlen; const uint8_t *p; uint32_t n; } *recs;
    int nrecs, *hash; int hsize;
    int missing, resized;
};

char state_msg[128];
uint32_t state_msg_time;
static int req_save = -1, req_load = -1;
static jmp_buf boot_jb;
static int boot_jb_ok;

static void msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state_msg, sizeof state_msg, fmt, ap);
    va_end(ap);
    state_msg_time = GetTickCount();
    rt_logf("state: %s\n", state_msg);
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
extern const unsigned char game_exe[];
extern const unsigned int game_exe_size;
static uint32_t game_crc(void) {
    static uint32_t c;
    if (!c) c = crc32(game_exe, game_exe_size);
    return c;
}

static uint32_t hashs(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

/* ---- field I/O ---------------------------------------------------------- */
int st_loading(StateIO *s) { return s->loading; }

void st_prefix(StateIO *s, const char *prefix) { snprintf(s->prefix, sizeof s->prefix, "%s", prefix); }

static void put(StateIO *s, const void *p, size_t n) {
    if (s->len + n > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 1 << 22;
        while (c < s->len + n) c *= 2;
        uint8_t *b = realloc(s->buf, c);
        if (!b) return;
        s->buf = b; s->cap = c;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
}

void st_field(StateIO *s, const char *name, void *p, size_t n) {
    char key[160];
    int kl = snprintf(key, sizeof key, "%s.%s", s->prefix, name);
    if (kl >= (int)sizeof key) kl = sizeof key - 1;
    if (!s->loading) {
        uint16_t l = (uint16_t)kl;
        uint32_t sz = (uint32_t)n;
        put(s, &l, 2); put(s, key, l); put(s, &sz, 4); put(s, p, n);
        s->nrec++;
        return;
    }
    uint32_t h = hashs(key, (size_t)kl) & (uint32_t)(s->hsize - 1);
    for (;;) {
        int i = s->hash[h];
        if (i < 0) { /* not in this state (older build): keep the current value */
            if (s->missing++ < 20) rt_logf("state: no field %s in the file, kept\n", key);
            return;
        }
        if (s->recs[i].nlen == kl && memcmp(s->recs[i].name, key, (size_t)kl) == 0) {
            uint32_t m = s->recs[i].n < n ? s->recs[i].n : (uint32_t)n;
            memcpy(p, s->recs[i].p, m);
            if (s->recs[i].n != n && s->resized++ < 20)
                rt_logf("state: field %s was %u bytes, now %u\n", key, s->recs[i].n, (unsigned)n);
            return;
        }
        h = (h + 1) & (uint32_t)(s->hsize - 1);
    }
}

static void all_devices(StateIO *s) {
    hw_state(s);      /* CPU, RAM, scheduler, IRQ, DMA, timers */
    gte_state(s);
    bios_state(s);
    gpu_state(s);
    cd_state(s);
    spu_state(s);
    sio_state(s);
    mdec_state(s);
    hooks_state(s);
    { void tex_state(StateIO *); tex_state(s); }    /* texpack.c: VRAM regions (checked against VRAM) */
    { void mods_state(StateIO *); mods_state(s); }   /* last: may add a warning to state_msg */
    host_state(s);    /* tests only: scripted-input clock */
}

static void slot_path(int slot, char *out, size_t n) { snprintf(out, n, "%s\\rugrats_slot%d.state", opt.saves, slot); }

int state_slot_exists(int slot) {
    char p[1200];
    slot_path(slot, p, sizeof p);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

void state_request(int save, int slot) {
    if (save) req_save = slot; else req_load = slot;
}

/* ---- save ------------------------------------------------------------- */
int hw_in_exception(void);
static const char *chain_check(void) {
    if (hw_in_exception()) return "inside an interrupt";
    if (cpu.r[31] != 0x8001d0a8u) return "not the frame pacing call";
    uint32_t sp = cpu.r[29];
    for (int i = 0; i < CHAIN_N; i++) {
        if ((sp & 0x1FFFFF) + chain[i].frame > 0x200000) return "stack out of range";
        if (mem_r32(sp + chain[i].ra_off) != chain[i].ret) return i < 2 ? "not a gameplay frame" : "unexpected call chain";
        sp += chain[i].frame;
    }
    return NULL;
}

static void do_save(int slot) {
    const char *why = chain_check();
    if (why) {
        static uint32_t last_try;
        (void)last_try;
        msg("Can't save here (%s) - try again in gameplay", why);
        return;
    }
    void gpu_hr_sync(void);
    gpu_hr_sync();
    StateIO s;
    memset(&s, 0, sizeof s);
    Header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, STATE_MAGIC, 16);
    h.version = STATE_VERSION;
    h.header_size = sizeof h;
    h.game_crc = game_crc();
    h.resume_pc = RESUME_PC;
    for (int i = 0; i < CHAIN_N; i++) h.chain_ret[i] = chain[i].ret;
    snprintf(h.build, sizeof h.build, "%s %s", __DATE__, __TIME__);
    put(&s, &h, sizeof h);
    all_devices(&s);
    if (!s.buf) { msg("Save failed (out of memory)"); return; }
    ((Header *)s.buf)->nrec = s.nrec;
    char p[1200], tmp[1210];
    slot_path(slot, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(s.buf, 1, s.len, f) == s.len;
    if (f) ok &= fclose(f) == 0;
    if (ok) ok = MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING) != 0; /* never leave a half-written slot */
    free(s.buf);
    if (ok) { msg("Saved to slot %d", slot); host_state_done(0); }
    else { DeleteFileA(tmp); msg("Save to slot %d failed (can't write %s)", slot, p); }
}

/* ---- load ------------------------------------------------------------- */
static uint8_t *load_buf;      /* the file being loaded (kept until the restore is done) */
static size_t load_len;

static int parse(StateIO *s, const uint8_t *d, size_t n) {
    memset(s, 0, sizeof *s);
    s->loading = 1;
    if (n < sizeof(Header)) return 0;
    const Header *h = (const Header *)d;
    size_t off = h->header_size;
    if (off < 64 || off > n) return 0;
    int cap = 64;
    s->recs = malloc(sizeof *s->recs * (size_t)cap);
    while (s->recs && off + 6 <= n) {
        uint16_t l;
        uint32_t sz;
        memcpy(&l, d + off, 2);
        if (off + 2 + l + 4 > n) break;
        memcpy(&sz, d + off + 2 + l, 4);
        if (off + 6 + l + sz > n) break;
        if (s->nrecs == cap) { cap *= 2; void *r = realloc(s->recs, sizeof *s->recs * (size_t)cap); if (!r) break; s->recs = r; }
        s->recs[s->nrecs].name = (const char *)d + off + 2;
        s->recs[s->nrecs].nlen = l;
        s->recs[s->nrecs].p = d + off + 6 + l;
        s->recs[s->nrecs].n = sz;
        s->nrecs++;
        off += 6 + (size_t)l + sz;
    }
    s->hsize = 1;
    while (s->hsize < s->nrecs * 2 + 16) s->hsize *= 2;
    s->hash = malloc(sizeof(int) * (size_t)s->hsize);
    if (!s->recs || !s->hash) return 0;
    for (int i = 0; i < s->hsize; i++) s->hash[i] = -1;
    for (int i = 0; i < s->nrecs; i++) {
        uint32_t k = hashs(s->recs[i].name, s->recs[i].nlen) & (uint32_t)(s->hsize - 1);
        while (s->hash[k] >= 0) k = (k + 1) & (uint32_t)(s->hsize - 1);
        s->hash[k] = i;
    }
    return 1;
}

static void do_load(int slot) {
    char p[1200];
    slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) { msg("Slot %d is empty", slot); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    int ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(d); msg("Slot %d: can't read %s", slot, p); return; }
    const Header *h = (const Header *)d;
    const char *bad = NULL;
    if ((size_t)n < sizeof(Header) || memcmp(h->magic, STATE_MAGIC, 16) != 0) bad = "not a save state";
    else if (h->version > STATE_VERSION) bad = "made by a newer build";
    else if (h->game_crc != game_crc()) bad = "made for a different game EXE";
    else if (h->resume_pc != RESUME_PC) bad = "unknown resume point";
    if (bad) { free(d); msg("Slot %d: %s", slot, bad); return; }
    if (!boot_jb_ok) { free(d); msg("Load not possible yet"); return; }
    free(load_buf);
    load_buf = d;
    load_len = (size_t)n;
    rt_logf("state: loading slot %d (written by build %s)\n", slot, h->build);
    snprintf(state_msg, sizeof state_msg, "Loaded slot %d", slot);
    longjmp(boot_jb, 1); /* drop the host call stack; state_boot restores and resumes */
}

void state_safe_point(void) {
    if (req_load >= 0) { int s = req_load; req_load = -1; req_save = -1; do_load(s); }
    if (req_save >= 0) { int s = req_save; req_save = -1; do_save(s); }
}

/* debug: RUGRATS_EXPORT=<dir> writes the just-loaded guest state as plain files, to replay the same moment in a
   reference emulator: ram.bin (2 MB), scratch.bin (1 KB), vram.bin (1 MB, 16-bit pixels), regs.txt (one hex word per
   line: pc, r0-r31, hi, lo, gte 0-63, then the GPU setting words from gpu_export_cmds). */
static int state_exported;
static void state_export(void) {
    const char *dir = getenv("RUGRATS_EXPORT");
    if (!dir || !*dir) return;
    state_exported = 1;
    char p[1024];
    FILE *f;
    snprintf(p, sizeof p, "%s\\ram.bin", dir);
    if ((f = fopen(p, "wb"))) { fwrite(ram, 1, 0x200000, f); fclose(f); }
    snprintf(p, sizeof p, "%s\\scratch.bin", dir);
    if ((f = fopen(p, "wb"))) { fwrite(scratch, 1, sizeof scratch, f); fclose(f); }
    snprintf(p, sizeof p, "%s\\vram.bin", dir);
    if ((f = fopen(p, "wb"))) { fwrite(vram, 2, 512 * 1024, f); fclose(f); }
    {   /* sound RAM + SPU registers (mkloader uploads them so the emulator plays the level's sounds) */
        const uint8_t *spu_export_sram(void);
        const uint16_t *spu_export_regs(void);
        snprintf(p, sizeof p, "%s\\spu_ram.bin", dir);
        if ((f = fopen(p, "wb"))) { fwrite(spu_export_sram(), 1, 0x80000, f); fclose(f); }
        snprintf(p, sizeof p, "%s\\spu_regs.bin", dir);
        if ((f = fopen(p, "wb"))) { fwrite(spu_export_regs(), 2, 0x200, f); fclose(f); }
    }
    snprintf(p, sizeof p, "%s\\regs.txt", dir);
    if ((f = fopen(p, "w"))) {
        int gpu_export_cmds(uint32_t *out);
        uint32_t cmds[16];
        int n = gpu_export_cmds(cmds);
        fprintf(f, "%08x\n", (unsigned)RESUME_PC);
        for (int i = 0; i < 32; i++) fprintf(f, "%08x\n", cpu.r[i]);
        fprintf(f, "%08x\n%08x\n", cpu.hi, cpu.lo);
        for (int i = 0; i < 64; i++) fprintf(f, "%08x\n", gte_read(i));
        for (int i = 0; i < n; i++) fprintf(f, "%08x\n", cmds[i]);
        int hw_export_regs(uint32_t *out);
        uint32_t hwr[32];
        int m = hw_export_regs(hwr);
        for (int i = 0; i < m; i++) fprintf(f, "%08x\n", hwr[i]);
        fclose(f);
    }
    rt_logf("state: exported to %s\n", dir);
}

/* debug: with RUGRATS_EXPORT and RUGRATS_EXPORT_N=<n>, also write ram_fK.bin + regs_fK.txt (pc, r0-r31, hi, lo,
   gte 0-63) at the start of each of the next n game frames after a load (called from the pacing hook) */
void state_export_frame(void) {
    static int k = -1, n;
    const char *dir = getenv("RUGRATS_EXPORT");
    if (!dir || !*dir) return;
    if (k < 0) { const char *e = getenv("RUGRATS_EXPORT_N"); n = e ? atoi(e) : 0; k = 0; }
    if (!state_exported || k >= n) return;
    k++;
    char p[1024];
    FILE *f;
    snprintf(p, sizeof p, "%s\\ram_f%d.bin", dir, k);
    if ((f = fopen(p, "wb"))) { fwrite(ram, 1, 0x200000, f); fclose(f); }
    snprintf(p, sizeof p, "%s\\regs_f%d.txt", dir, k);
    if ((f = fopen(p, "w"))) {
        fprintf(f, "%08x\n", 0x8001ceb4u);
        for (int i = 0; i < 32; i++) fprintf(f, "%08x\n", cpu.r[i]);
        fprintf(f, "%08x\n%08x\n", cpu.hi, cpu.lo);
        for (int i = 0; i < 64; i++) fprintf(f, "%08x\n", gte_read(i));
        fclose(f);
    }
}

void state_boot(void (*start)(void)) {
    if (!setjmp(boot_jb)) {
        boot_jb_ok = 1;
        start();
        return;
    }
    /* a load: restore the guest state, then resume the gameplay chain at the pacing call */
    for (;;) {
        StateIO s;
        void gpu_hr_sync(void);
        gpu_hr_sync();
        if (parse(&s, load_buf, load_len)) {
            all_devices(&s);
            if (s.missing || s.resized) rt_logf("state: %d fields missing, %d resized (older build)\n", s.missing, s.resized);
        }
        free(s.recs);
        free(s.hash);
        free(load_buf);
        load_buf = NULL;
        state_msg_time = GetTickCount();
        host_state_done(1);
        state_export();
        { void gte_log_start(void); gte_log_start(); } /* debug: RUGRATS_GTELOG */
        rt_logf("state: %s, resuming at %08x (ra %08x sp %08x)\n", state_msg, RESUME_PC, cpu.r[31], cpu.r[29]);
        if (setjmp(boot_jb)) continue; /* another load while running the resumed chain */
        uint32_t pc = RESUME_PC;
        for (;;) {          /* each function returns to the guest return address: continue there */
            rt_dispatch(pc);
            pc = cpu.r[31];
        }
    }
}
