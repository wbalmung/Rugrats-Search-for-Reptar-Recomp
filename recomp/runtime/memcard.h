/* Memory card: standard 128KB .mcd images, BIOS file API on top. Included once by bios.c. */
#include <ctype.h>

void bios_deliver_event(uint32_t cls, uint32_t spec);

#define MC_SIZE 0x20000
static uint8_t mc_img[2][MC_SIZE];
static char mc_path[2][1100];
static int mc_present[2];

typedef struct { int used, slot, first_block; uint32_t pos, size; } McFile;
static McFile mc_fd[16];
static uint32_t mc_find_slot, mc_find_index;
static char mc_find_pat[24];

static void mc_events(uint32_t spec) {
    bios_deliver_event(0xF0000011, spec); /* HwCARD */
    bios_deliver_event(0xF4000001, spec); /* SwCARD */
}

static uint8_t *mc_frame(int s, int f) { return mc_img[s] + f * 128; }

static void mc_checksum(uint8_t *fr) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= fr[i];
    fr[127] = x;
}

static void mc_blank(int s) {
    memset(mc_img[s], 0, MC_SIZE);
    uint8_t *h = mc_frame(s, 0);
    h[0] = 'M'; h[1] = 'C';
    mc_checksum(h);
    for (int i = 1; i < 16; i++) {
        uint8_t *d = mc_frame(s, i);
        d[0] = 0xA0;
        d[8] = d[9] = 0xFF;
        mc_checksum(d);
    }
    for (int i = 16; i < 36; i++) {
        uint8_t *d = mc_frame(s, i);
        memset(d, 0, 128);
        d[0] = d[1] = d[2] = d[3] = 0xFF;
        d[8] = d[9] = 0xFF;
        mc_checksum(d);
    }
    memcpy(mc_frame(s, 63), h, 128);
}

static void mc_save(int s) {
    FILE *f = fopen(mc_path[s], "wb");
    if (!f) { rt_logf("cannot write %s\n", mc_path[s]); return; }
    fwrite(mc_img[s], 1, MC_SIZE, f);
    fclose(f);
}

static void mc_init(const char *dir) {
    for (int s = 0; s < 2; s++) {
        snprintf(mc_path[s], sizeof mc_path[s], "%s/card%d.mcd", dir[0] ? dir : ".", s + 1);
        FILE *f = fopen(mc_path[s], "rb");
        if (f && fread(mc_img[s], 1, MC_SIZE, f) == MC_SIZE) {
            mc_present[s] = 1;
        } else {
            mc_blank(s);
            mc_present[s] = (s == 0); /* slot 1 always has a card */
            if (s == 0) mc_save(s);
        }
        if (f) fclose(f);
    }
}

/* "bu00:NAME" -> slot, name */
static int mc_parse(uint32_t p, char *name) {
    char buf[64];
    int i;
    for (i = 0; i < 63; i++) { buf[i] = (char)mem_r8(p + i); if (!buf[i]) break; }
    buf[63] = 0;
    if (strncmp(buf, "bu", 2) != 0 || !buf[2] || !buf[3]) return -1;
    int slot = buf[2] == '1' ? 1 : 0;
    const char *c = strchr(buf, ':');
    snprintf(name, 21, "%s", c ? c + 1 : "");
    return slot;
}

static int mc_lookup(int s, const char *name) {
    for (int i = 1; i < 16; i++) {
        uint8_t *d = mc_frame(s, i);
        if (d[0] == 0x51 && strncmp((char *)d + 10, name, 20) == 0) return i;
    }
    return -1;
}

static uint32_t mc_card_info(uint32_t port) {
    int s = (port >> 4) & 1;
    mc_events(mc_present[s] ? 0x0004 : 0x0100);
    return 1;
}

static uint32_t mc_card_load(uint32_t port) { return mc_card_info(port); }

static uint32_t mc_open(uint32_t pname, uint32_t mode) {
    char name[24];
    int s = mc_parse(pname, name);
    if (s < 0 || !mc_present[s]) return 0xFFFFFFFF;
    int blk = mc_lookup(s, name);
    if (mode & 0x200) { /* create */
        if (blk >= 0) return 0xFFFFFFFF;
        int nblk = (mode >> 16) ? (int)(mode >> 16) : 1;
        int list[15], n = 0;
        for (int i = 1; i < 16 && n < nblk; i++)
            if ((mc_frame(s, i)[0] & 0xF0) == 0xA0) list[n++] = i;
        if (n < nblk) return 0xFFFFFFFF;
        for (int k = 0; k < n; k++) {
            uint8_t *d = mc_frame(s, list[k]);
            memset(d, 0, 128);
            d[0] = k == 0 ? 0x51 : (k == n - 1 ? 0x53 : 0x52);
            if (k == 0) { uint32_t sz = n * 8192; memcpy(d + 4, &sz, 4); snprintf((char *)d + 10, 21, "%s", name); }
            uint16_t nx = k == n - 1 ? 0xFFFF : (uint16_t)(list[k + 1] - 1);
            memcpy(d + 8, &nx, 2);
            mc_checksum(d);
        }
        mc_save(s);
        blk = list[0];
    }
    if (blk < 0) return 0xFFFFFFFF;
    for (int fd = 2; fd < 16; fd++)
        if (!mc_fd[fd].used) {
            uint32_t sz;
            memcpy(&sz, mc_frame(s, blk) + 4, 4);
            mc_fd[fd] = (McFile){ 1, s, blk, 0, sz };
            return fd;
        }
    return 0xFFFFFFFF;
}

static uint8_t *mc_byte(McFile *f, uint32_t pos) {
    int blk = f->first_block;
    for (uint32_t skip = pos / 8192; skip; skip--) {
        uint16_t nx;
        memcpy(&nx, mc_frame(f->slot, blk) + 8, 2);
        if (nx == 0xFFFF) return 0;
        blk = nx + 1;
    }
    return mc_img[f->slot] + blk * 8192 + pos % 8192;
}

static uint32_t mc_rw(uint32_t fd, uint32_t buf, uint32_t len, int wr) {
    if (fd >= 16 || !mc_fd[fd].used) return 0xFFFFFFFF;
    McFile *f = &mc_fd[fd];
    uint32_t done = 0;
    while (done < len && f->pos < f->size) {
        uint8_t *b = mc_byte(f, f->pos);
        if (!b) break;
        if (wr) *b = (uint8_t)mem_r8(buf + done); else mem_w8(buf + done, *b);
        done++;
        f->pos++;
    }
    if (wr) mc_save(f->slot);
    mc_events(0x0004);
    return done;
}

static uint32_t mc_read(uint32_t fd, uint32_t buf, uint32_t len) { return mc_rw(fd, buf, len, 0); }
static uint32_t mc_write(uint32_t fd, uint32_t buf, uint32_t len) { return mc_rw(fd, buf, len, 1); }

static uint32_t mc_lseek(uint32_t fd, uint32_t off, uint32_t whence) {
    if (fd >= 16 || !mc_fd[fd].used) return 0xFFFFFFFF;
    McFile *f = &mc_fd[fd];
    f->pos = whence == 0 ? off : whence == 1 ? f->pos + off : f->size + off;
    return f->pos;
}

static uint32_t mc_close(uint32_t fd) {
    if (fd < 16) mc_fd[fd].used = 0;
    return fd;
}

static int mc_match(const char *pat, const char *s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') return 1;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s)) return 0;
        if (!*s) return 0;
    }
    return *s == 0;
}

static uint32_t mc_nextfile(uint32_t dirent) {
    int s = mc_find_slot;
    for (uint32_t i = mc_find_index; i < 16; i++) {
        uint8_t *d = mc_frame(s, i);
        if (d[0] == 0x51 && mc_match(mc_find_pat, (char *)d + 10)) {
            for (int k = 0; k < 20; k++) mem_w8(dirent + k, d[10 + k]);
            mem_w32(dirent + 0x14, 0x50);
            uint32_t sz;
            memcpy(&sz, d + 4, 4);
            mem_w32(dirent + 0x18, sz);
            mem_w32(dirent + 0x1C, 0);
            mem_w32(dirent + 0x20, i);
            mc_find_index = i + 1;
            return dirent;
        }
    }
    mc_find_index = 16;
    return 0;
}

static uint32_t mc_firstfile(uint32_t pname, uint32_t dirent) {
    char name[24];
    int s = mc_parse(pname, name);
    if (s < 0 || !mc_present[s]) return 0;
    mc_find_slot = s;
    mc_find_index = 1;
    snprintf(mc_find_pat, sizeof mc_find_pat, "%s", name[0] ? name : "*");
    return mc_nextfile(dirent);
}

static uint32_t mc_erase(uint32_t pname) {
    char name[24];
    int s = mc_parse(pname, name);
    if (s < 0) return 0;
    int blk = mc_lookup(s, name);
    if (blk < 0) return 0;
    for (int guard = 0; blk > 0 && blk < 16 && guard < 15; guard++) {
        uint8_t *d = mc_frame(s, blk);
        uint16_t nx;
        memcpy(&nx, d + 8, 2);
        d[0] = 0xA1 + (d[0] & 3) - 1;
        mc_checksum(d);
        blk = nx == 0xFFFF ? -1 : nx + 1;
    }
    mc_save(s);
    return 1;
}

static uint32_t mc_format(uint32_t pname) {
    char name[24];
    int s = mc_parse(pname, name);
    if (s < 0) return 0;
    mc_blank(s);
    mc_present[s] = 1;
    mc_save(s);
    return 1;
}

static uint32_t mc_read_sector(uint32_t port, uint32_t sector, uint32_t dst) {
    int s = (port >> 4) & 1;
    if (!mc_present[s] || sector >= 1024) { mc_events(0x0100); return 0; }
    for (int i = 0; i < 128; i++) mem_w8(dst + i, mc_img[s][sector * 128 + i]);
    mc_events(0x0004);
    return 1;
}

static uint32_t mc_write_sector(uint32_t port, uint32_t sector, uint32_t src) {
    int s = (port >> 4) & 1;
    if (!mc_present[s] || sector >= 1024) { mc_events(0x0100); return 0; }
    for (int i = 0; i < 128; i++) mc_img[s][sector * 128 + i] = (uint8_t)mem_r8(src + i);
    mc_save(s);
    mc_events(0x0004);
    return 1;
}
