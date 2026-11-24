/* Mods. Spec for mod authors: recomp/MODDING.md.
   Phase 1 (2026-11-13): RAM patches and GameShark codes. Phase 2 (2026-11-13): disc file replacement.

   A mod is a folder mods/<id>/ next to Rugrats.exe holding mod.ini:
       [mod]        name, version, author, description, api (1 = RAM/GameShark only, 2 = also files)
       [ram]        <when> <address> <u8|u16|u32> <value>     when = always | level:<db>
       [gameshark]  XXXXXXXX YYYY codes (80 30 10 11 20 21 D0-D3 E0-E3 50)
       [files]      <disc path> = <file in the mod>    replaces that disc file
       [patch]      <disc path> = <file.bps>           BPS patch applied to the disc file (data files only)
       [textures]   folder = <folder in the mod>       texture pack (texpack.c; api 3)
   Enabled mods and their order: rugrats.ini [mods] enabled=id1,id2 (later mods win: they are applied after).
   RAM codes run once per vblank (60 Hz), like a cheat cartridge. With no mod enabled nothing runs and no sector or
   table changes, so the game is bit-identical to a build without this file.
   A mod with anything we can't use (bad line, missing file, wrong patch source) is refused as a whole (logged, shown
   in F1 and the launcher), never half-applied.

   Disc overlay: the game finds every file through its own table in the EXE (0x800B2C40, 714 x {LBA, size}; size in
   bytes, or sectors x 2336 for streamed STR/XA files) plus the movie table (0x800BAE4C, 7 x {frames, LBA}); it never
   reads the ISO directory (checked with RUGRATS_TRACE=2 from boot to gameplay). A replaced file is served in its
   original sectors when it fits, else from free sectors after the end of the disc; the tables in guest RAM are
   rewritten after the EXE is loaded (and again after a quick load). Data files are given as plain bytes; streamed
   files (STR movies, XA audio: sectors with the real-time bit) as raw sectors (2352 or 2336 bytes each, as
   recomp/tools/modkit.py extract writes them). EDC/ECC are left zero (our CD model doesn't check them).
   Debug: RUGRATS_MODS=<id,id> overrides the enabled list (not saved), RUGRATS_MODDIR=<dir> the mods folder. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "hw.h"
#include "savestate.h"

#define MOD_API 3
#define MAX_MODS 64
#define MAX_OPS 4096
#define MAX_FILES 256
#define MAX_INI (256 * 1024)
#define MAX_DATA_FILE (64u << 20)       /* bytes, a replaced data file */
#define MAX_STREAM_FILE (512u << 20)    /* bytes, a replaced movie / XA stream */
#define LAST_LBA (440000u - 1)   /* icecream.c serves its files from LBA 440000 */

#define FILE_TAB 0x800B2C40u
#define FILE_TAB_N 714
#define MOVIE_TAB 0x800BAE4Cu
#define MOVIE_TAB_N 7

enum { OP_W8, OP_W16, OP_W32, OP_ADD8, OP_ADD16, OP_IF8, OP_IF16 };
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_GT };
typedef struct { uint8_t op, cmp; int16_t level; uint32_t addr, val; } ModOp;   /* addr = offset into ram[] */
typedef struct { char disc[96], src[200]; int patch, line; } ModFile;

typedef struct {
    char id[64], name[64], version[32], author[64], desc[512], err[160];
    int enabled;   /* wanted: saved to the ini, switched in F1 */
    int live;      /* running now (a mod with files only changes on restart) */
    int n_ram, n_gs, nops, nfiles, ntex;
    char texdir[200];   /* [textures] folder, "" = none */
    ModOp *ops;
    ModFile *files;
} Mod;

static Mod mods[MAX_MODS];
static int n_mods, n_active;
static char mod_dir[1024], mod_ini[1100];
extern int hooks_level;

int mods_count(void) { return n_mods; }
int mods_active(void) { return n_active; }
const char *mods_dir(void) { return mod_dir; }
const char *mods_name(int i) { return mods[i].name[0] ? mods[i].name : mods[i].id; }
const char *mods_version(int i) { return mods[i].version; }
const char *mods_author(int i) { return mods[i].author; }
const char *mods_desc(int i) { return mods[i].desc; }
const char *mods_error(int i) { return mods[i].err[0] ? mods[i].err : NULL; }
int mods_enabled(int i) { return mods[i].enabled; }
int mods_restart(int i) { return mods[i].enabled != mods[i].live; }   /* switched, takes effect after a restart */
static void plural(char *out, int n, int k, const char *what) {
    if (!k) return;
    size_t l = strlen(out);
    snprintf(out + l, n - l, "%s%d %s%s", l ? ", " : "", k, what, k == 1 ? "" : "s");
}
void mods_summary(int i, char *out, int n) {
    const Mod *m = &mods[i];
    if (m->err[0]) { snprintf(out, n, "Not loaded: %s", m->err); return; }
    out[0] = 0;
    plural(out, n, m->nfiles, "file");
    if (m->texdir[0]) { if (m->ntex) plural(out, n, m->ntex, "texture"); else plural(out, n, 1, "texture pack"); }
    plural(out, n, m->n_ram, "RAM write");
    plural(out, n, m->n_gs, "GameShark code");
    if (!out[0]) snprintf(out, n, "empty");
    if (mods_restart(i)) strncat(out, m->enabled ? "  (on after a restart)" : "  (off after a restart)",
                                 n - strlen(out) - 1);
}

static void count_active(void) {
    n_active = 0;
    for (int i = 0; i < n_mods; i++) n_active += mods[i].live;
}

/* ---- parsing ---- */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
static int valid_id(const char *s) {
    if (!*s || strlen(s) > 63) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.') return 0;
    return 1;
}
/* a path inside the mod folder: relative, no "..", no drive, no ADS / device tricks */
static int safe_rel(const char *p) {
    if (!*p || *p == '/' || *p == '\\' || strchr(p, ':')) return 0;
    const char *s = p;
    while (*s) {
        const char *e = s + strcspn(s, "/\\");
        if ((e - s == 2 && s[0] == '.' && s[1] == '.') || e == s) return 0;
        s = *e ? e + 1 : e;
    }
    return 1;
}
/* "data/db05\\x.bin;1" -> "DATA/DB05/X.BIN" */
static void norm_disc(char *p) {
    char *r = p, *w = p;
    while (*r == '/' || *r == '\\') r++;
    for (; *r && *r != ';'; r++) *w++ = (char)(*r == '\\' ? '/' : toupper((unsigned char)*r));
    *w = 0;
}
/* guest address -> ram[] offset, or -1. Accepts KUSEG/KSEG0/KSEG1 views of the 2 MB of main RAM */
static long ram_off(uint32_t a, int align) {
    uint32_t seg = a >> 24;
    if (seg != 0x00 && seg != 0x80 && seg != 0xA0) return -1;
    a &= 0xFFFFFF;
    if (a >= 0x200000 || (a & (align - 1))) return -1;
    return (long)a;
}
static int parse_num(const char *s, uint32_t *out) {
    char *e;
    if (!*s) return 0;
    if (s[0] == '-') { long v = strtol(s, &e, 0); *out = (uint32_t)v; return *e == 0; }
    unsigned long v = strtoul(s, &e, 0);
    *out = (uint32_t)v;
    return *e == 0;
}
static int add_op(Mod *m, int op, int cmp, int level, long off, uint32_t val) {
    if (m->nops >= MAX_OPS) { snprintf(m->err, sizeof m->err, "more than %d codes", MAX_OPS); return 0; }
    ModOp *o = &m->ops[m->nops++];
    o->op = (uint8_t)op; o->cmp = (uint8_t)cmp; o->level = (int16_t)level; o->addr = (uint32_t)off; o->val = val;
    return 1;
}
#define FAIL(...) do { snprintf(m->err, sizeof m->err, __VA_ARGS__); return 0; } while (0)

/* [ram]: always 800E2CEC u16 255  |  level:5 0x800C4CC0 u32 0x7FFF */
static int parse_ram(Mod *m, char *line, int ln) {
    char when[32], as[32], ty[8], vs[32];
    if (sscanf(line, "%31s %31s %7s %31s", when, as, ty, vs) != 4) FAIL("line %d: want <when> <address> <type> <value>", ln);
    int level = -1;
    if (!_stricmp(when, "always")) level = -1;
    else if (!_strnicmp(when, "level:", 6)) {
        uint32_t v;
        if (!parse_num(when + 6, &v) || v > 63) FAIL("line %d: bad level number", ln);
        level = (int)v;
    } else FAIL("line %d: <when> must be always or level:<n>", ln);
    int op, al;
    if (!_stricmp(ty, "u8") || !_stricmp(ty, "s8")) { op = OP_W8; al = 1; }
    else if (!_stricmp(ty, "u16") || !_stricmp(ty, "s16")) { op = OP_W16; al = 2; }
    else if (!_stricmp(ty, "u32") || !_stricmp(ty, "s32")) { op = OP_W32; al = 4; }
    else FAIL("line %d: type must be u8, u16 or u32", ln);
    char *e;
    uint32_t a = (uint32_t)strtoul(as, &e, 16);
    long off = *e ? -1 : ram_off(a, al);
    if (off < 0) FAIL("line %d: address %s is not in main RAM (or not aligned)", ln, as);
    uint32_t v;
    if (!parse_num(vs, &v)) FAIL("line %d: bad value %s", ln, vs);
    m->n_ram++;
    return add_op(m, op, 0, level, off, v);
}

/* [gameshark]: one code per line, XXXXXXXX YYYY. Serial repeaters (50) take the following line too. */
static int parse_gs(Mod *m, char **lines, int nl, int *i, int ln) {
    unsigned a, v;
    char extra;
    if (sscanf(lines[*i], "%8x %4x %c", &a, &v, &extra) != 2 || strlen(trim(lines[*i])) != 13)
        FAIL("line %d: GameShark codes look like 800E2CEC 00FF", ln);
    int type = a >> 24;
    long off2 = ram_off(a & 0xFFFFFF, 2), off1 = ram_off(a & 0xFFFFFF, 1);
    m->n_gs++;
    switch (type) {
    case 0x80: if (off2 < 0) break; return add_op(m, OP_W16, 0, -1, off2, v);
    case 0x30: if (off1 < 0) break; return add_op(m, OP_W8, 0, -1, off1, v & 0xFF);
    case 0x10: if (off2 < 0) break; return add_op(m, OP_ADD16, 0, -1, off2, v);
    case 0x11: if (off2 < 0) break; return add_op(m, OP_ADD16, 0, -1, off2, (uint32_t)-(int32_t)v);
    case 0x20: if (off1 < 0) break; return add_op(m, OP_ADD8, 0, -1, off1, v & 0xFF);
    case 0x21: if (off1 < 0) break; return add_op(m, OP_ADD8, 0, -1, off1, (uint32_t)-(int32_t)(v & 0xFF));
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        if (off2 < 0) break; return add_op(m, OP_IF16, type - 0xD0, -1, off2, v);
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        if (off1 < 0) break; return add_op(m, OP_IF8, type - 0xE0, -1, off1, v & 0xFF);
    case 0x50: {   /* 5000nnss iiii / 80aaaaaa vvvv (or 30): n writes, address += ss, value += iiii */
        unsigned n = (a >> 8) & 0xFF, step = a & 0xFF, b, w;
        if (*i + 1 >= nl || sscanf(lines[*i + 1], "%8x %4x", &b, &w) != 2) FAIL("line %d: serial code needs a second line", ln);
        int t2 = b >> 24, wide = t2 == 0x80;
        if (t2 != 0x80 && t2 != 0x30) FAIL("line %d: serial code must be followed by an 80 or 30 code", ln);
        (*i)++;
        for (unsigned k = 0; k < n; k++) {
            long o = ram_off((b & 0xFFFFFF) + k * step, wide ? 2 : 1);
            if (o < 0) FAIL("line %d: serial code runs outside main RAM", ln + 1);
            if (!add_op(m, wide ? OP_W16 : OP_W8, 0, -1, o, (w + k * v) & (wide ? 0xFFFF : 0xFF))) return 0;
        }
        return 1;
    }
    default:
        FAIL("line %d: GameShark code type %02X is not supported", ln, type);
    }
    FAIL("line %d: address %06X is not in main RAM (or not aligned)", ln, a & 0xFFFFFF);
}

/* [files] / [patch]: <disc path> = <file in the mod folder>. The disc path is checked later (mods_disc_init). */
static int parse_file(Mod *m, char *line, int ln, int patch) {
    char *eq = strchr(line, '=');
    if (!eq) FAIL("line %d: want <disc path> = <file in the mod>", ln);
    *eq = 0;
    char *d = trim(line), *s = trim(eq + 1);
    if (!*d || !*s || strlen(d) >= sizeof m->files[0].disc || strlen(s) >= sizeof m->files[0].src)
        FAIL("line %d: want <disc path> = <file in the mod>", ln);
    if (!safe_rel(s)) FAIL("line %d: %s must be a plain path inside the mod folder", ln, s);
    if (m->nfiles >= MAX_FILES) FAIL("more than %d files", MAX_FILES);
    ModFile *f = &m->files[m->nfiles++];
    snprintf(f->disc, sizeof f->disc, "%s", d);
    norm_disc(f->disc);
    snprintf(f->src, sizeof f->src, "%s", s);
    f->patch = patch;
    f->line = ln;
    for (int k = 0; k < m->nfiles - 1; k++)
        if (!strcmp(m->files[k].disc, f->disc)) FAIL("line %d: %s is replaced twice in this mod", ln, f->disc);
    return 1;
}

static void free_mod(Mod *m) {
    free(m->ops); m->ops = NULL; m->nops = 0;
    free(m->files); m->files = NULL; m->nfiles = 0;
}

static int load_mod(Mod *m, const char *dir) {
    char p[1300];
    snprintf(p, sizeof p, "%s\\%s\\mod.ini", dir, m->id);
    FILE *f = fopen(p, "rb");
    if (!f) FAIL("no mod.ini");
    char *buf = malloc(MAX_INI + 1);
    size_t n = buf ? fread(buf, 1, MAX_INI + 1, f) : 0;
    fclose(f);
    if (!buf) FAIL("out of memory");
    if (n > MAX_INI) { free(buf); FAIL("mod.ini is bigger than 256 KB"); }
    buf[n] = 0;
    static char *lines[MAX_OPS * 2];
    int nl = 0;
    for (char *s = buf; s && nl < MAX_OPS * 2; ) {
        char *e = strchr(s, '\n');
        if (e) *e++ = 0;
        lines[nl++] = s;
        s = e;
    }
    m->ops = calloc(MAX_OPS, sizeof(ModOp));
    m->files = calloc(MAX_FILES, sizeof(ModFile));
    if (!m->ops || !m->files) { free(buf); free_mod(m); FAIL("out of memory"); }
    int api = 0, ok = 1;
    char sec[32] = "";
    for (int i = 0; i < nl && ok; i++) {
        char *s = trim(lines[i]);
        if (i == 0 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) { snprintf(m->err, sizeof m->err, "line %d: bad section", i + 1); ok = 0; break; }
            *e = 0;
            snprintf(sec, sizeof sec, "%s", trim(s + 1));
            if (_stricmp(sec, "mod") && _stricmp(sec, "ram") && _stricmp(sec, "gameshark") && _stricmp(sec, "files") &&
                _stricmp(sec, "patch") && _stricmp(sec, "textures")) {
                snprintf(m->err, sizeof m->err, "section [%s] needs a newer build", sec); ok = 0;
            }
            continue;
        }
        if (!_stricmp(sec, "mod")) {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            if (!_stricmp(k, "name")) snprintf(m->name, sizeof m->name, "%s", v);
            else if (!_stricmp(k, "version")) snprintf(m->version, sizeof m->version, "%s", v);
            else if (!_stricmp(k, "author")) snprintf(m->author, sizeof m->author, "%s", v);
            else if (!_stricmp(k, "description")) snprintf(m->desc, sizeof m->desc, "%s", v);
            else if (!_stricmp(k, "api")) api = atoi(v);
            continue;
        }
        char *c = strchr(s, ';');   /* code and file lines may carry a trailing comment */
        if (c) { *c = 0; s = trim(s); }
        if (!*s) continue;
        if (!_stricmp(sec, "ram")) ok = parse_ram(m, s, i + 1);
        else if (!_stricmp(sec, "gameshark")) { lines[i] = s; ok = parse_gs(m, lines, nl, &i, i + 1); }
        else if (!_stricmp(sec, "files")) ok = parse_file(m, s, i + 1, 0);
        else if (!_stricmp(sec, "patch")) ok = parse_file(m, s, i + 1, 1);
        else if (!_stricmp(sec, "textures")) {
            char *eq = strchr(s, '=');
            char *k = s, *v = (char *)"";
            if (eq) { *eq = 0; k = trim(s); v = trim(eq + 1); }
            if (_stricmp(k, "folder") || !*v || strlen(v) >= sizeof m->texdir || !safe_rel(v)) {
                snprintf(m->err, sizeof m->err, "line %d: want folder = <folder inside the mod>", i + 1); ok = 0;
            } else snprintf(m->texdir, sizeof m->texdir, "%s", v);
        }
        else { snprintf(m->err, sizeof m->err, "line %d: outside a section", i + 1); ok = 0; }
    }
    free(buf);
    if (ok && api > MOD_API) { snprintf(m->err, sizeof m->err, "needs mod API %d (this build has %d)", api, MOD_API); ok = 0; }
    if (!ok) free_mod(m);
    return ok;
}

static int find_mod(const char *id) {
    for (int i = 0; i < n_mods; i++) if (!_stricmp(mods[i].id, id)) return i;
    return -1;
}

void mods_init(const char *exe_dir, const char *ini) {
    const char *e = getenv("RUGRATS_MODDIR");
    snprintf(mod_dir, sizeof mod_dir, "%s", e ? e : exe_dir);
    if (!e) strncat(mod_dir, "\\mods", sizeof mod_dir - strlen(mod_dir) - 1);
    snprintf(mod_ini, sizeof mod_ini, "%s", ini);
    char pat[1100];
    snprintf(pat, sizeof pat, "%s\\*", mod_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            if (n_mods >= MAX_MODS) { rt_logf("mods: more than %d mods, rest ignored\n", MAX_MODS); break; }
            Mod *m = &mods[n_mods];
            memset(m, 0, sizeof *m);
            if (!valid_id(fd.cFileName)) {
                rt_logf("mods: skipped folder '%s' (names may use only letters, digits, _ - .)\n", fd.cFileName);
                continue;
            }
            snprintf(m->id, sizeof m->id, "%s", fd.cFileName);
            n_mods++;
            if (!load_mod(m, mod_dir)) rt_logf("mods: %s: %s\n", m->id, m->err);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* order: enabled mods in the ini's order, then the rest by name */
    char list[1024];
    e = getenv("RUGRATS_MODS");
    if (e) snprintf(list, sizeof list, "%s", e);
    else GetPrivateProfileStringA("mods", "enabled", "", list, sizeof list, mod_ini);
    static Mod sorted[MAX_MODS];
    int ns = 0;
    for (char *t = strtok(list, ", "); t; t = strtok(NULL, ", ")) {
        int k = find_mod(t);
        if (k < 0) { rt_logf("mods: enabled mod '%s' is not installed (or listed twice)\n", t); continue; }
        sorted[ns] = mods[k];
        sorted[ns].enabled = sorted[ns].live = !mods[k].err[0];
        ns++;
        mods[k].id[0] = 0;
    }
    int rest = ns;
    for (int i = 0; i < n_mods; i++) if (mods[i].id[0]) { sorted[ns] = mods[i]; sorted[ns].enabled = sorted[ns].live = 0; ns++; }
    for (int i = rest; i < ns; i++)
        for (int j = i + 1; j < ns; j++)
            if (_stricmp(sorted[j].id, sorted[i].id) < 0) { Mod t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    memcpy(mods, sorted, sizeof(Mod) * ns);
    n_mods = ns;
    count_active();
    rt_logf("mods: %d found in %s, %d enabled\n", n_mods, mod_dir, n_active);
    for (int i = 0; i < n_mods; i++) {
        char sm[200];
        mods_summary(i, sm, sizeof sm);
        rt_logf("mods: %s %s (%s %s): %s\n", mods[i].live ? "ON " : "off", mods[i].id, mods_name(i), mods[i].version, sm);
    }
}

static void list_ids(char *out, int n, int live) {
    out[0] = 0;
    for (int i = 0; i < n_mods; i++) {
        if (!(live ? mods[i].live : mods[i].enabled)) continue;
        if (out[0]) strncat(out, ",", n - strlen(out) - 1);
        strncat(out, mods[i].id, n - strlen(out) - 1);
    }
}
void mods_list(char *out, int n) { list_ids(out, n, 0); }   /* wanted (saved) list */
void mods_save(void) {
    char l[1024];
    mods_list(l, sizeof l);
    WritePrivateProfileStringA("mods", "enabled", l, mod_ini);
}
/* F1 switch. RAM/GameShark-only mods take effect at the next vblank; a mod with files only after a restart
   (the game may already hold the old file in RAM). Returns 1 if it changed. */
int mods_toggle(int i) {
    if (i < 0 || i >= n_mods || mods[i].err[0]) return 0;
    Mod *m = &mods[i];
    m->enabled = !m->enabled;
    if (!m->nfiles && !m->texdir[0]) m->live = m->enabled;
    count_active();
    rt_logf("mods: %s %s%s\n", m->id, m->enabled ? "enabled" : "disabled", m->nfiles || m->texdir[0] ? " (at the next start)" : "");
    return 1;
}

/* ---- per vblank ---- */
static int test(const ModOp *o) {
    uint32_t cur = o->op == OP_IF16 ? (uint32_t)(ram[o->addr] | ram[o->addr + 1] << 8) : ram[o->addr];
    switch (o->cmp) {
    case CMP_EQ: return cur == o->val;
    case CMP_NE: return cur != o->val;
    case CMP_LT: return cur < o->val;
    default: return cur > o->val;
    }
}
void mods_vblank(void) {
    if (!n_active) return;
    for (int k = 0; k < n_mods; k++) {
        const Mod *m = &mods[k];
        if (!m->live) continue;
        for (int i = 0; i < m->nops; i++) {
            const ModOp *o = &m->ops[i];
            if (o->level >= 0 && o->level != hooks_level) continue;
            uint8_t *p = ram + o->addr;
            switch (o->op) {
            case OP_W8: p[0] = (uint8_t)o->val; break;
            case OP_W16: p[0] = (uint8_t)o->val; p[1] = (uint8_t)(o->val >> 8); break;
            case OP_W32: memcpy(p, &o->val, 4); break;
            case OP_ADD8: p[0] = (uint8_t)(p[0] + o->val); break;
            case OP_ADD16: { uint16_t v = (uint16_t)(p[0] | p[1] << 8); v = (uint16_t)(v + o->val);
                             p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break; }
            case OP_IF8: case OP_IF16:
                if (!test(o)) {   /* false: skip the conditions that follow, then one code (as DuckStation does) */
                    while (i + 1 < m->nops && (m->ops[i + 1].op == OP_IF8 || m->ops[i + 1].op == OP_IF16)) i++;
                    i++;
                }
                break;
            }
        }
    }
}

/* =================================================================================================================
   Disc overlay (phase 2)
   ================================================================================================================= */
int cd_disc_read(uint32_t lba, uint8_t *out);   /* cdrom.c: the original image only */
uint32_t cd_disc_sectors(void);

typedef struct { char path[96]; uint32_t lba, size; int stream; } IsoFile;
static IsoFile *iso;
static int n_iso;

typedef struct {
    int mod;                    /* index into mods[] */
    const IsoFile *f;
    uint32_t lba, nsect;        /* where it is served */
    uint32_t tab_size;          /* size for the game's file table */
    uint32_t frames;            /* STR: frame count for the movie table (0 = keep) */
    uint8_t *data; size_t size; /* data file: the bytes */
    FILE *raw; int stride;      /* stream: raw sectors in a file (2352 or 2336 bytes each) */
    uint8_t sub_last[4];        /* subheader for data sectors (last one gets EOF/EOR) */
} Repl;
static Repl *repl;
static int n_repl;
static uint32_t ov_lo = 0xFFFFFFFFu, ov_hi;   /* quick reject range */

static int iso_walk(uint32_t lba, uint32_t size, const char *prefix, int depth) {
    if (depth > 8 || size > (1u << 20)) return 0;
    uint8_t sec[2352];
    for (uint32_t s = 0; s < (size + 2047) / 2048; s++) {
        if (!cd_disc_read(lba + s, sec)) return 0;
        const uint8_t *d = sec + 24;
        for (int i = 0; i < 2048;) {
            int n = d[i];
            if (!n || i + n > 2048) break;
            const uint8_t *r = d + i;
            i += n;
            int nl = r[32];
            if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;
            uint32_t el, es;
            memcpy(&el, r + 2, 4); memcpy(&es, r + 10, 4);
            char name[64], full[96];
            snprintf(name, sizeof name, "%.*s", nl < 63 ? nl : 63, (const char *)r + 33);
            char *semi = strchr(name, ';');
            if (semi) *semi = 0;
            snprintf(full, sizeof full, "%s%s%s", prefix, prefix[0] ? "/" : "", name);
            norm_disc(full);
            if (r[25] & 2) iso_walk(el, es, full, depth + 1);
            else if (n_iso < 2048) {
                IsoFile *f = &iso[n_iso++];
                snprintf(f->path, sizeof f->path, "%s", full);
                f->lba = el; f->size = es;
                uint8_t fs[2352];   /* streamed (real-time) file: STR / XA, read as whole raw sectors */
                for (uint32_t k = 0; k < 8 && k < (es + 2047) / 2048 && !f->stream; k++)
                    if (cd_disc_read(el + k, fs) && fs[15] == 2 && (fs[18] & 0x60)) f->stream = 1;
            }
        }
    }
    return 1;
}
static const IsoFile *iso_find(const char *p) {
    for (int i = 0; i < n_iso; i++) if (!strcmp(iso[i].path, p)) return &iso[i];
    return NULL;
}

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* BPS (beat) patch: https://github.com/blakesmith/beat/blob/master/doc/bps.txt (format by byuu, public domain) */
static int bps_num(const uint8_t *p, size_t n, size_t *i, uint64_t *out) {
    uint64_t d = 0, sh = 1;
    for (int k = 0; k < 10; k++) {
        if (*i >= n) return 0;
        uint8_t x = p[(*i)++];
        d += (x & 0x7F) * sh;
        if (x & 0x80) { *out = d; return 1; }
        sh <<= 7;
        d += sh;
    }
    return 0;
}
static const char *bps_apply(const uint8_t *p, size_t pn, const uint8_t *src, size_t sn, uint8_t **out, size_t *on) {
    *out = NULL;
    if (pn < 19 || memcmp(p, "BPS1", 4)) return "not a BPS patch";
    uint32_t c_src, c_tgt, c_pat;
    memcpy(&c_src, p + pn - 12, 4); memcpy(&c_tgt, p + pn - 8, 4); memcpy(&c_pat, p + pn - 4, 4);
    if (crc32_buf(p, pn - 4) != c_pat) return "the patch file is damaged (checksum)";
    size_t i = 4, end = pn - 12;
    uint64_t ss, ts, ms;
    if (!bps_num(p, end, &i, &ss) || !bps_num(p, end, &i, &ts) || !bps_num(p, end, &i, &ms)) return "bad BPS header";
    if (ss != sn || crc32_buf(src, sn) != c_src) return "the patch is for a different version of this file";
    if (ts > MAX_DATA_FILE || ms > end - i) return "bad BPS header";
    i += (size_t)ms;
    uint8_t *t = malloc(ts ? (size_t)ts : 1);
    if (!t) return "out of memory";
    size_t o = 0;
    int64_t srel = 0, trel = 0;
    while (i < end) {
        uint64_t d, len, k;
        if (!bps_num(p, end, &i, &d)) goto bad;
        len = (d >> 2) + 1;
        if (len > ts - o) goto bad;
        switch (d & 3) {
        case 0:   /* SourceRead */
            if (o + len > sn) goto bad;
            memcpy(t + o, src + o, (size_t)len); o += (size_t)len; break;
        case 1:   /* TargetRead */
            if (len > end - i) goto bad;
            memcpy(t + o, p + i, (size_t)len); o += (size_t)len; i += (size_t)len; break;
        case 2:   /* SourceCopy */
            if (!bps_num(p, end, &i, &k)) goto bad;
            srel += (k & 1 ? -1 : 1) * (int64_t)(k >> 1);
            if (srel < 0 || (uint64_t)srel + len > sn) goto bad;
            memcpy(t + o, src + srel, (size_t)len); o += (size_t)len; srel += (int64_t)len; break;
        default:  /* TargetCopy (may overlap: byte by byte) */
            if (!bps_num(p, end, &i, &k)) goto bad;
            trel += (k & 1 ? -1 : 1) * (int64_t)(k >> 1);
            if (trel < 0 || (uint64_t)trel >= o) goto bad;
            while (len--) { if ((uint64_t)trel >= o) goto bad; t[o++] = t[trel++]; }
        }
    }
    if (o != ts || crc32_buf(t, o) != c_tgt) goto bad;
    *out = t; *on = o;
    return NULL;
bad:
    free(t);
    return "the patch is damaged or doesn't fit this file";
}

static uint8_t *read_disc_file(const IsoFile *f) {
    uint8_t *b = malloc(f->size ? f->size : 1), sec[2352];
    if (!b) return NULL;
    for (uint32_t s = 0; s * 2048 < f->size; s++) {
        if (!cd_disc_read(f->lba + s, sec)) { free(b); return NULL; }
        uint32_t n = f->size - s * 2048 < 2048 ? f->size - s * 2048 : 2048;
        memcpy(b + s * 2048, sec + 24, n);
    }
    return b;
}
static uint8_t *read_mod_file(const char *path, size_t *n, size_t cap, const char **err) {
    FILE *f = fopen(path, "rb");
    if (!f) { *err = "file not found"; return NULL; }
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    if (sz < 0 || (unsigned long long)sz > cap) { fclose(f); *err = "file too big"; return NULL; }
    _fseeki64(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz ? (size_t)sz : 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); *err = "can't read the file"; return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void drop_repls_of(int mi) {
    for (int k = 0; k < n_repl; k++)
        if (repl[k].mod == mi) {
            free(repl[k].data);
            if (repl[k].raw) fclose(repl[k].raw);
            repl[k--] = repl[--n_repl];
        }
}
static void mod_fail(int mi, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mods[mi].err, sizeof mods[mi].err, fmt, ap);
    va_end(ap);
    rt_logf("mods: %s refused: %s\n", mods[mi].id, mods[mi].err);
    mods[mi].live = mods[mi].enabled = 0;
    drop_repls_of(mi);
}

/* one [files]/[patch] entry of an enabled mod -> a Repl (not placed yet). 0 = refused (err set). */
static int make_repl(int mi, const ModFile *mf, Repl *r) {
    char path[1400];
    const char *err = NULL;
    memset(r, 0, sizeof *r);
    r->mod = mi;
    r->f = iso_find(mf->disc);
    if (!r->f) { mod_fail(mi, "line %d: no file %s on the disc", mf->line, mf->disc); return 0; }
    snprintf(path, sizeof path, "%s\\%s\\%s", mod_dir, mods[mi].id, mf->src);
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
    if (!r->f->stream) {
        size_t n;
        uint8_t *b = read_mod_file(path, &n, MAX_DATA_FILE, &err);
        if (!b) { mod_fail(mi, "line %d: %s: %s", mf->line, mf->src, err); return 0; }
        if (mf->patch) {
            uint8_t *orig = read_disc_file(r->f), *out;
            size_t on;
            err = orig ? bps_apply(b, n, orig, r->f->size, &out, &on) : "can't read the disc file";
            free(orig); free(b);
            if (err) { mod_fail(mi, "line %d: %s: %s", mf->line, mf->src, err); return 0; }
            b = out; n = on;
        }
        r->data = b; r->size = n;
        r->nsect = (uint32_t)((n + 2047) / 2048);
        if (!r->nsect) r->nsect = 1;
        r->tab_size = (uint32_t)n;
        uint8_t sec[2352];   /* keep the original subheader (file/channel/coding) */
        if (cd_disc_read(r->f->lba, sec)) memcpy(r->sub_last, sec + 16, 4);
        else { r->sub_last[2] = 0x08; }
        return 1;
    }
    if (mf->patch) { mod_fail(mi, "line %d: %s is a streamed file (movie/audio): use [files] with raw sectors", mf->line, mf->disc); return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) { mod_fail(mi, "line %d: %s: file not found", mf->line, mf->src); return 0; }
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    int stride = sz > 0 && sz % 2352 == 0 ? 2352 : sz > 0 && sz % 2336 == 0 ? 2336 : 0;
    if (!stride || (unsigned long long)sz > MAX_STREAM_FILE) {
        fclose(f);
        mod_fail(mi, "line %d: %s is a streamed file: give raw sectors (2352 or 2336 bytes each, as modkit extract "
                 "writes them), up to 512 MB", mf->line, mf->disc);
        return 0;
    }
    r->raw = f; r->stride = stride;
    r->nsect = (uint32_t)(sz / stride);
    r->tab_size = r->nsect * 2336;
    uint8_t sec[2352];   /* STR frame count: highest frame number in the video sector headers */
    for (uint32_t k = 0; k < r->nsect; k++) {
        _fseeki64(f, (long long)k * stride, SEEK_SET);
        if (fread(sec, 1, stride, f) != (size_t)stride) break;
        const uint8_t *u = sec + (stride == 2352 ? 24 : 8);
        if (u[0] == 0x60 && u[1] == 0x01) { uint32_t fr; memcpy(&fr, u + 8, 4); if (fr > r->frames && fr < 100000) r->frames = fr; }
    }
    return 1;
}

static uint8_t tab_orig[FILE_TAB_N * 8], movie_orig[MOVIE_TAB_N * 8];
static int tab_ok;
extern const unsigned char game_exe[];
extern const unsigned int game_exe_size;
static int exe_copy(uint32_t addr, uint8_t *out, size_t n) {
    uint32_t taddr, tsize;
    memcpy(&taddr, game_exe + 24, 4);
    memcpy(&tsize, game_exe + 28, 4);
    if (addr < taddr || addr + n > taddr + tsize || 0x800 + (addr - taddr) + n > game_exe_size) return 0;
    memcpy(out, game_exe + 0x800 + (addr - taddr), n);
    return 1;
}

void mods_disc_init(void) {
    int want = 0;
    for (int i = 0; i < n_mods; i++) want += mods[i].live && mods[i].nfiles;
    if (!want) return;
    tab_ok = exe_copy(FILE_TAB, tab_orig, sizeof tab_orig) && exe_copy(MOVIE_TAB, movie_orig, sizeof movie_orig);
    iso = calloc(2048, sizeof(IsoFile));
    repl = calloc(MAX_MODS * MAX_FILES, sizeof(Repl));
    uint8_t pvd[2352];
    if (!tab_ok || !iso || !repl || !cd_disc_read(16, pvd) || memcmp(pvd + 25, "CD001", 5)) {
        for (int i = 0; i < n_mods; i++) if (mods[i].live && mods[i].nfiles) mod_fail(i, "can't read the disc's file list");
        count_active();
        return;
    }
    uint32_t rl, rs;
    memcpy(&rl, pvd + 24 + 156 + 2, 4); memcpy(&rs, pvd + 24 + 156 + 10, 4);
    iso_walk(rl, rs, "", 0);
    rt_logf("mods: disc has %d files\n", n_iso);
    /* build every replacement; a later mod replacing the same file wins */
    for (int mi = 0; mi < n_mods; mi++) {
        if (!mods[mi].live) continue;
        for (int k = 0; k < mods[mi].nfiles && mods[mi].live; k++) {
            Repl r;
            if (!make_repl(mi, &mods[mi].files[k], &r)) break;
            for (int j = 0; j < n_repl; j++)
                if (repl[j].f == r.f) {
                    rt_logf("mods: %s replaces %s again (was %s): %s wins\n", mods[mi].id, r.f->path,
                            mods[repl[j].mod].id, mods[mi].id);
                    free(repl[j].data);
                    if (repl[j].raw) fclose(repl[j].raw);
                    repl[j--] = repl[--n_repl];
                }
            repl[n_repl++] = r;
        }
    }
    /* place them: in the original sectors when they fit, else after the end of the disc */
    uint32_t next = (cd_disc_sectors() + 15) & ~15u;
    for (int j = 0; j < n_repl; j++) {
        Repl *r = &repl[j];
        uint32_t have = (r->f->size + 2047) / 2048;
        if (r->nsect <= have) r->lba = r->f->lba;
        else if (next + r->nsect > LAST_LBA) { mod_fail(r->mod, "%s: no room left on the virtual disc", r->f->path); j = -1; next = (cd_disc_sectors() + 15) & ~15u; continue; }
        else { r->lba = next; next = (next + r->nsect + 15) & ~15u; }
    }
    for (int j = 0; j < n_repl; j++) {
        Repl *r = &repl[j];
        if (r->lba < ov_lo) ov_lo = r->lba;
        if (r->lba + r->nsect > ov_hi) ov_hi = r->lba + r->nsect;
        int ids = 0;
        for (int i = 0; i < FILE_TAB_N; i++) { uint32_t l; memcpy(&l, tab_orig + i * 8, 4); ids += l == r->f->lba; }
        rt_logf("mods: %s: %s (%u sectors%s) %s LBA %u, %d table entr%s%s\n", mods[r->mod].id, r->f->path, r->nsect,
                r->raw ? ", raw" : "", r->lba == r->f->lba ? "in place at" : "moved to", r->lba, ids, ids == 1 ? "y" : "ies",
                r->frames ? ", movie" : "");
        if (!ids) rt_logf("mods: note: the game's file table has no entry for %s, so the game never loads it\n", r->f->path);
    }
    count_active();
}

/* rewrite the game's file and movie tables in guest RAM: originals from the EXE image, then the replacements */
void mods_tables_apply(void) {
    if (!tab_ok) tab_ok = exe_copy(FILE_TAB, tab_orig, sizeof tab_orig) && exe_copy(MOVIE_TAB, movie_orig, sizeof movie_orig);
    if (!tab_ok) return;
    uint8_t *t = ram + (FILE_TAB & 0x1FFFFF), *mv = ram + (MOVIE_TAB & 0x1FFFFF);
    memcpy(t, tab_orig, sizeof tab_orig);
    memcpy(mv, movie_orig, sizeof movie_orig);
    for (int j = 0; j < n_repl; j++) {
        const Repl *r = &repl[j];
        for (int i = 0; i < FILE_TAB_N; i++) {
            uint32_t l;
            memcpy(&l, tab_orig + i * 8, 4);
            if (l != r->f->lba) continue;
            memcpy(t + i * 8, &r->lba, 4);
            memcpy(t + i * 8 + 4, &r->tab_size, 4);
        }
        for (int i = 0; i < MOVIE_TAB_N; i++) {
            uint32_t l;
            memcpy(&l, movie_orig + i * 8 + 4, 4);
            if (l != r->f->lba) continue;
            memcpy(mv + i * 8 + 4, &r->lba, 4);
            if (r->frames) memcpy(mv + i * 8, &r->frames, 4);
        }
    }
}

/* texture packs of the running mods, in load order (later packs win), then the pack index (texpack.c) */
int tex_add_folder(const char *dir);
void tex_init(int dump);
void mods_tex_init(int dump) {
    for (int i = 0; i < n_mods; i++) {
        Mod *m = &mods[i];
        if (!m->live || !m->texdir[0]) continue;
        char p[1400];
        snprintf(p, sizeof p, "%s\\%s\\%s", mod_dir, m->id, m->texdir);
        for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
        DWORD a = GetFileAttributesA(p);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { mod_fail(i, "texture folder %s not found", m->texdir); continue; }
        m->ntex = tex_add_folder(p);
        rt_logf("mods: %s: %d textures in %s\n", m->id, m->ntex, m->texdir);
    }
    count_active();
    tex_init(dump);
}

void mods_boot(void) { if (n_repl) mods_tables_apply(); }

static int bcd(int v) { return ((v / 10) << 4) | (v % 10); }
static void sector_header(uint8_t *s, uint32_t lba) {
    s[0] = 0; memset(s + 1, 0xFF, 10); s[11] = 0;
    uint32_t a = lba + 150;
    s[12] = (uint8_t)bcd(a / 4500); s[13] = (uint8_t)bcd(a / 75 % 60); s[14] = (uint8_t)bcd(a % 75); s[15] = 2;
}
/* cdrom.c asks here first; 1 = this sector comes from a mod */
int mods_sector(uint32_t lba, uint8_t *out) {
    if (lba < ov_lo || lba >= ov_hi) return 0;
    for (int j = n_repl - 1; j >= 0; j--) {
        const Repl *r = &repl[j];
        if (lba < r->lba || lba >= r->lba + r->nsect) continue;
        uint32_t k = lba - r->lba;
        static int modlog = -1;   /* RUGRATS_MODLOG=1: log when the game reads a replaced file */
        if (modlog < 0) modlog = getenv("RUGRATS_MODLOG") != NULL;
        if (modlog && (k == 0 || k + 1 == r->nsect)) rt_logf("mods: game reads %s sector %u/%u (LBA %u)\n", r->f->path, k + 1, r->nsect, lba);
        memset(out, 0, 2352);
        if (r->raw) {
            _fseeki64(r->raw, (long long)k * r->stride, SEEK_SET);
            uint8_t *dst = r->stride == 2352 ? out : out + 16;
            if (fread(dst, 1, r->stride, r->raw) != (size_t)r->stride) memset(out, 0, 2352);
        } else {
            memcpy(out + 16, r->sub_last, 4);
            out[18] = (uint8_t)((out[18] & ~0x81) | (k + 1 == r->nsect ? 0x81 : 0));   /* EOF + EOR on the last */
            memcpy(out + 20, out + 16, 4);
            size_t off = (size_t)k * 2048, n = r->size > off ? r->size - off : 0;
            memcpy(out + 24, r->data + off, n < 2048 ? n : 2048);
        }
        sector_header(out, lba);
        return 1;
    }
    return 0;
}

/* ---- save states: remember which mods were on; warn (don't refuse) when loading under a different set ---- */
void mods_state(StateIO *s) {
    char now[512], saved[512];
    list_ids(now, sizeof now, 1);
    memcpy(saved, now, sizeof saved);   /* a state from before mods keeps "same as now": no warning */
    st_prefix(s, "mods");
    ST_N(s, "list", saved, sizeof saved);
    if (st_loading(s)) {
        saved[sizeof saved - 1] = 0;
        if (strcmp(saved, now)) {
            rt_logf("state: saved with mods '%s', running with '%s'\n", saved, now);
            strncat(state_msg, saved[0] ? " (saved with other mods)" : " (saved without mods)",
                    sizeof state_msg - strlen(state_msg) - 1);
        }
        /* the state's RAM holds the tables of the mods it was saved with */
        if (n_repl || strcmp(saved, now)) mods_tables_apply();
    }
}
