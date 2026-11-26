/* CD-ROM controller (register level) backed by the original MODE2/2352 .bin. XA-ADPCM decoding. */
#include <stdlib.h>
#include "hw.h"

static FILE *disc;
static uint32_t disc_sectors;

static struct {
    int index;
    uint8_t param[16]; int nparam;
    uint8_t resp[16]; int nresp, rpos;
    uint8_t ie, iflag;
    uint8_t stat, mode;
    uint32_t setloc, pos;          /* LBA */
    int setloc_pending;
    int reading, seeking;
    uint8_t sector[2352];
    uint8_t data[2352]; int ndata, dpos;
    int sector_ready;
    uint8_t filter_file, filter_chan;
    int muted;
    /* pending second response / interrupts */
    struct { int type; uint8_t r[16]; int n; uint32_t delay; } q[8];
    int nq;
    int busy;
    uint32_t next_read;
    uint8_t vol_ll, vol_lr, vol_rl, vol_rr;
} c;

/* the original image only (mods.c reads the ISO directory and source files through this) */
int cd_disc_read(uint32_t lba, uint8_t *out) {
    if (!disc || lba >= disc_sectors) { memset(out, 0, 2352); return 0; }
    fseek(disc, (long)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, disc) == 2352;
}
uint32_t cd_disc_sectors(void) { return disc_sectors; }

/* what the game reads: a mod's replacement file (mods.c) or the image */
int cd_read_sector_raw(uint32_t lba, uint8_t *out) {
    int mods_sector(uint32_t lba, uint8_t *out), icm_sector(uint32_t lba, uint8_t *out), hub_sector(uint32_t lba, uint8_t *out);
    if (icm_sector(lba, out)) return 1;   /* icecream.c: Ice Cream Mountain converted from this disc */
    if (hub_sector(lba, out)) return 1;   /* hubrooms.c: the house with the family room and garage (Hub World Enhancements) */
    if (mods_sector(lba, out)) return 1;
    if (!disc || lba >= disc_sectors) { memset(out, 0, 2352); return 0; }
    fseek(disc, (long)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, disc) == 2352;
}

void cd_init(const char *path) {
    memset(&c, 0, sizeof c);
    c.stat = 0x02;
    c.vol_ll = c.vol_rr = 0x80;
    c.ie = 0x1F; /* the BIOS leaves all CD interrupts enabled after boot */
    disc = fopen(path, "rb");
    if (!disc) rt_fatal("cannot open disc image %s", path);
    fseek(disc, 0, SEEK_END);
    disc_sectors = (uint32_t)(ftell(disc) / 2352);
    rt_logf("disc: %s, %u sectors\n", path, disc_sectors);
}

static int bcd(int v) { return (v >> 4) * 10 + (v & 15); }
static int tobcd(int v) { return ((v / 10) << 4) | (v % 10); }

static void queue_irq(int type, const uint8_t *r, int n, uint32_t delay) {
    if (c.nq >= 8) { rt_logf("cd irq queue overflow\n"); return; }
    c.q[c.nq].type = type;
    memcpy(c.q[c.nq].r, r, n);
    c.q[c.nq].n = n;
    c.q[c.nq].delay = delay;
    c.nq++;
    if (c.nq == 1 && !(c.iflag & 7)) sched(EV_CDROM, delay);
}

static void ack1(uint32_t delay) { uint8_t r = c.stat; queue_irq(3, &r, 1, delay); }

static uint32_t sector_cycles(void) {
    return (c.mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75;
}

/* ---- XA ADPCM ------------------------------------------------------- */
static const int xa_k0[4] = { 0, 60, 115, 98 }, xa_k1[4] = { 0, 0, -52, -55 };
static int32_t xa_prev[2][2];

static void xa_decode(const uint8_t *sec) {
    uint8_t coding = sec[16 + 3];
    int stereo = coding & 1, rate = (coding & 4) ? 18900 : 37800, bits8 = coding & 0x10;
    {   /* RUGRATS_XALOG=1: one line per decoded XA sector (debug) */
        static int xl = -1;
        if (xl < 0) xl = getenv("RUGRATS_XALOG") != NULL;
        if (xl) rt_logf("xa lba %u file %u chan %u coding %02x mode %02x filter %u/%u vol %02x %02x %02x %02x cyc %u\n",
                        c.pos - 1, sec[16], sec[17], coding, c.mode, c.filter_file, c.filter_chan, c.vol_ll, c.vol_lr,
                        c.vol_rl, c.vol_rr, cpu.cycles);
    }
    if (bits8) return;
    static int16_t out[18 * 8 * 28 * 2];
    int16_t mono[18 * 8 * 28];
    int nl = 0, nm = 0;
    int16_t left[18 * 4 * 28], right[18 * 4 * 28];
    int nr = 0;
    const uint8_t *data = sec + 24;
    for (int grp = 0; grp < 18; grp++) {
        const uint8_t *gp = data + grp * 128;
        for (int blk = 0; blk < 8; blk++) {
            uint8_t hdr = gp[4 + blk];
            int shift = hdr & 15, filt = (hdr >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (blk & 1) : 0;
            int32_t *pv = xa_prev[ch];
            for (int s = 0; s < 28; s++) {
                uint8_t b = gp[16 + blk / 2 + s * 4];
                int nib = (blk & 1) ? (b >> 4) : (b & 15);
                int32_t smp = (int16_t)(nib << 12) >> shift;
                smp += (pv[0] * xa_k0[filt] + pv[1] * xa_k1[filt] + 32) >> 6;
                if (smp > 32767) smp = 32767;
                if (smp < -32768) smp = -32768;
                pv[1] = pv[0];
                pv[0] = smp;
                if (!stereo) mono[nm++] = (int16_t)smp;
                else if (ch == 0) left[nl++] = (int16_t)smp;
                else right[nr++] = (int16_t)smp;
            }
        }
    }
    int frames;
    if (stereo) {
        frames = nl < nr ? nl : nr;
        for (int i = 0; i < frames; i++) { out[i * 2] = left[i]; out[i * 2 + 1] = right[i]; }
    } else {
        frames = nm;
        for (int i = 0; i < frames; i++) out[i * 2] = out[i * 2 + 1] = mono[i];
    }
    if (!c.muted) spu_cd_audio(out, frames, rate);
}

/* Over-fed XA streams. The house intro (DATA/CINEMAT/OPEN.BIN) is read at 2x in one go, and 4 of every 5 sectors
   are XA audio for the same channel: each audio sector is stored 6-7 times in a row, byte for byte (880 unique
   sectors = 46.9 s = the intro), so the stream arrives ~6.4x faster than it plays. Queueing every copy kept the
   CD audio ring full and stitched pieces of repeated sectors together (distorted sound). A sector is skipped when
   it is identical to the previous XA sector of the same file/channel AND arrived in less than half of that
   sector's playing time: a normal real-time stream (one sector per playing time) is never touched, even when
   it has identical silent sectors. RUGRATS_XADUP=0 turns this off (debug). */
static uint8_t xa_last[2304];
static uint8_t xa_last_fc[2] = { 0xFF, 0xFF };
static uint32_t xa_last_cyc;
static uint32_t xa_skipped;
static int xa_duplicate(const uint8_t *sec) {
    static int env = -1;
    if (env < 0) { const char *e = getenv("RUGRATS_XADUP"); env = e ? atoi(e) != 0 : 1; }
    uint8_t coding = sec[16 + 3];
    int stereo = coding & 1, rate = (coding & 4) ? 18900 : 37800;
    uint32_t frames = stereo ? 2016 : 4032;
    uint32_t play = (uint32_t)((uint64_t)CPU_HZ * frames / rate);
    uint32_t dt = cpu.cycles - xa_last_cyc;
    int same = xa_last_fc[0] == sec[16] && xa_last_fc[1] == sec[17] && !memcmp(xa_last, sec + 24, sizeof xa_last);
    xa_last_cyc = cpu.cycles;
    if (env && same && dt < play / 2) {
        if (getenv("RUGRATS_XALOG")) rt_logf("xa skip duplicate lba %u (%u skipped)\n", c.pos - 1, ++xa_skipped);
        return 1;
    }
    memcpy(xa_last, sec + 24, sizeof xa_last);
    xa_last_fc[0] = sec[16]; xa_last_fc[1] = sec[17];
    return 0;
}

/* ---- sector reading ------------------------------------------------- */
static void read_next_sector(void) {
    cd_read_sector_raw(c.pos, c.sector);
    uint32_t lba = c.pos++;
    static int trace = -1;
    if (trace < 0) trace = getenv("RUGRATS_TRACE") && getenv("RUGRATS_TRACE")[0] == '2';
    if (trace) rt_logf("cd sector %u sub %02x mode %02x iflag %02x nq %d cyc %u\n", lba, c.sector[18], c.mode, c.iflag, c.nq, cpu.cycles);
    uint8_t sub = c.sector[18];
    if ((c.mode & 0x40) && (sub & 0x04) && c.sector[15] == 2) {
        /* XA audio sector */
        if (!(c.mode & 0x08) || (c.sector[16] == c.filter_file && c.sector[17] == c.filter_chan)) {
            if (!xa_duplicate(c.sector)) xa_decode(c.sector);
        }
        return; /* audio sectors never reach the CPU when XA playback is on */
    }
    if ((c.mode & 0x08) && (sub & 0x40) && !(c.sector[16] == c.filter_file && c.sector[17] == c.filter_chan))
        return; /* real-time sector of another channel */
    c.sector_ready = 1;
    uint8_t r = c.stat;
    queue_irq(1, &r, 1, 100);
}

void cd_event(void) {
    if (c.nq && !(c.iflag & 7)) {
        int t = c.q[0].type;
        memcpy(c.resp, c.q[0].r, c.q[0].n);
        c.nresp = c.q[0].n;
        c.rpos = 0;
        c.iflag = (c.iflag & ~7) | t;
        memmove(&c.q[0], &c.q[1], sizeof c.q[0] * (c.nq - 1));
        c.nq--;
        if (c.ie & c.iflag & 7) irq_raise(IRQ_CDROM);
    }
    /* reading: schedule next sector */
    if (c.reading && (int32_t)(cpu.cycles - c.next_read) >= 0) {
        /* next sector one sector time after this one was DUE, not after this late event ran: the lateness used to
           add up (~4% slower than the drive), so XA voice lines played faster than they arrived and gapped out */
        uint32_t sc = sector_cycles();
        c.next_read += sc;
        if ((int32_t)(cpu.cycles - c.next_read) >= (int32_t)sc) c.next_read = cpu.cycles + sc; /* far behind: resync */
        if (!(c.iflag & 7) || c.nq == 0) read_next_sector();
    }
    if (c.nq && !(c.iflag & 7)) sched(EV_CDROM, c.q[0].delay);
    else if (c.reading) sched(EV_CDROM, (int32_t)(c.next_read - cpu.cycles) > 0 ? c.next_read - cpu.cycles : 1000);
}

/* Seek time before the first sector of a ReadN/ReadS that moved the head (Setloc to another position). A real drive
   needs ~0.1 s or more for that; without it the first sectors arrive at once. The streamed in-engine cutscenes need it:
   80019da4 starts ReadS, waits VSync(3), then takes the first data sector it gets as the stream header (no stream id
   check). With no seek the 4 interleaved stream headers had already passed and it took stream 3's (e.g. 844 frames
   instead of 1491: Grandpa's Teeth intro cut at 45 s; PCSX-Redux plays all 1491). Model: forward hops under 32 sectors
   = 20 ms + reading through them; longer = 100 ms + up to 300 ms by distance. RUGRATS_CDSEEK=0 = old (no seek). */
static uint32_t seek_cycles(uint32_t from, uint32_t to) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_CDSEEK"); on = e ? atoi(e) != 0 : 1; }
    if (!on || from == to) return 0;
    uint32_t diff = to > from ? to - from : from - to;
    uint64_t hz = CPU_HZ;
    if (to > from && diff < 32) return (uint32_t)(hz / 50 + (uint64_t)sector_cycles() * diff);
    uint64_t span = disc_sectors ? disc_sectors : 330000;
    if (diff > span) diff = (uint32_t)span;
    return (uint32_t)(hz / 10 + hz * 3 / 10 * diff / span);
}

static void start_read(void) {
    uint32_t seek = 0;
    if (c.setloc_pending) { seek = seek_cycles(c.pos, c.setloc); c.pos = c.setloc; c.setloc_pending = 0; }
    c.reading = 1;
    c.next_read = cpu.cycles + sector_cycles() + seek;
    c.stat = 0x22;
    ack1(1000);
    sched(EV_CDROM, 1000);
}

static void command(uint8_t cmd) {
    uint8_t r[8];
    static int trace = -1;
    if (trace < 0) trace = getenv("RUGRATS_TRACE") != 0;
    if (trace) rt_logf("cd cmd %02x params %d [%02x %02x %02x] pos %u\n", cmd, c.nparam, c.param[0], c.param[1], c.param[2], c.pos);
    {   /* RUGRATS_XALOG=1: also the commands that start / stop streams, with the vblank and the caller */
        static int xl = -1;
        if (xl < 0) xl = getenv("RUGRATS_XALOG") != NULL;
        if (xl && (cmd == 0x02 || cmd == 0x0D || cmd == 0x1B || cmd == 0x06 || cmd == 0x09 || cmd == 0x0E)) {
            uint32_t hooks_frame(void);
            rt_logf("cdcmd %02x [%02x %02x %02x] vbl %u ra %08x last_loop %08x\n", cmd, c.param[0], c.param[1],
                    c.param[2], hooks_frame(), cpu.r[31], cpu.last_loop);
            if (cmd == 0x0D || cmd == 0x09) { /* likely return addresses on the stack */
                char b[512]; int n = 0;
                for (uint32_t a = cpu.r[29]; a < cpu.r[29] + 0x300 && n < 480; a += 4) {
                    uint32_t v = mem_r32(a);
                    if (v >= 0x80010000u && v < 0x800B6000u && (v & 3) == 0) n += snprintf(b + n, sizeof b - n, " %08x", v);
                }
                rt_logf("  stack%s\n", b);
            }
        }
    }
    c.busy = 0;
    switch (cmd) {
    case 0x01: ack1(1000); break; /* Getstat */
    case 0x02: /* Setloc */
        c.setloc = (uint32_t)(bcd(c.param[0]) * 60 * 75 + bcd(c.param[1]) * 75 + bcd(c.param[2])) - 150;
        c.setloc_pending = 1;
        ack1(1000);
        break;
    case 0x03: /* Play (CD-DA): no audio tracks on this disc */
        c.stat = 0x82; ack1(1000); break;
    case 0x06: case 0x1B: start_read(); break; /* ReadN / ReadS */
    case 0x07: c.stat = 0x02; ack1(1000); r[0] = c.stat; queue_irq(2, r, 1, 20000); break;
    case 0x08: c.reading = 0; ack1(1000); c.stat = 0x00; r[0] = c.stat; queue_irq(2, r, 1, 20000); break;
    case 0x09: /* Pause */
        ack1(1000);
        c.reading = 0;
        c.stat = 0x02;
        r[0] = c.stat;
        queue_irq(2, r, 1, 20000);
        break;
    case 0x0A: /* Init */
        c.mode = 0; c.reading = 0; c.stat = 0x02;
        ack1(1000);
        r[0] = c.stat;
        queue_irq(2, r, 1, 30000);
        break;
    case 0x0B: c.muted = 1; ack1(1000); break;
    case 0x0C: c.muted = 0; ack1(1000); break;
    case 0x0D: c.filter_file = c.param[0]; c.filter_chan = c.param[1]; ack1(1000); break;
    case 0x0E: c.mode = c.param[0]; ack1(1000); break;
    case 0x0F: r[0] = c.stat; r[1] = c.mode; r[2] = 0; r[3] = c.filter_file; r[4] = c.filter_chan; queue_irq(3, r, 5, 1000); break;
    case 0x10: memcpy(r, c.sector + 12, 8); queue_irq(3, r, 8, 1000); break; /* GetlocL */
    case 0x11: { /* GetlocP */
        uint32_t l = c.pos + 150;
        uint32_t rel = c.pos;
        r[0] = 1; r[1] = 1;
        r[2] = (uint8_t)tobcd(rel / 4500); r[3] = (uint8_t)tobcd((rel / 75) % 60); r[4] = (uint8_t)tobcd(rel % 75);
        r[5] = (uint8_t)tobcd(l / 4500); r[6] = (uint8_t)tobcd((l / 75) % 60); r[7] = (uint8_t)tobcd(l % 75);
        queue_irq(3, r, 8, 1000);
        break;
    }
    case 0x13: r[0] = c.stat; r[1] = 1; r[2] = 1; queue_irq(3, r, 3, 1000); break; /* GetTN */
    case 0x14: { /* GetTD */
        uint32_t l = (c.param[0] == 0) ? disc_sectors + 150 : 150;
        r[0] = c.stat; r[1] = (uint8_t)tobcd(l / 4500); r[2] = (uint8_t)tobcd((l / 75) % 60);
        queue_irq(3, r, 3, 1000);
        break;
    }
    case 0x15: case 0x16: /* SeekL / SeekP */
        if (c.setloc_pending) { c.pos = c.setloc; c.setloc_pending = 0; }
        c.reading = 0;
        c.stat = 0x02;
        ack1(1000);
        r[0] = c.stat;
        queue_irq(2, r, 1, 30000);
        break;
    case 0x19: /* Test */
        if (c.param[0] == 0x20) { r[0] = 0x94; r[1] = 0x09; r[2] = 0x19; r[3] = 0xC0; queue_irq(3, r, 4, 1000); }
        else ack1(1000);
        break;
    case 0x1A: /* GetID: licensed NTSC-U */
        ack1(1000);
        r[0] = 0x02; r[1] = 0x00; r[2] = 0x20; r[3] = 0x00; r[4] = 'S'; r[5] = 'C'; r[6] = 'E'; r[7] = 'A';
        queue_irq(2, r, 8, 30000);
        break;
    case 0x1E: ack1(1000); r[0] = c.stat; queue_irq(2, r, 1, 40000); break; /* ReadTOC */
    default:
        rt_logf("cd: unknown command %02x\n", cmd);
        r[0] = c.stat | 1; r[1] = 0x40;
        queue_irq(5, r, 2, 1000);
        break;
    }
    c.nparam = 0;
}

uint8_t cd_read(int reg) {
    switch (reg) {
    case 0: {
        uint8_t s = (uint8_t)c.index;
        if (c.nparam == 0) s |= 0x08;
        if (c.nparam < 16) s |= 0x10;
        if (c.rpos < c.nresp) s |= 0x20;
        if (c.dpos < c.ndata) s |= 0x40;
        if (c.busy) s |= 0x80;
        return s;
    }
    case 1: return c.rpos < c.nresp ? c.resp[c.rpos++] : 0;
    case 2: return c.dpos < c.ndata ? c.data[c.dpos++] : 0;
    case 3: return (c.index & 1) ? (c.iflag | 0xE0) : (c.ie | 0xE0);
    }
    return 0;
}

void cd_write(int reg, uint8_t v) {
    static int trace = -1;
    if (trace < 0) trace = getenv("RUGRATS_TRACE") && getenv("RUGRATS_TRACE")[0] == '2';
    if (trace) rt_logf("cd w idx%d reg%d = %02x (iflag %02x ie %02x nq %d)\n", c.index, reg, v, c.iflag, c.ie, c.nq);
    if (reg == 0) { c.index = v & 3; return; }
    switch ((c.index << 2) | reg) {
    case 0x01: c.busy = 1; command(v); break;
    case 0x02: if (c.nparam < 16) c.param[c.nparam++] = v; break;
    case 0x03: /* request */
        if (v & 0x80) {
            if (c.sector_ready || c.ndata == 0) {
                if (c.mode & 0x20) { memcpy(c.data, c.sector + 12, 0x924); c.ndata = 0x924; }
                else { memcpy(c.data, c.sector + 24, 0x800); c.ndata = 0x800; }
                c.dpos = 0;
                c.sector_ready = 0;
            }
        } else {
            c.ndata = c.dpos = 0;
        }
        break;
    case 0x05: break; /* sound map data out */
    case 0x06: /* interrupt enable: the IRQ line is level-triggered */
        c.ie = v & 0x1F;
        if (c.ie & c.iflag & 7) irq_raise(IRQ_CDROM);
        break;
    case 0x07: /* ack */
        c.iflag &= ~(v & 0x1F);
        if (v & 0x40) c.nparam = 0;
        if (!(c.iflag & 7)) {
            c.nresp = c.rpos = 0;
            if (c.nq) sched(EV_CDROM, c.q[0].delay);
        }
        break;
    case 0x09: break; /* sound map coding info */
    case 0x0A: c.vol_ll = v; break;
    case 0x0B: c.vol_lr = v; break;
    case 0x0D: c.vol_rr = v; break;
    case 0x0E: c.vol_rl = v; break;
    case 0x0F: break; /* apply volume */
    }
}

uint32_t cd_dma_read(void) {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) w |= (uint32_t)(c.dpos < c.ndata ? c.data[c.dpos++] : 0) << (i * 8);
    return w;
}

/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
void cd_state(StateIO *s) {
    st_prefix(s, "cdrom");
    ST(s, c.index); ST(s, c.param); ST(s, c.nparam); ST(s, c.resp); ST(s, c.nresp); ST(s, c.rpos);
    ST(s, c.ie); ST(s, c.iflag); ST(s, c.stat); ST(s, c.mode); ST(s, c.setloc); ST(s, c.pos); ST(s, c.setloc_pending);
    ST(s, c.reading); ST(s, c.seeking); ST(s, c.sector); ST(s, c.data); ST(s, c.ndata); ST(s, c.dpos);
    ST(s, c.sector_ready); ST(s, c.filter_file); ST(s, c.filter_chan); ST(s, c.muted); ST(s, c.q); ST(s, c.nq);
    ST(s, c.busy); ST(s, c.next_read); ST(s, c.vol_ll); ST(s, c.vol_lr); ST(s, c.vol_rl); ST(s, c.vol_rr);
    ST(s, xa_prev);
}
