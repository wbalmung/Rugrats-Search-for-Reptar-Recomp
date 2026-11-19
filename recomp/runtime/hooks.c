/* Native hooks on specific guest functions (see HOOKS in tools/recomp.py). */
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include "hw.h"

/* stats for frame-timing research */
static uint32_t vs_calls[8], vs_frames;
static int trace_fps = -1;

static uint32_t frame_counter;
uint32_t hooks_frame(void) { return frame_counter; }

void hooks_vblank(void) {
    frame_counter++;
    void hooks_level_vblank(void);
    hooks_level_vblank();
    mods_vblank();
    if (trace_fps < 0) trace_fps = getenv("RUGRATS_TRACE") && getenv("RUGRATS_TRACE")[0] == '4';
    if (!trace_fps) return;
    if (++vs_frames % 120 == 0) {
        extern uint32_t gpu_flips;
        rt_logf("2s @%u: game fps %.1f | vsync -1:%u 0:%u 1:%u 2:%u 3:%u >3:%u\n", vs_frames, gpu_flips / 2.0,
                vs_calls[0], vs_calls[1], vs_calls[2], vs_calls[3], vs_calls[4], vs_calls[5]);
        memset(vs_calls, 0, sizeof vs_calls);
        gpu_flips = 0;
        void hooks_report(void);
        hooks_report();
    }
}

static uint32_t n_drawsync[2], n_cb, n_trans;

int rt_hook_8009c264(void) { n_drawsync[cpu.r[4] ? 1 : 0]++; return 0; }
/* The vblank callback 8001d0d8 is a two-step pipeline (gp+132): bit 1 = a finished frame waits to be DRAWN
   (8001c408 sends its lists; then bit 1 -> bit 0), bit 0 = the drawn buffer waits to be SHOWN (8001c088 ->
   8001c020 PutDispEnv). On the PS1 one call per vblank does "show N-1, draw N". Above 60 fps this ran partly from
   the early-flip poll (8001d3c4, at the END of the next frame) and partly from the real 60 Hz interrupt, so every
   other present got two flips and the next none: only half the frames were ever shown, and one present drew two
   frames (user 2026-11-18: circus drops to 80 fps; 0/2 flips per present in every level since at least 11-07).
   Now, in high-fps gameplay (cb_hifps), the pacing hook calls the callback once at the end of every game frame
   (before the present), and the interrupt's own calls are skipped. */
static int cb_inside;
static uint32_t pace_present;
static int cb_hifps(void);
int rt_hook_8001d0d8(void) {
    n_cb++;
    if (!cb_inside && cb_hifps()) return 1;   /* the pacing hook runs it once per frame instead */
    return 0;
}
static void run_frame_cb(void) {
    Cpu save = cpu;
    cpu.sr &= ~0x401u;
    cpu.r[31] = 0;
    cb_inside = 1;
    rt_dispatch(0x8001d0d8);
    cb_inside = 0;
    uint32_t c = cpu.cycles;
    cpu = save;
    cpu.cycles = c;
}
int rt_hook_8001c408(void) { n_trans++; return 0; }

static uint32_t n_step, step_hist[10], n_pace, pace_min[4];
static uint32_t game_ticks; /* 1/300 s units of game time this window */

/* ---- real-time game step ------------------------------------------------
   Original: 8001ceb4 waits for >= 2 vblanks (it assumes VSync(0) always waits a full vblank,
   which is not true when the frame finished early -> double speed), then 8001ce48(delta) sets
     gp+156 = delta*5          (ticks of 1/300 s)
     gp+160 = delta*5*4096/300 (seconds, 4.12 fixed point)
     gp+154 = ring index (0..31), ring[0x800CE0B8 + idx*4] = ticks
   Replacement: one game frame per presented frame, step = virtual time that really passed. */
static uint32_t step_last_cycles, step_init;
static uint64_t acc_ticks, acc_secs;
static uint32_t frame_present;

/* 8001d3c4 returns 1 when bit1 of gp+132 ("flip pending") is clear. The flip itself is done by the
   vblank callback 8001d0d8, which caps the game at 60 fps. Above 60 fps, run that callback now
   (interrupts masked, like the real IRQ context) so the flip happens at the presentation rate. */
/* Only in gameplay: the pacing hook 8001ceb4 must have run in a gameplay frame for this or the previous present.
   The intro / title loop steps its animations per frame and ran at the PS1's 60 Hz (one frame per vblank); above
   60 fps it ran at the presentation rate: the title's bounce-in and colour cycling 2x fast at 120 fps, and the house
   intro was cut short. There the original pacing runs instead (see rt_hook_8001ceb4) and the flip stays on the
   vblank; the screen still presents at the chosen rate. (Not told apart by database: it stays 1 there.)
   menu_loop() is called at the pacing hook entry. Gameplay = 8001d00c called from 8001d3e8 (return 0x8001d400)
   called from the level loop 80018c38 (return 0x80019038), the chain savestate.c chain[] checks. The intro and title
   also come through 8001d3e8, but from another loop. */
static int menu_loop(void) {
    static int last = -1;
    uint32_t sp = cpu.r[29];                                     /* 8001d00c's frame (0x18, ra at +0x14) */
    int m = mem_r32(sp + 0x14) != 0x8001d400u ||                 /* not called from 8001d3e8 */
            mem_r32(sp + 0x18 + 0x14) != 0x80019038u;            /* 8001d3e8 not called from the level loop */
    {   /* debug: RUGRATS_PACELOG=1 logs the caller chain when it changes */
        static uint32_t prev;
        uint32_t r2 = mem_r32(sp + 0x18 + 0x14);
        if (getenv("RUGRATS_PACELOG") && r2 != prev) { rt_logf("pacing chain %08x %08x\n", mem_r32(sp + 0x14), r2); prev = r2; }
    }
    if (m != last) {
        rt_logf("pacing: %s at vblank %u\n", m ? "menu/intro loop, original 60 Hz" : "gameplay, real-time step", hooks_frame());
        last = m;
    }
    return m;
}
static uint32_t pace_present = 0xFFFFFFF0u;
/* high-fps gameplay: real-time step, presenting above 60, and the gameplay pacing ran for this or the last present */
static int cb_hifps(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_FRAMECB"); on = e ? atoi(e) != 0 : 1; }   /* 0 = old behaviour */
    return on && opt.fixed_timestep && PRESENT_FPS > 60 && present_count - pace_present <= 1;
}
int rt_hook_8001d3c4(void) {
    if (!opt.fixed_timestep || PRESENT_FPS <= 60) return 0;
    if (present_count - pace_present > 1) return 0;
    uint32_t gp = cpu.r[28];
    if (!(mem_r8(gp + 132) & 2)) return 0;
    run_frame_cb();
    return 0; /* now run the original poll, which sees the updated flag */
}

void state_safe_point(void);
/* Draw packet ring (2026-11-14, user: Reptar 2010 crashed after standing a while). The game builds each frame's draw
   packets AND its ordering table in one ring (control block 0x800DE3B0: +0 next free, +4 start, +8 end; set up by
   80021e40 as 0x80193E50..0x801D8410 = 280 KB). The allocators (800220a8, 80022390) wrap at the end without any
   check, and each frame's lists are sent at its end, so one frame must stay under the ring size or its packets wrap
   onto its own ordering table: "DMA2 list loop" (corrupt list) in the log, then a crash. Reptar 2010 used ~84 KB a
   frame; with the extra rooms (over-wall, low-wall box mode) ~127 KB, with the building back faces ~170 KB, and in
   a real-time run at the user's settings (9x, 150 fps, a step forward) it jumped to 271 KB about 80 s in and broke.
   Bigger ring: guest RAM is 8 MB here (cpu.h; the PS1's 2 MB + an extension the game never uses), and at every
   pacing call (frame start, after any state load) the ring is pointed at 0x80200000.. +RUGRATS_BIGRING KB (default
   2048, 0 = the game's own 280 KB ring). Save states also hold the ring (cpu.ram_ext, hw.c): the frame after a load re-sends the previous frame's lists.
   Safety limits per frame, scaled to the ring (frame start = the pacing call):
   - back faces (gte.c NCLIP flips) only while the frame has used < RUGRATS_PKTBUDGET KB (big ring 1280, own 128);
   - the extra-room rules (over-wall, low walls) pause for 2 s (120 vblanks) after a frame that used
     > RUGRATS_PKTROOMS KB (big ring 1536, own 200);
   - hard stop: past RUGRATS_PKTHARD KB (big ring 1792, own 250) further room / object faces are dropped (gte.c).
   0 = that limit off. RUGRATS_PKTLOG=1 logs every frame's use (and each list send, hw.c). */
#define PKT_EXT_BASE 0x80200000u
static uint32_t pkt_start, pkt_last, pkt_rooms_off_until;
static int pkt_env(const char *name, int def) { const char *e = getenv(name); return e ? atoi(e) : def; }
static uint32_t pkt_ring_kb(void) {
    static int kb = -1;
    if (kb < 0) { kb = pkt_env("RUGRATS_BIGRING", 2048); if (kb < 0) kb = 0; if (kb > 6144) kb = 6144; }
    return (uint32_t)kb;
}
static void pkt_ring_place(void) {
    const uint32_t kb = pkt_ring_kb();
    if (!kb || mem_r32(0x800DE3B4u) == PKT_EXT_BASE) return;
    const uint32_t lo = mem_r32(0x800DE3B4u), hi = mem_r32(0x800DE3B8u);
    if (hi <= lo || lo < 0x80000000u || hi > 0x80200000u) return;   /* not set up yet */
    mem_w32(0x800DE3B0u, PKT_EXT_BASE);
    mem_w32(0x800DE3B4u, PKT_EXT_BASE);
    mem_w32(0x800DE3B8u, PKT_EXT_BASE + kb * 1024u);
    static int lg = -1;
    if (lg < 0) lg = getenv("RUGRATS_PKTLOG") != NULL;
    if (lg) rt_logf("pkt ring moved from %08x..%08x to %08x..%08x\n", lo, hi, PKT_EXT_BASE, PKT_EXT_BASE + kb * 1024u);
}
static uint32_t pkt_used(void) {
    const uint32_t cur = mem_r32(0x800DE3B0u), lo = mem_r32(0x800DE3B4u), hi = mem_r32(0x800DE3B8u);
    if (hi <= lo || hi - lo > 0x600000u || pkt_start < lo || pkt_start >= hi || cur < lo || cur > hi) return 0;
    return cur >= pkt_start ? cur - pkt_start : (hi - pkt_start) + (cur - lo);
}
static int pkt_big(void) { return mem_r32(0x800DE3B4u) == PKT_EXT_BASE; }
int hooks_pkt_room(void) {
    static int b1 = -2, b0;
    if (b1 == -2) { b1 = pkt_env("RUGRATS_PKTBUDGET", -1); b0 = b1 >= 0 ? b1 : 128; if (b1 < 0) b1 = 1280; }
    const int budget = pkt_big() ? b1 : b0;
    return !budget || pkt_used() < (uint32_t)budget * 1024u;
}
int hooks_pkt_full(void) {
    static int h1 = -2, h0;
    if (h1 == -2) { h1 = pkt_env("RUGRATS_PKTHARD", -1); h0 = h1 >= 0 ? h1 : 250; if (h1 < 0) h1 = 1792; }
    const int hard = pkt_big() ? h1 : h0;
    return hard && pkt_used() >= (uint32_t)hard * 1024u;
}
uint32_t hooks_frame(void);
static int pkt_rooms_ok(void) { return (int32_t)(hooks_frame() - pkt_rooms_off_until) >= 0; }
static void pkt_frame_start(void) {
    static int lg = -1, r1 = -2, r0;
    if (lg < 0) lg = getenv("RUGRATS_PKTLOG") != NULL;
    if (r1 == -2) { r1 = pkt_env("RUGRATS_PKTROOMS", -1); r0 = r1 >= 0 ? r1 : 200; if (r1 < 0) r1 = 1536; }
    if (pkt_start) {
        const int rooms = pkt_big() ? r1 : r0;
        pkt_last = pkt_used();
        if (rooms && pkt_last > (uint32_t)rooms * 1024u) {
            if (lg && pkt_rooms_ok()) rt_logf("pkt f%u: %u KB, extra rooms paused\n", present_count, pkt_last / 1024);
            pkt_rooms_off_until = hooks_frame() + 120;
        }
        if (lg) rt_logf("pkt f%u used %u KB\n", present_count, pkt_last / 1024);
    }
    pkt_ring_place();
    pkt_start = mem_r32(0x800DE3B0u);
}
uint32_t hooks_pkt_ring_bytes(void) { return pkt_ring_kb() * 1024u; }   /* saved as cpu.ram_ext (hw.c) */
unsigned fp_pace;   /* RUGRATS_FRAMEPROF: game frames (pacing calls) this present (host_win32.c) */
int rt_hook_8001ceb4(void) {
    fp_pace++;
    {   /* old-build state loaded: the resume call takes 2 -> 1 (that frame's stale list sends are skipped, hw.c),
           the next frame 1 -> 0 */
        extern int dma2_stale;
        if (dma2_stale) dma2_stale--;
    }
    pkt_frame_start();
    state_safe_point(); /* quick save / load requests run here: start of a frame, known guest call chain */
    pkt_ring_place(); pkt_start = mem_r32(0x800DE3B0u);   /* a state just loaded may hold the game's own ring */
    { void state_export_frame(void); state_export_frame(); } /* debug: RUGRATS_EXPORT_N */
    {   /* debug: RUGRATS_PEEKF=<hex addr>,<words> logs those guest words at the start of every game frame, with the
           presented-frame count and game ticks gp+156 (RUGRATS_PEEK samples only at 60 Hz vblanks) */
        static int init; static uint32_t pa, pn;
        if (!init) { init = 1; const char *e = getenv("RUGRATS_PEEKF"); if (e) { sscanf(e, "%x,%u", &pa, &pn); if (pn > 16) pn = 16; } }
        if (pn) {
            char b[256]; int n = 0;
            for (uint32_t i = 0; i < pn; i++) n += snprintf(b + n, sizeof b - n, " %08x", mem_r32(pa + i * 4));
            rt_logf("peekf f%u t%d:%s\n", present_count, (int)(int16_t)mem_r16(0x800C3E80u + 156), b);
        }
    }
    n_pace++;
    pace_min[cpu.r[5] < 3 ? cpu.r[5] : 3]++;
    if (!opt.fixed_timestep) return 0;
    if (menu_loop() && !getenv("RUGRATS_MENU_FAST")) return 0; /* menus/intro/title: original vblank pacing (60 Hz) */
    pace_present = present_count;
    /* the frame just finished (8001d00c set its "draw" bit): show the previous one and draw this one now, before
       this present (see rt_hook_8001d0d8) */
    if (cb_hifps() && (mem_r8(cpu.r[28] + 132) & 3)) run_frame_cb();
    /* wait for the next presented frame (fast-forward idle virtual time) */
    int guard = 0;
    while (present_count == frame_present && guard++ < 100000) {
        if ((int32_t)(cpu.next_event - cpu.cycles) > 0) cpu.cycles = cpu.next_event;
        rt_event();
    }
    frame_present = present_count;
    cpu.r[2] = 1; /* caller passes this to 8001ce48, which we also replace */
    return 1;
}

int rt_hook_8001ce48(void) {
    uint32_t d = cpu.r[4];
    n_step++;
    if (!opt.fixed_timestep) {
        step_hist[d < 9 ? d : 9]++; game_ticks += d * 5;
        { void hooks_dmg_tick(uint32_t t); hooks_dmg_tick(d * 5); }
        return 0;
    }
    if (!step_init) { step_init = 1; step_last_cycles = cpu.cycles - CYCLES_PER_FRAME; }
    uint32_t elapsed = cpu.cycles - step_last_cycles;
    step_last_cycles = cpu.cycles;
    /* original clamps to 8 vblanks */
    if (elapsed > CYCLES_PER_FRAME * 8) elapsed = CYCLES_PER_FRAME * 8;
    uint64_t frame60 = CYCLES_PER_FRAME; /* cycles per 1/60 s */
    acc_ticks += (uint64_t)elapsed * 5;           /* 5 ticks per vblank */
    acc_secs += (uint64_t)elapsed * 4096;         /* 4096/60 per vblank */
    uint32_t ticks = (uint32_t)(acc_ticks / frame60);
    acc_ticks -= (uint64_t)ticks * frame60;
    uint32_t secs = (uint32_t)(acc_secs / (frame60 * 60));
    acc_secs -= (uint64_t)secs * frame60 * 60;
    {   /* debug: RUGRATS_TICKS=<n> forces n ticks per game frame with the original truncated seconds
           (n*4096/300), e.g. 10 = the PS1's 30 fps steps, to compare against a reference emulator */
        static int force = -1;
        if (force < 0) { const char *e = getenv("RUGRATS_TICKS"); force = e ? atoi(e) : 0; }
        if (force > 0) { ticks = (uint32_t)force; secs = (uint32_t)force * 4096 / 300; }
    }
    step_hist[ticks < 9 ? ticks : 9]++;
    game_ticks += ticks;
    { void hooks_dmg_tick(uint32_t t); hooks_dmg_tick(ticks); }

    uint32_t gp = cpu.r[28];
    mem_w32(gp + 156, ticks);
    mem_w32(gp + 160, secs);
    uint32_t idx = (mem_r16(gp + 154) + 1) & 0xFFFF;
    if ((int16_t)idx >= 32) idx = 0;
    mem_w16(gp + 154, idx);
    mem_w32(0x800CE0B8u + idx * 4, ticks);
    return 1;
}

static uint32_t guest_vsync_count(void) {
    Cpu save = cpu;
    cpu.r[4] = 0xFFFFFFFF;
    cpu.r[31] = 0;
    rt_dispatch(0x8009af48);
    uint32_t v = cpu.r[2], c = cpu.cycles;
    cpu = save;
    cpu.cycles = c;
    return v;
}

void hooks_report(void) {
    static uint32_t last_vc;
    uint32_t vc = guest_vsync_count();
    rt_logf("   guest vblank counter advanced %u (real vblanks 120)\n", vc - last_vc);
    last_vc = vc;
    rt_logf("   drawsync(0):%u drawsync(1):%u frame-cb:%u transition:%u\n", n_drawsync[0], n_drawsync[1], n_cb, n_trans);
    rt_logf("   timestep calls:%u delta hist 0:%u 1:%u 2:%u 3:%u 4:%u 5+:%u | pacing calls:%u (min1:%u min2:%u)\n",
            n_step, step_hist[0], step_hist[1], step_hist[2], step_hist[3], step_hist[4],
            step_hist[5] + step_hist[6] + step_hist[7] + step_hist[8] + step_hist[9], n_pace, pace_min[1], pace_min[2]);
    if (n_step) rt_logf("   game speed %.0f%% (game time / real time)\n", game_ticks * 100.0 / 600.0);
    game_ticks = 0;
    n_drawsync[0] = n_drawsync[1] = n_cb = n_trans = 0;
    n_step = n_pace = 0;
    memset(step_hist, 0, sizeof step_hist);
    memset(pace_min, 0, sizeof pace_min);
}

/* PsyQ VSync(mode): -1 = vblank count, 0 = wait 1, n = wait until n vblanks since last call */
int rt_hook_8009af48(void) {
    int32_t m = (int32_t)cpu.r[4];
    int k = m < 0 ? 0 : m > 3 ? 5 : m + 1;
    vs_calls[k]++;
    return 0;
}

/* libetc v_wait(target, timeout) 8009b0c0: VSync's inner wait, spins until the vblank counter (0x800BCB60) reaches
   target, giving up after timeout << 15 loop passes (prints "VSync: timeout" and resets the pad/counters). On a PS1
   that loop takes longer than a vblank; our virtual CPU (overclock x4, cheap instructions) ran it in a fraction
   of one, so VSync(0) gave up before the vblank (2000+ timeouts per boot). In the movie player that skipped its
   two VSync(0) waits, so it decoded the next frame into the buffer still on screen a vblank before the display
   switch: every movie frame flashed for one vblank, then the previous one showed for three (checked against
   PCSX-Redux, 2026-11-07). Here the wait advances virtual time to the next events until the counter is reached,
   with a budget of ~40 PS1 cycles per loop pass (real-hardware-like timeout). Not in the gameplay loop: there our
   pacing hook (8001ceb4) owns the frame timing at high fps. RUGRATS_VWAIT=0 = old behaviour. */
int rt_hook_8009b0c0(void) {
    static int env = -1;
    if (env < 0) { const char *e = getenv("RUGRATS_VWAIT"); env = e ? atoi(e) != 0 : 1; }
    if (!env) return 0;
    if (present_count - pace_present <= 2) return 0;        /* gameplay frames: leave as is */
    int32_t target = (int32_t)cpu.r[4];
    uint32_t passes = cpu.r[5] << 15;
    if (!passes) return 0;                                   /* timeout 0: the original gives up at once */
    uint64_t budget = (uint64_t)passes * 40 * clk_mul, spent = 0;
    while ((int32_t)mem_r32(0x800BCB60u) < target && spent < budget) {
        uint32_t c0 = cpu.cycles;
        if ((int32_t)(cpu.next_event - cpu.cycles) > 0) cpu.cycles = cpu.next_event;
        else cpu.cycles += 64;
        rt_event();
        spent += (uint32_t)(cpu.cycles - c0);
    }
    if ((int32_t)mem_r32(0x800BCB60u) < target) return 0;    /* still not there: original timeout path */
    return 1;
}

/* movie player (8008b448) cleanup: log why playback ended (RUGRATS_MDECLOG) */
int rt_hook_8008b398(void) {
    static int on = -1;
    if (on < 0) on = getenv("RUGRATS_MDECLOG") != 0;
    if (on && cpu.r[31] == 0x8008b6d0u) rt_logf("movie %u (skippable %u) ", cpu.r[22], cpu.r[23]);
    if (on) rt_logf("movie end: ra=%08x frames=%u done=%u err=%d buf=%u cyc %u\n", cpu.r[31], cpu.r[21], cpu.r[18], (int)cpu.r[19], cpu.r[20], cpu.cycles);
    return 0;
}


/* movie player entry 8008b448(index, skippable): RUGRATS_MOVIE=<n> plays movie n (table at 0x800BAE4C:
   0 THQ, 1 Nick, 2 n-Space, 3-5 demos, 6 credits) instead of the first movie the game asks for (debug) */
/* also marks the movie playing (hooks_movie_active: its CD audio follows only the master volume, not Dialogue) */
void f_8008b448(void);
static int movie_active;
int hooks_movie_active(void) { return movie_active; }
int rt_hook_8008b448(void) {
    static int done, inside;
    if (inside) return 0;
    if (!done) {
        done = 1;
        const char *e = getenv("RUGRATS_MOVIE");
        if (e) { cpu.r[4] = (uint32_t)atoi(e); rt_logf("movie override: playing %u\n", cpu.r[4]); }
    }
    inside = 1; movie_active = 1;
    f_8008b448();                           /* the original */
    inside = 0; movie_active = 0;
    return 1;
}

/* Skip intro (ini [game] skip_intro, F1 Game page): the boot function 800185f8 shows the legal screen with
   80030010(0xea, 5) (a still for 5 s), then plays the logo movies through their wrappers 8008b230 (THQ),
   8008b27c (Nickelodeon), 8008b0b4 (n-Space), then loads the title. Only those boot calls (checked by return
   address) are skipped; the wrappers' return value (skipped by a button) is not used there. Attract demos and
   the credits are not touched. */
static int skip_boot(uint32_t ra) {
    if (!opt.skip_intro || cpu.r[31] != ra) return 0;
    static int logged;
    if (!logged++) rt_logf("skip intro: legal screen and logo movies skipped\n");
    cpu.r[2] = 0;
    return 1;
}
int rt_hook_80030010(void) { return skip_boot(0x800186d8u); }
int rt_hook_8008b230(void) { return skip_boot(0x800186f4u); }
int rt_hook_8008b27c(void) { return skip_boot(0x800186fcu); }
int rt_hook_8008b0b4(void) { return skip_boot(0x80018704u); }

/* ReadGeomScreen (returns H). Called from 80033278, the room/portal visibility test: it clips a portal quad's
   camera-space corners (array at scratchpad 0x1F8000C0, 16 bytes each, count in s2) and projects them on the
   CPU as x*H/z, bypassing the GTE's widescreen squeeze, then culls against the 4:3 rect. Squeeze x here so the
   portal rect matches the squeezed GTE output: no more geometry popping in at the widescreen edges. */
int rt_hook_8009fd08(void) {
    if (cpu.r[31] == 0x800333b4u && opt.widescreen && opt.widecull) {
        int n = (int)cpu.r[18];
        if (n > 16) n = 16;
        for (int i = 0; i < n; i++) {
            uint32_t a = 0x1F8000C0u + (uint32_t)i * 16;
            mem_w32(a, (uint32_t)gte_ws_x((int32_t)mem_r32(a)));
        }
    }
    return 0;
}

/* Room/portal visibility test 80033278(portal, rect*, out rect*, ...), called once per portal by the scene renderer
   (80033df4). It culls the next room only when every projected portal corner lies past one side of the parent rect
   (a1 {x, y, w, h}); the room's own geometry in front of the portal plane (a door jamb next to the doorway) is then
   missing although it would be on screen, and popped in ~40 native px wide once the doorway itself reached the
   screen edge (2026-11-12, nudging the stick camera at a toy store door). The parent rect is widened by
   RUGRATS_PORTALPAD native px on every side for the call (default 64, 0 = old) and restored afterwards, but only
   when that parent rect covers the whole screen (portals seen directly). Padding nested rects too broke the game in
   several saves (picture became one wall colour for good, likely its room list overran). RUGRATS_PORTALLOG=1 logs
   every call. */
void f_80033278(void);
static int rcam_eye_in_use(uint32_t cam);
static uint32_t ow_over_frame = ~0u;   /* present_count when the stick camera's eye was over a wall (gap colour) */
static int hooks_gap_level(void);      /* level has a gap colour / back faces (table below) */
static int hooks_backface_level(void); /* room / prop back faces drawn (gap levels + bf_levels) */
static int portal_in;
/* Eye over the wall (below): is neighbour room nb's box on screen within rect? Runs the game's own portal test
   80033278 (RotTrans with the current room's matrix, near clip, project, compare with the rect) on four quads of
   the box, in the frame of the room being traversed (cur = its 0x70 entry; portal corners are room-local halfwords):
   top and bottom faces and the two vertical diagonals, so a box that spans the rect still counts. Called from inside
   the hook at function entry: sp and the caller's stack arguments (sp+16/20) are the real call's; scratch lies 2 KB
   below sp; all registers restored. */
static int ow_room_rect(uint32_t cur, int nb, uint32_t rect, int16_t *box);
static int ow_room_on_screen(uint32_t cur, int nb, uint32_t rect) { return ow_room_rect(cur, nb, rect, NULL); }
/* box != NULL: test all four quads and return in box[0..3] (x, y, w, h like a portal rect) the union of the rects
   the game's test gave the visible ones, i.e. where room nb's box covers the screen inside rect. */
static int ow_room_rect(uint32_t cur, int nb, uint32_t rect, int16_t *box) {
    const uint32_t tbl = mem_r32(0x800C4A8Cu), gp = 0x800C3E80u;
    const int n = (int32_t)mem_r32(gp + 0xc1c);
    if (nb < 0 || nb >= n || n > 4096) return 0;
    const uint32_t nr = tbl + (uint32_t)nb * 0x70;
    int lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        const int32_t o = (int32_t)mem_r32(cur + 4 * a);
        int32_t l = (int32_t)mem_r32(nr + 16 + 4 * a) - o, h = (int32_t)mem_r32(nr + 32 + 4 * a) - o;
        if (l < -32768) l = -32768; if (l > 32767) l = 32767;
        if (h < -32768) h = -32768; if (h > 32767) h = 32767;
        lo[a] = l; hi[a] = h;
    }
    /* quads in the portal's strip order (v0 v1 / v2 v3): {x, y, z} per corner */
    const int q[4][4][3] = {
        { { lo[0], lo[1], lo[2] }, { hi[0], lo[1], lo[2] }, { lo[0], lo[1], hi[2] }, { hi[0], lo[1], hi[2] } },
        { { lo[0], hi[1], lo[2] }, { hi[0], hi[1], lo[2] }, { lo[0], hi[1], hi[2] }, { hi[0], hi[1], hi[2] } },
        { { lo[0], lo[1], lo[2] }, { hi[0], lo[1], hi[2] }, { lo[0], hi[1], lo[2] }, { hi[0], hi[1], hi[2] } },
        { { lo[0], lo[1], hi[2] }, { hi[0], lo[1], lo[2] }, { lo[0], hi[1], hi[2] }, { hi[0], hi[1], lo[2] } },
    };
    uint32_t save[32], shi = cpu.hi, slo = cpu.lo;
    memcpy(save, cpu.r, sizeof save);
    const uint32_t P = (save[29] - 0x800u) & ~7u, O = P + 0x40;
    int vis = 0, x0 = 0x7fff, y0 = 0x7fff, x1 = -0x8000, y1 = -0x8000;
    for (int k = 0; k < 4 && (!vis || box); k++) {
        for (int v = 0; v < 4; v++) {
            mem_w16(P + v * 8, (uint16_t)q[k][v][0]); mem_w16(P + v * 8 + 2, (uint16_t)q[k][v][1]);
            mem_w16(P + v * 8 + 4, (uint16_t)q[k][v][2]); mem_w16(P + v * 8 + 6, 0);
        }
        memcpy(cpu.r, save, sizeof save);
        cpu.r[4] = P; cpu.r[5] = rect; cpu.r[6] = O; cpu.r[7] = mem_r8(gp + 345);
        portal_in = 1;
        f_80033278();
        portal_in = 0;
        if (cpu.r[2]) {
            vis = 1;
            if (box) {
                const int x = (int16_t)mem_r16(O), y = (int16_t)mem_r16(O + 2), w = (int16_t)mem_r16(O + 4), h = (int16_t)mem_r16(O + 6);
                if (w > 0 && h > 0) {
                    if (x < x0) x0 = x; if (y < y0) y0 = y;
                    if (x + w > x1) x1 = x + w; if (y + h > y1) y1 = y + h;
                }
            }
        }
    }
    memcpy(cpu.r, save, sizeof save);
    cpu.hi = shi; cpu.lo = slo;
    if (box) {
        if (!vis || x1 <= x0 || y1 <= y0) return 0;
        box[0] = (int16_t)x0; box[1] = (int16_t)y0; box[2] = (int16_t)(x1 - x0); box[3] = (int16_t)(y1 - y0);
    }
    return vis;
}
/* Low walls (below): is room nb, or a room up to `ahead` doorways past it (not back into a room already searched),
   on screen? Third user save (2026-11-14): the far side seen over the hedge hangs off room 51 (52-55 and on, the maze
   winds), and 51's own box left the screen as the camera turned left, so the whole chain derendered. Breadth-first
   from nb, at most LW_SEEN_MAX rooms looked at. */
/* Occlusion test for the rooms the low-wall rule adds (2026-11-16, user's save by the fence in Grandpa's Teeth:
   looking up, the rule passed 12 rooms whose boxes are on screen but lie behind the near hedge / fence: twice the
   vertices, CPU frame 2.0 -> 3.5 ms). Each candidate room's box goes to the GPU as an occlusion query against the
   frame's room depth (gpu_occl_box -> gpu_d3d11.c hw_occl_add); the answer comes back a frame or more later. A room
   counts as hidden only while the newest answer is recent and says so and it has not been visible for OCCL_HOLD
   presents; no answer = visible. The box is projected here with the GTE's current matrix (the room being
   traversed: corners are relative to its origin, as in ow_room_rect), clipped at z = 64; with the eye inside the
   box (+-256) nothing is sent, so the room stays visible. RUGRATS_OCCL=0 = off, RUGRATS_OCCLLOG=1. */
enum { OCCL_HOLD = 30, OCCL_IDS = 256 };
#define OCCL_PAD_PX 3.0
void gte_cam(double r[9], double t[3]);
void gte_proj_params(double *ox, double *oy, double *h, double *w);
void gpu_occl_box(uint32_t id, const float (*v)[3], int n);
int hw_occl_hidden(uint32_t id, int hold);
void hw_occl_reset(void);
static int occl_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_OCCL"); on = e ? atoi(e) != 0 : 1; }
    return on && gpu_hw_on();
}
static int occl_send(uint32_t key, const double *lo, const double *hi);
static void occl_box(uint32_t cur, int nb) {
    static uint32_t sent[OCCL_IDS];
    if (!occl_on() || nb < 0 || nb >= OCCL_IDS || sent[nb] == present_count + 1) return;
    sent[nb] = present_count + 1;
    const uint32_t nr = mem_r32(0x800C4A8Cu) + (uint32_t)nb * 0x70;
    double lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        const int32_t o = (int32_t)mem_r32(cur + 4 * a);
        lo[a] = (double)((int32_t)mem_r32(nr + 16 + 4 * a) - o); hi[a] = (double)((int32_t)mem_r32(nr + 32 + 4 * a) - o);
        if (hi[a] < lo[a]) return;
    }
    occl_send(0x100u + (uint32_t)nb, lo, hi);
}
/* box lo..hi in the coordinates of the GTE's current matrix -> occlusion query `key`; 0 = nothing sent (eye inside) */
static int occl_send(uint32_t key, const double *lo, const double *hi) {
    double R[9], T[3], ox, oy, hh, ww;
    gte_cam(R, T);
    gte_proj_params(&ox, &oy, &hh, &ww);
    /* The box is padded by OCCL_PAD_PX screen pixels' worth of units at its far end (second pass): the game's own
       projection (GTE divide approximation, whole pixels) can sit 1-2 px off this exact one, and a thin far strip
       seen edge-on then fell outside its box (floor line under the golf net, look-up regression 2026-11-16). */
    double c[8][3], pad = 0;
    for (int pass = 0; pass < 2; pass++) {
        double zmax = 0;
        for (int k = 0; k < 8; k++) {
            const double v[3] = { k & 1 ? hi[0] + pad : lo[0] - pad, k & 2 ? hi[1] + pad : lo[1] - pad, k & 4 ? hi[2] + pad : lo[2] - pad };
            for (int i = 0; i < 3; i++) c[k][i] = (R[i * 3] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2]) / 4096.0 + T[i];
            if (c[k][2] > zmax) zmax = c[k][2];
        }
        pad = OCCL_PAD_PX * zmax / (hh * ww > 1 ? hh * ww : 1);
        if (pad > 1024) pad = 1024;
    }
    /* faces: z lo, z hi, y lo, y hi, x lo, x hi; only the (at most 3) faces turned to the eye are drawn: they cover
       the same pixels as the whole box at its nearest depth, with half the fill */
    static const uint8_t face[6][4] = { { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 } };
    /* eye distance in front of each face, in camera space (eye = origin): normal = cross product of two face edges,
       turned away from the box centre (works whatever scaling the game's matrix carries). No face more than 256 in
       front = eye inside the box (+-256): no query. */
    double fd[6], far = -1e30, ctr[3];
    for (int i = 0; i < 3; i++) ctr[i] = (c[0][i] + c[7][i]) * 0.5;
    for (int f = 0; f < 6; f++) {
        const double *p0 = c[face[f][0]], *p1 = c[face[f][1]], *p3 = c[face[f][3]];
        const double u[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, w[3] = { p3[0] - p0[0], p3[1] - p0[1], p3[2] - p0[2] };
        double nx = u[1] * w[2] - u[2] * w[1], ny = u[2] * w[0] - u[0] * w[2], nz = u[0] * w[1] - u[1] * w[0];
        const double len = sqrt(nx * nx + ny * ny + nz * nz);
        if (len <= 0) { fd[f] = 0; continue; }   /* flat box: this face has no area */
        if (nx * (p0[0] - ctr[0]) + ny * (p0[1] - ctr[1]) + nz * (p0[2] - ctr[2]) < 0) { nx = -nx; ny = -ny; nz = -nz; }
        fd[f] = -(nx * p0[0] + ny * p0[1] + nz * p0[2]) / len;
        if (fd[f] > far) far = fd[f];
    }
    if (far <= 256) return 0;
    float out[72][3];
    int n = 0;
    const double zn = 64.0;
    for (int f = 0; f < 6; f++) {
        if (fd[f] <= 0) continue;
        double p[8][3];
        int np = 0;
        for (int k = 0; k < 4; k++) {   /* clip the quad at z >= zn */
            const double *a = c[face[f][k]], *b = c[face[f][(k + 1) & 3]];
            if (a[2] >= zn) { memcpy(p[np++], a, sizeof p[0]); }
            if ((a[2] >= zn) != (b[2] >= zn)) {
                const double t = (zn - a[2]) / (b[2] - a[2]);
                p[np][0] = a[0] + (b[0] - a[0]) * t; p[np][1] = a[1] + (b[1] - a[1]) * t; p[np][2] = zn; np++;
            }
        }
        if (np < 3) continue;
        float q[8][3];
        for (int k = 0; k < np; k++) {
            double x = hh * ww * p[k][0] / p[k][2] + ox, y = hh * p[k][1] / p[k][2] + oy;
            if (x < -30000) x = -30000; if (x > 30000) x = 30000;
            if (y < -30000) y = -30000; if (y > 30000) y = 30000;
            q[k][0] = (float)x; q[k][1] = (float)y; q[k][2] = (float)(4096.0 / p[k][2]);
        }
        for (int k = 1; k + 1 < np && n + 3 <= 72; k++) {
            memcpy(out[n++], q[0], sizeof q[0]); memcpy(out[n++], q[k], sizeof q[0]); memcpy(out[n++], q[k + 1], sizeof q[0]);
        }
    }
    if (n) gpu_occl_box(key, out, n);
    return n != 0;
}
static int occl_hidden(int nb) {
    static int lg = -1;
    if (!occl_on() || nb < 0 || nb >= OCCL_IDS) return 0;
    const int h = hw_occl_hidden(0x100u + (uint32_t)nb, OCCL_HOLD);
    if (lg < 0) lg = getenv("RUGRATS_OCCLLOG") != NULL;
    if (lg && h) rt_logf("f%u room %d hidden (occlusion)\n", present_count, nb);
    return h;
}
/* Extended View Distance (2026-11-17, F1 Enhancements, opt.view_ext; user: the extra rooms the low-wall / over-wall
   rules bring in cost raw polygons in the maze and Reptar 2010). Those rules only force or widen a room whose box
   (room +0x10 min / +0x20 max, world words) lies within vd_lim of the eye (cam+20); the game's own passes and rects
   are never touched. 0 = Off (no extra rooms at all), 1 Short, 2 Medium, 3 Full (no limit = before this option).
   RUGRATS_VIEWDIST=<units> overrides the distance (debug; 0 = no limit). */
static double vd_lim2;            /* squared limit, < 0 = none */
static int32_t vd_eye[3];
static int vd_lg = -1;
static void vd_frame(uint32_t cam) {
    static const double lim[4] = { 0, 6000, 12000, -1 };   /* Off, Short, Medium, Full */
    static double env = -2;
    if (env == -2) { const char *e = getenv("RUGRATS_VIEWDIST"); env = e ? atof(e) : -1; }
    if (vd_lg < 0) vd_lg = getenv("RUGRATS_VIEWDISTLOG") != NULL;
    const int v = opt.view_ext < 0 ? 0 : opt.view_ext > 3 ? 3 : opt.view_ext;
    const double l = env >= 0 && v ? (env > 0 ? env : -1) : lim[v];
    vd_lim2 = l < 0 ? -1 : l * l;
    for (int a = 0; a < 3; a++) vd_eye[a] = (int32_t)mem_r32(cam + 20 + 4 * a);
}
static double vd_dist2(int rm) {
    const uint32_t room = mem_r32(0x800C4A8Cu) + (uint32_t)rm * 0x70;
    double d2 = 0;
    for (int a = 0; a < 3; a++) {
        const double e = vd_eye[a], mn = (int32_t)mem_r32(room + 16 + 4 * a), mx = (int32_t)mem_r32(room + 32 + 4 * a);
        const double d = e < mn ? mn - e : e > mx ? e - mx : 0;
        d2 += d * d;
    }
    return d2;
}
/* 1 = an extra room may be brought in (within the Extended View Distance) */
static int vd_ok(int rm) {
    if (opt.view_ext <= 0) return 0;
    if (vd_lim2 < 0 || rm < 0) return 1;
    const double d2 = vd_dist2(rm);
    if (vd_lg && d2 > vd_lim2) rt_logf("f%u room %d beyond view distance (%.0f)\n", present_count, rm, sqrt(d2));
    return d2 <= vd_lim2;
}
enum { LW_SEEN_MAX = 48 };
static int lw_room_seen(uint32_t cur, int nb, int from, uint32_t rect, int ahead) {
    const uint32_t tbl = mem_r32(0x800C4A8Cu);
    const int n = (int32_t)mem_r32(0x800C3E80u + 0xc1c);
    int q[LW_SEEN_MAX], d[LW_SEEN_MAX], nq = 0;
    if (nb < 0 || nb >= n || n > 4096) return 0;
    q[nq] = from; d[nq++] = -1;   /* visited, never tested */
    q[nq] = nb; d[nq++] = 0;
    for (int h = 1; h < nq; h++) {
        const int rm = q[h];
        if (!vd_ok(rm)) continue;   /* past the Extended View Distance: not drawn, not looked past */
        if (ow_room_on_screen(cur, rm, rect)) {   /* on screen and not known to be hidden behind nearer walls */
            occl_box(cur, rm);
            if (!occl_hidden(rm)) return 1;
        }
        if (d[h] >= ahead) continue;
        const uint32_t nr = tbl + (uint32_t)rm * 0x70;
        const int np = (int)(mem_r32(nr + 48) & 0xff);
        const uint32_t pa = mem_r32(nr + 52);
        if (np > 16 || (pa >> 24) != 0x80u) continue;
        for (int i = 0; i < np && nq < LW_SEEN_MAX; i++) {
            const int nb2 = (int16_t)mem_r16(pa + i * 48 + 14);
            if (nb2 < 0 || nb2 >= n) continue;
            int seen = 0;
            for (int k = 0; k < nq; k++) if (q[k] == nb2) { seen = 1; break; }
            if (!seen) { q[nq] = nb2; d[nq++] = d[h] + 1; }
        }
    }
    return 0;
}
int rt_hook_80033278(void) {
    static int pad = -1, all = -1, lg = -1, maxa;
    int in = portal_in;
    if (pad < 0) { const char *e = getenv("RUGRATS_PORTALPAD"); pad = e ? atoi(e) : 64; if (pad < 0) pad = 0; }
    if (all < 0) { const char *e = getenv("RUGRATS_PORTALPADALL"); all = e ? atoi(e) : 0; }
    static int eyeout = -1;
    if (lg < 0) lg = getenv("RUGRATS_PORTALLOG") != NULL;
    if (eyeout < 0) { const char *e = getenv("RUGRATS_EYEOUT"); eyeout = e ? atoi(e) != 0 : 1; }
    if (in || (!pad && !lg && !eyeout)) return 0;
    const uint32_t r = cpu.r[5], out = cpu.r[6], portal = cpu.r[4];
    const uint16_t s0 = mem_r16(r), s1 = mem_r16(r + 2), s2 = mem_r16(r + 4), s3 = mem_r16(r + 6);
    /* pad only a parent rect that covers the whole screen rect (0x800C4A00 {x, y, w, h}: the camera's own room and
       rooms seen through a portal that fills the screen); RUGRATS_PORTALPADALL=1: every level (overran the game in
       several saves: the picture turned into one wall colour for good) */
    const int16_t vx = (int16_t)mem_r16(0x800C4A00u), vy = (int16_t)mem_r16(0x800C4A02u);
    const int16_t vw = (int16_t)mem_r16(0x800C4A04u), vh = (int16_t)mem_r16(0x800C4A06u);
    const int full = (int16_t)s0 <= vx && (int16_t)s1 <= vy && (int16_t)s0 + (int16_t)s2 >= vx + vw &&
                     (int16_t)s1 + (int16_t)s3 >= vy + vh;
    (void)maxa;
    int use = pad && r != out && (int16_t)s2 > 0 && (int16_t)s3 > 0 && vw > 0 && vh > 0 && (all || full);
    if (use) {
        mem_w16(r, (uint16_t)((int16_t)s0 - pad)); mem_w16(r + 2, (uint16_t)((int16_t)s1 - pad));
        mem_w16(r + 4, (uint16_t)((int16_t)s2 + 2 * pad)); mem_w16(r + 6, (uint16_t)((int16_t)s3 + 2 * pad));
    }
    portal_in = 1;
    f_80033278();
    portal_in = 0;
    if (use) { mem_w16(r, s0); mem_w16(r + 2, s1); mem_w16(r + 4, s2); mem_w16(r + 6, s3); }
    vd_frame(mem_r32(cpu.r[29] + 124));
    const int vd_nb = (int16_t)mem_r16(portal + 14), vd_in = vd_ok(vd_nb);   /* Extended View Distance */
    /* Eye outside the camera room (2026-11-12, space station elevator, db 3x): the scripted elevator camera hangs
       ~3000 units beside the shaft and looks at it through the shaft's back-face-culled wall, so the room above is
       NOT seen only through the shaft-top portal; its projected rect (a thin band) culled the upper shaft walls and
       the starfield showed through. The game itself passes a portal with the parent rect when the eye is within
       2048 of it (800307f8). Same here when the camera room's own portals (parent = screen rect 0x800C4A00) are
       tested while the eye lies outside that room's box (room +0x10 min / +0x20 max, words): pass, out = screen
       rect. Only the camera room's neighbours, so the room list stays small (a full rect for every room overflowed
       the primitive buffer). RUGRATS_EYEOUT=0 = off. Eye = cam+20 (cam at the caller's sp+124), room index = s2. */
    if (eyeout && r == 0x800C4A00u && r != out) {
        const uint32_t cam = mem_r32(cpu.r[29] + 124), room = mem_r32(0x800C4A8Cu) + cpu.r[18] * 0x70;
        int outside = 0;
        for (int i = 0; i < 3; i++) {
            const int32_t e = (int32_t)mem_r32(cam + 20 + 4 * i);
            if (e < (int32_t)mem_r32(room + 16 + 4 * i) - 64 || e > (int32_t)mem_r32(room + 32 + 4 * i) + 64) outside = 1;
        }
        if (outside) {
            if (lg && !cpu.r[2]) rt_logf("f%u portal %08x: eye outside room %u, forced pass\n", present_count, portal, cpu.r[18]);
            cpu.r[2] = 1;
            mem_w16(out, s0); mem_w16(out + 2, s1); mem_w16(out + 4, s2); mem_w16(out + 6, s3);
        }
    }
    /* Eye over the wall (2026-11-14, Grandpa's Teeth hedge maze, user's save: still camera tilted high). The stick
       camera can lift the eye far above where the game's own camera ever goes (there 2700 units over the corridor
       floor, 2200 above the hedge tops), so it sees over the walls into rooms linked only through portals behind or
       below the view. The game's near rule (800307f8, eye within 2048 of the portal) then fails, the portal projects
       off screen, and that room and everything past it was culled: one big brown area (clear colour) across the
       hedge until the eye came back down. And a room that did pass got the portal's narrow rect, so its hedge
       geometry outside the doorway was clipped (a brown strip along the hedge top).
       While the manual camera's eye is in use and lies above the portal's top edge (y down: eye y < room origin y +
       min corner y), at every traversal level:
       - a passing portal keeps the parent rect (out = parent) instead of the narrowed one;
       - a failing portal passes with the parent rect when it is within 4096 units of the eye in x/z (a stepping
         stone: the hedge maze's far side hangs off a room behind the camera) or its room's box is on screen
         (ow_room_on_screen), at most OVERWALL_MAX per frame (passing every room overflowed the primitive buffer
         once: PORTALPADALL). Without these two gates the whole maze (24+ rooms) passed from one high view.
       The game's own camera never triggers it (eye must be the manual one). RUGRATS_OVERWALL=0 = off,
       2 = every level (debug).
       Only in the levels of the gap table (mazes, hooks_gap_level) since 2026-11-14: in the house hub (user's save
       in the hallway by the stairs) the eye is above the top of the stairway portal, so the passing stairs portal
       kept the full rect and the whole upstairs passed too: the picture turned into a mess of long lines for good
       (same failure as PORTALPADALL). Walls there reach the ceiling, so nothing is seen over them anyway.
       Also in the levels of ow_levels[] (outdoor levels the camera looks down on, without the maze's gap colour /
       back faces): Reptar 2010 (db 7, user's save looking down past a corner building: the street around the corner
       was black until the camera came down; with the rule the street and the truck on it show; 3-6 rooms pass a
       frame there).
       Room index = s2 (80033ac4 loop), eye = cam+20 (cam at the caller's sp+124). */
    {
        static int overwall = -1;
        static uint32_t ow_frame, ow_n;
        enum { OVERWALL_MAX = 24 };
        static const int ow_levels[] = { 7 };   /* Reptar 2010 */
        if (overwall < 0) { const char *e = getenv("RUGRATS_OVERWALL"); overwall = e ? atoi(e) : 1; }
        if (ow_frame != present_count) { ow_frame = present_count; ow_n = 0; }
        int ow_lvl = overwall == 2 || hooks_gap_level();
        if (overwall && !ow_lvl) {
            const int db = (int32_t)mem_r32(0x800C5678u);
            for (size_t i = 0; i < sizeof ow_levels / sizeof ow_levels[0]; i++) if (ow_levels[i] == db) ow_lvl = 1;
        }
        const uint32_t cam = mem_r32(cpu.r[29] + 124);
        if (overwall && ow_lvl && pkt_rooms_ok() && r != out && (int16_t)s2 > 0 && (int16_t)s3 > 0 &&
            rcam_eye_in_use(cam)) {
            const uint32_t room = mem_r32(0x800C4A8Cu) + cpu.r[18] * 0x70;
            int top = 0x7fffffff;
            for (int v = 0; v < 4; v++) { const int y = (int16_t)mem_r16(portal + v * 8 + 2); if (y < top) top = y; }
            if ((int32_t)mem_r32(cam + 24) < (int32_t)mem_r32(room + 4) + top) {
                /* gte.c NCLIP: draw room back faces this frame (hedge backs). Only in the levels of the gap table
                   (mazes): in the house they added thin slivers along door frames (regression 2026-11-14).
                   RUGRATS_BACKFACE=0 off, 2 = every level (debug). */
                static int backface = -1;
                if (backface < 0) { const char *e = getenv("RUGRATS_BACKFACE"); backface = e ? atoi(e) : 1; }
                if (backface == 2 || (backface && hooks_backface_level())) gte_backface_frame = present_count;
                ow_over_frame = present_count;
                int pass = cpu.r[2] != 0;
                if (!vd_in) pass = 0;   /* past the Extended View Distance: the game's own result and rect stay */
                else if (!pass && ow_n < OVERWALL_MAX) {
                    double hd2 = 0; /* horizontal distance from the eye to the portal's box (room-local x/z) */
                    for (int a = 0; a < 3; a += 2) {
                        int mn = 0x7fffffff, mx = -0x7fffffff;
                        for (int v = 0; v < 4; v++) {
                            const int c = (int16_t)mem_r16(portal + v * 8 + a * 2);
                            if (c < mn) mn = c;
                            if (c > mx) mx = c;
                        }
                        const double e = (double)(int32_t)mem_r32(cam + 20 + 4 * a) - (int32_t)mem_r32(room + 4 * a);
                        const double d = e < mn ? mn - e : e > mx ? e - mx : 0;
                        hd2 += d * d;
                    }
                    static double ow_dist = -1; /* RUGRATS_OVERWALLDIST: stepping-stone distance (debug) */
                    if (ow_dist < 0) { const char *e = getenv("RUGRATS_OVERWALLDIST"); ow_dist = e ? atof(e) : 4096; }
                    if (hd2 <= ow_dist * ow_dist || ow_room_on_screen(room, (int16_t)mem_r16(portal + 14), r)) {
                        if (lg) rt_logf("f%u portal %08x: eye over the wall of room %u, forced pass\n", present_count, portal, cpu.r[18]);
                        ow_n++;
                        pass = 1;
                    }
                }
                if (pass) {
                    cpu.r[2] = 1;
                    mem_w16(out, s0); mem_w16(out + 2, s1); mem_w16(out + 4, s2); mem_w16(out + 6, s3);
                }
            }
        }
    }
    /* Low walls (2026-11-14, Grandpa's Teeth hedge maze, user's save: game camera standing still): the hedges are
       lower than the camera, so a neighbour room's floor shows OVER the hedge as well as through the doorway. The
       game hands the room the doorway portal's projected rect and culls its floor chunks outside it: a jagged edge
       of missing floor between the trees past the hedge, filling in as the camera turned. In the levels of the gap
       table (mazes, hooks_gap_level) a passing portal keeps the parent rect, at any traversal level, while fewer
       than LOWWALL_PASSES portals have passed this frame (the game's own count: 2-4 in these levels). Unlimited (first
       version) the open backyard passed up to 51 rooms a frame and the game broke (picture turned to noise for good,
       same failure as PORTALPADALL; its room queue at 0x800E00C0 holds ~65). Camera room only (second version) left
       a doorway-shaped cut on the room two doorways away (user's second hedge save). RUGRATS_LOWWALL=0 = off.
       Also Reptar 2010 (hooks_backface_level, 2026-11-14, user's save looking down the street past a corner building:
       buildings / street at the top left missing until the camera moved): 4 rooms whose box is on screen pass; the
       frame is then pixel-identical to RUGRATS_PORTALPADALL=1 (every portal passes) on both user saves. */
    {
        static int lowwall = -1;
        static uint32_t lw_frame, lw_pass;
        enum { LOWWALL_PASSES = 12, LOWWALL_AHEAD = 6 };
        /* Box mode cap (Reptar 2010): 12 was hit in every frame on the user's street save, and which doorway got the
           last pass alternated frame to frame as the camera turned: a room (or part of one) on, off, on (2026-11-14,
           "polygons flicking along both sides of the street"). With the 2 MB packet ring the box mode gets 40
           (game's room queue 0x800E00C0 holds 98 entries, rect array 0x800E03D0 ~146). RUGRATS_LOWWALLBOX=<n>. */
        static int box_cap = -1;
        if (box_cap < 0) { const char *e = getenv("RUGRATS_LOWWALLBOX"); box_cap = e ? atoi(e) : 40; }
        if (lowwall < 0) { const char *e = getenv("RUGRATS_LOWWALL"); lowwall = e ? atoi(e) != 0 : 1; }
        if (lw_frame != present_count) {
            lw_frame = present_count; lw_pass = 0;
            /* occlusion answers describe the previous frames' view: after a camera cut (eye more than 600 units
               away from last frame's) they are dropped, so nothing is skipped until new answers arrive */
            static int32_t oe[3];
            const uint32_t cam = mem_r32(cpu.r[29] + 124);
            int jump = 0;
            for (int a = 0; a < 3; a++) {
                const int32_t e = (int32_t)mem_r32(cam + 20 + 4 * a);
                if (e - oe[a] > 600 || oe[a] - e > 600) jump = 1;
                oe[a] = e;
            }
            if (jump) hw_occl_reset();
        }
        /* Box mode (levels outside the gap table, i.e. Reptar 2010): the doorway's rect is widened only to where the
           room's box covers the screen (ow_room_rect), not to the parent rect. With the parent rect the park of
           Reptar 2010 went from 18 to 36 rooms a frame (the widened rects let the game pass more rooms on its own)
           and broke for good (lines, then noise textures). */
        int16_t bx[4];
        if (lowwall && vd_in && pkt_rooms_ok() && r != out && (int16_t)s2 > 0 && (int16_t)s3 > 0 &&
            lw_pass < (uint32_t)box_cap && !hooks_gap_level() && hooks_backface_level()) {
            const uint32_t cur = mem_r32(0x800C4A8Cu) + cpu.r[18] * 0x70;
            /* Hold (2026-11-14, user: polygons flicking along the street): a room whose box just touches the edge of
               the parent rect (1 px wide) was in one frame and out the next as the camera turned, so it and the rooms
               past it blinked. A doorway the box test passed keeps passing for 15 vblanks (0.25 s) with its last rect.
               RUGRATS_BOXHOLD=<vblanks>, 0 = off. */
            enum { BOXHOLD_N = 64 };
            static struct { uint32_t portal, vbl; int16_t rc[4]; } hold[BOXHOLD_N];
            static int hold_vbl = -1;
            if (hold_vbl < 0) { const char *e = getenv("RUGRATS_BOXHOLD"); hold_vbl = e ? atoi(e) : 15; }
            int vis = ow_room_rect(cur, (int16_t)mem_r16(portal + 14), r, bx), held = 0;
            const uint32_t now = hooks_frame();
            int slot = -1, oldest = 0;
            for (int k = 0; k < BOXHOLD_N; k++) {
                if (hold[k].portal == portal) { slot = k; break; }
                if ((int32_t)(hold[oldest].vbl - hold[k].vbl) > 0) oldest = k;
            }
            if (vis) {
                if (slot < 0) slot = oldest;
                hold[slot].portal = portal; hold[slot].vbl = now; memcpy(hold[slot].rc, bx, sizeof bx);
            } else if (!cpu.r[2] && hold_vbl && slot >= 0 && now - hold[slot].vbl <= (uint32_t)hold_vbl) {
                memcpy(bx, hold[slot].rc, sizeof bx);
                vis = held = 1;
            }
            if (vis) {
                if (held && lg) rt_logf("f%u portal %08x: low walls (box), held\n", present_count, portal);
                int x0 = bx[0], y0 = bx[1], x1 = bx[0] + bx[2], y1 = bx[1] + bx[3];
                if (cpu.r[2]) {   /* passing: union with the game's own rect */
                    const int ox = (int16_t)mem_r16(out), oy = (int16_t)mem_r16(out + 2);
                    const int ow = (int16_t)mem_r16(out + 4), oh = (int16_t)mem_r16(out + 6);
                    if (ow > 0 && oh > 0) {
                        if (ox < x0) x0 = ox; if (oy < y0) y0 = oy;
                        if (ox + ow > x1) x1 = ox + ow; if (oy + oh > y1) y1 = oy + oh;
                    }
                }
                if (lg) rt_logf("f%u portal %08x: low walls (box), %s, rect %d,%d %dx%d\n", present_count, portal,
                                cpu.r[2] ? "widened" : "forced pass", x0, y0, x1 - x0, y1 - y0);
                cpu.r[2] = 1;
                mem_w16(out, (uint32_t)x0); mem_w16(out + 2, (uint32_t)y0);
                mem_w16(out + 4, (uint32_t)(x1 - x0)); mem_w16(out + 6, (uint32_t)(y1 - y0));
            }
        } else if (lowwall && vd_in && pkt_rooms_ok() && r != out && (int16_t)s2 > 0 && (int16_t)s3 > 0 &&
            lw_pass < LOWWALL_PASSES && hooks_gap_level()) {
            /* a failing doorway (off screen, e.g. behind the camera's side) still passes when its room's box is on
               screen: that room shows over the hedge (user's second save: room 49 left of the camera room) */
            const uint32_t cur = mem_r32(0x800C4A8Cu) + cpu.r[18] * 0x70;
            const int nb = (int16_t)mem_r16(portal + 14);
            int widen = 1;
            if (!cpu.r[2]) {
                if (lw_room_seen(cur, nb, (int)cpu.r[18], r, LOWWALL_AHEAD)) {
                    if (lg) rt_logf("f%u portal %08x: low walls, room on screen, forced pass\n", present_count, portal);
                    cpu.r[2] = 1;
                }
            } else {
                /* the game's own pass: the doorway's rect is widened unless the room is hidden (occlusion test) and
                   so is everything on screen past it; then it keeps the game's own rect, as on the PS1 */
                occl_box(cur, nb);
                if (occl_hidden(nb) && !lw_room_seen(cur, nb, (int)cpu.r[18], r, LOWWALL_AHEAD)) widen = 0;
            }
            if (cpu.r[2] && widen) {
                if (lg) rt_logf("f%u portal %08x: low walls, parent rect kept\n", present_count, portal);
                mem_w16(out, s0); mem_w16(out + 2, s1); mem_w16(out + 4, s2); mem_w16(out + 6, s3);
            }
        }
        if (cpu.r[2]) lw_pass++;
        /* Tree and hedge backs (2026-11-14, user): in the same levels, room faces seen from behind are drawn in every
           frame, not only while the stick camera is over a wall (gte.c NCLIP, gte_backface_frame).
           RUGRATS_BACKFACEALL=0 = only over the wall (as before). */
        static int bfall = -1;
        if (bfall < 0) { const char *e = getenv("RUGRATS_BACKFACEALL"); bfall = e ? atoi(e) != 0 : 1; }
        /* Every frame only in the maze levels. Reptar 2010 (bf_levels) gets them only while the stick camera's eye is
           over the walls (over-wall block above): at street level the reverse sides of the short side walls where
           balcony sections step out came out as black bars that blinked as the camera bobbed (user, 2026-11-14). */
        if (bfall && hooks_gap_level()) gte_backface_frame = present_count;
    }
    if (lg)
        rt_logf("f%u portal %08x parent %08x %d,%d %dx%d%s -> %d out %08x %d,%d %dx%d\n", present_count, portal, r, (int16_t)s0,
                (int16_t)s1, (int16_t)s2, (int16_t)s3, use ? " padded" : "", (int)cpu.r[2], out, (int16_t)mem_r16(out),
                (int16_t)mem_r16(out + 2), (int16_t)mem_r16(out + 4), (int16_t)mem_r16(out + 6));
    return 1;
}

/* Object visibility test 80031834(radius, rect*), called by the object draw 80031b84 with the object's camera-space
   position in GTE TR. It projects the centre (TRx*H/TRz, TRy*H/TRz) and the radius (scaled by the screen size at
   0x800C4A04/06 over 320/240) on the CPU and returns 0 when the circle is outside rect {x, y, w, h}, 1 otherwise
   (also 1 when the object reaches the camera plane, 0 when it is fully behind). The CPU projection bypasses the GTE's
   widescreen squeeze, so objects vanished in the 16:9 side bands. Native copy of the same integer maths (32-bit
   products, truncating divides) plus the squeeze on the projected x and x radius when widescreen + widecull. */
/* Cutscene props (2026-11-13, user: Let There Be Light, Triangle on Spike, fridge cutscene). In-engine cutscene shots
   were framed for 4:3; widescreen culling (above) lets in props just past the 4:3 edge that were never staged for the
   shot. The fridge's right door swings through the dark prop standing beside the fridge (db 12, world 32081,181,11572;
   checked in PCSX-Redux: same door motion, prop culled there): a dark wedge cut through the open door. While a
   streamed cutscene runs (8007412c called in the last 10 vblanks), the props listed here keep the original 4:3 cull.
   RUGRATS_CUTPROPS=0 = off. */
static uint32_t cut_last_vbl, cut_seen;
static int cutscene_prop(uint32_t obj) {
    static const struct { int db; int32_t x, y, z; } props[] = {
        { 12, 32081, 181, 11572 },   /* beside the fridge (fridge cutscene) */
    };
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_CUTPROPS"); on = e ? atoi(e) != 0 : 1; }
    if (!on || !cut_seen || hooks_frame() - cut_last_vbl > 10) return 0;
    const int db = (int)mem_r32(0x800C5678u);
    const int32_t x = (int32_t)mem_r32(obj + 0x18), y = (int32_t)mem_r32(obj + 0x1c), z = (int32_t)mem_r32(obj + 0x20);
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++)
        if (props[i].db == db && props[i].x == x && props[i].y == y && props[i].z == z) return 1;
    return 0;
}

static int32_t div_trunc(int32_t a, int32_t b) {
    if (b == 0) return a >= 0 ? -1 : 1;                 /* MIPS div by zero (the game breaks before that) */
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}
static uint32_t occl_still_obj;   /* object being drawn when it is a still prop, else 0 (dcls_run) */
int rt_hook_80031834(void) {
    int32_t z = (int32_t)gte_read(39), x = (int32_t)gte_read(37), y = (int32_t)gte_read(38);
    int32_t h = (int32_t)gte_read(58);
    int32_t r = (int32_t)cpu.r[4];
    uint32_t rect = cpu.r[5];
    cpu.r[2] = 0;
    if (z < -r) return 1;                               /* behind the camera */
    if (z < r) { cpu.r[2] = 1; return 1; }              /* touches the camera plane: draw */
    int32_t sx = div_trunc((int32_t)((uint32_t)x * (uint32_t)h), z);
    int32_t sy = div_trunc((int32_t)((uint32_t)y * (uint32_t)h), z);
    int32_t t = (int32_t)((uint32_t)r * (uint32_t)(int32_t)(int16_t)mem_r16(0x800C4A04u));
    int32_t rx = div_trunc((int32_t)((uint32_t)(t / 320) * (uint32_t)h), z);
    int32_t u = (int32_t)((uint32_t)r * (uint32_t)(int32_t)(int16_t)mem_r16(0x800C4A06u));
    int32_t ry = div_trunc((int32_t)((uint32_t)(u / 240) * (uint32_t)h), z);
    const int32_t sx43 = sx, rx43 = rx;
    if (opt.widescreen && opt.widecull && !(cpu.r[31] == 0x80031c9cu && cutscene_prop(cpu.r[16]))) {
        sx = gte_ws_x(sx); rx = gte_ws_x(rx);
    }
    int32_t rx0 = (int16_t)mem_r16(rect), ry0 = (int16_t)mem_r16(rect + 2);
    int32_t rw = (int16_t)mem_r16(rect + 4), rh = (int16_t)mem_r16(rect + 6);
    {   /* debug: RUGRATS_OBJCULLLOG=1 logs objects whose pass/cull differs between the 16:9 and the 4:3 test */
        static int lg = -1;
        if (lg < 0) lg = getenv("RUGRATS_OBJCULLLOG") != NULL;
        if (lg) {
            const int p43 = !(sx43 < rx0 - rx43 || rx0 + rw + rx43 < sx43), pw = !(sx < rx0 - rx || rx0 + rw + rx < sx);
            if (p43 != pw)
                rt_logf("f%u objcull ra %08x s0 %08x s1 %08x r %d z %d sx %d/%d rect %08x %d,%d %dx%d: %s\n", present_count,
                        cpu.r[31], cpu.r[16], cpu.r[17], r, z, sx43, sx, rect, rx0, ry0, rw, rh, pw ? "16:9 only" : "4:3 only");
        }
        static int hide = -1; static uint32_t hid;   /* debug: RUGRATS_OBJHIDE=<hex s0> culls that object */
        if (hide < 0) { const char *e = getenv("RUGRATS_OBJHIDE"); hide = e != NULL; if (e) hid = (uint32_t)strtoul(e, 0, 16); }
        if (hide && cpu.r[16] == hid) return 1;
        static int all = -1;   /* debug: RUGRATS_OBJCULLALL=1 logs every test (obj, centre, radius, rect, result) */
        if (all < 0) all = getenv("RUGRATS_OBJCULLALL") != NULL;
        if (all) {
            const int pass = !(sx < rx0 - rx || rx0 + rw + rx < sx) && !(sy < ry0 - ry || ry0 + rh + ry < sy);
            rt_logf("f%u objall ra %08x s0 %08x r %d z %d sx %d rx %d sy %d ry %d rect %d,%d %dx%d -> %d\n", present_count,
                    cpu.r[31], cpu.r[16], r, z, sx, rx, sy, ry, rx0, ry0, rw, rh, pass);
        }
    }
    if (sx < rx0 - rx || rx0 + rw + rx < sx) return 1;
    if (sy < ry0 - ry || ry0 + rh + ry < sy) return 1;
    cpu.r[2] = 1;
    /* Occlusion (2026-11-16, see occl_box): world objects of the object draw (ra 80031c9c, s0 = object) in the gap
       levels get an occlusion query too: a screen square around the sphere at its nearest depth (z - r). Hidden
       behind room walls = culled like the sphere test does. Only props that have not moved (prop_still, set by the
       object draw wrapper in occl_still_obj): characters and moving things draw more than what is inside their
       sphere (a small patch at the foot of the golf net went missing in the look-up regression, save bk_golf-aim).
       RUGRATS_OCCLOBJ=0 = off. */
    {
        static int on = -1;
        if (on < 0) { const char *e = getenv("RUGRATS_OCCLOBJ"); on = e ? atoi(e) != 0 : 1; }
        const uint32_t obj = cpu.r[16];
        if (on && cpu.r[31] == 0x80031c9cu && (obj >> 24) == 0x80u && obj == occl_still_obj && z - r >= 64 && r > 0 &&
            occl_on() && hooks_gap_level()) {
            int hw_occl_seen(uint32_t id, int age);
            if (!(hw_occl_seen(obj, 12) && ((present_count + (obj >> 3)) & 7))) {
                double ox, oy, hh, ww;
                gte_proj_params(&ox, &oy, &hh, &ww);
                /* screen bound of the sphere: the cube around it, each side projected at the cube's near and far
                   depth, whichever reaches further out */
                const double zf = (double)z - r, zb = (double)z + r, kx = hh * ww, ky = hh;
                const double xl = (double)x - r, xr = (double)x + r, yt = (double)y - r, yb = (double)y + r;
                double x0 = kx * (xl / zf < xl / zb ? xl / zf : xl / zb) + ox, x1 = kx * (xr / zf > xr / zb ? xr / zf : xr / zb) + ox;
                double y0 = ky * (yt / zf < yt / zb ? yt / zf : yt / zb) + oy, y1 = ky * (yb / zf > yb / zb ? yb / zf : yb / zb) + oy;
                x0 -= OCCL_PAD_PX; x1 += OCCL_PAD_PX; y0 -= OCCL_PAD_PX; y1 += OCCL_PAD_PX;
                if (x0 < -30000) x0 = -30000; if (x1 > 30000) x1 = 30000;
                if (y0 < -30000) y0 = -30000; if (y1 > 30000) y1 = 30000;
                const float iz = (float)(4096.0 / zf);
                const float q[6][3] = { { (float)x0, (float)y0, iz }, { (float)x1, (float)y0, iz }, { (float)x0, (float)y1, iz },
                                        { (float)x1, (float)y0, iz }, { (float)x1, (float)y1, iz }, { (float)x0, (float)y1, iz } };
                gpu_occl_box(obj, q, 6);
                if (hw_occl_hidden(obj, OCCL_HOLD)) cpu.r[2] = 0;
            }
        }
    }
    return 1;
}

/* ---- level changes (bench / RUGRATS_LEVEL) --------------------------------
   Main loop 80017c0c: s4 = 80018c38(5) runs the current database (hub, level) and returns the next database number;
   then 8006ee14 is called and 0x800C5678 (current database) is set to s4. 80018c38 loops until gp+0xa98
   (0x800C4918) is non-zero (a door / level script sets it; -1 = credits), tears the level down and returns that value.
   RUGRATS_LEVEL=<db> (1..31) lets the hub run for a moment and then sets 0x800C4918 = <db>: the hub leaves through
   its own exit path, as if Tommy took a door. (An earlier version skipped the hub loop entirely; that left its
   memory allocated and big levels, db 19 and 22, failed in MemAlloc.) */
uint32_t hooks_frame(void);
int hooks_level = -1;            /* database running now (as set by the main loop), -1 before the first */
uint32_t hooks_level_vbl;        /* vblank when it started */
static int level_want = -2;
static uint32_t level_exit_at;   /* vblank at which to request the level change (0 = none) */
int rt_hook_80018c38(void) {
    hooks_level = (int)mem_r32(0x800C5678u);
    hooks_level_vbl = hooks_frame();
    rt_logf("level: run db %d (mode %u) at vblank %u\n", hooks_level, cpu.r[4], hooks_level_vbl);
    if (level_want == -2) {
        const char *e = getenv("RUGRATS_LEVEL");
        level_want = e ? atoi(e) : -1;
        if (level_want == 0 || level_want > 31) { rt_logf("level: RUGRATS_LEVEL must be 1..31\n"); level_want = -1; }
        if (level_want > 0 && hooks_level != level_want) level_exit_at = hooks_level_vbl + 30;
    }
    return 0;
}
/* F1 Enhancements "Level Select" (host_win32.c): only while the level loop runs gameplay frames (not on the title,
   in cutscenes or in the pause screen); the request uses the same exit path as RUGRATS_LEVEL at the next vblank. */
int hooks_gameplay(void) { return hooks_level > 0 && present_count - pace_present <= 2; }
void hooks_level_request(int db) {
    level_want = db;
    level_exit_at = hooks_frame() + 1;
    rt_logf("level: level select asks for db %d (now db %d)\n", db, hooks_level);
}
void hooks_level_vblank(void) {
    if (!level_exit_at || hooks_frame() < level_exit_at) return;
    level_exit_at = 0;
    rt_logf("level: leaving db %d for db %d\n", hooks_level, level_want);
    mem_w32(0x800C4918u, (uint32_t)level_want);
}
int rt_hook_8006ee14(void) {
    if (cpu.r[31] == 0x80018210u) rt_logf("level: loop returned, next db %d at vblank %u\n", (int)cpu.r[20], hooks_frame());
    return 0;
}

/* Eye blink (2026-11-13; user: Tommy's blink plays way too fast at 120 fps). 80075698(id), called once per game frame
   per character (player update ra 8005a3c4, others ra 80073f94), steps a blink state machine: obj+0xA5 0 = open ->
   with its own LCG (seed 0x800C4C48) a 1-in-32 chance per call -> 1 = shut, 2 calls -> 2 = half, 2 calls -> 0, then
   80075304(id, state) uploads that eye frame (LoadImage). All counted in game frames: at 30 fps a shut lasts 1/15 s and
   blinks come ~1 per s; at 120 fps 4x shorter and 4x more often. The state machine now steps at the PS1's 30 Hz game
   rate (once per 10 ticks of game time, remainder carried, decided once per game frame for all characters); on the
   other frames only the eye upload runs with the current state (what the original does when nothing changes).
   Only with realtime_step on. RUGRATS_EYEBLINK=0 = every frame (old). */
static int32_t blink_acc;
static uint32_t blink_fc = 0xFFFFFFFFu;
static uint8_t blink_step = 1;
void f_800461f4(void);
void f_80075304(void);
int rt_hook_80075698(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_EYEBLINK"); on = e ? atoi(e) != 0 : 1; }
    if (!on || !opt.fixed_timestep) return 0;
    const uint32_t fc = mem_r32(0x800C3F24u);   /* game frame counter */
    if (fc != blink_fc) {
        blink_fc = fc;
        blink_acc += (int32_t)mem_r32(0x800C3F1Cu);   /* gp+156: ticks this game frame */
        blink_step = blink_acc >= 10;
        if (blink_step) blink_acc -= 10;
        if (blink_acc > 10) blink_acc = 10;
    }
    if (blink_step) return 0;
    const uint32_t ra = cpu.r[31], s0 = cpu.r[16];
    const uint32_t id = cpu.r[4] & 0xFFFF;
    uint32_t obj = 0x800E2B48u;
    if (id != mem_r16(obj + 0x38)) {
        cpu.r[4] = (uint32_t)(int32_t)(int16_t)id; cpu.r[31] = 0x800756ccu; f_800461f4();
        obj = cpu.r[2];
    }
    if (obj) {
        cpu.r[4] = (uint32_t)(int32_t)(int16_t)id; cpu.r[5] = mem_r8(obj + 0xa5); cpu.r[31] = 0x800757c0u;
        f_80075304();
    }
    cpu.r[31] = ra; cpu.r[16] = s0;
    return 1;
}

/* ---- save state (savestate.c) ---------------------------------------- */
#include "savestate.h"
void hooks_state(StateIO *s) {
    st_prefix(s, "hooks");
    if (st_loading(s)) hw_occl_reset();
    ST(s, step_last_cycles); ST(s, step_init); ST(s, acc_ticks); ST(s, acc_secs); ST(s, frame_counter);
    ST(s, blink_acc); ST(s, blink_fc); ST(s, blink_step);
    ST(s, hooks_level); ST(s, hooks_level_vbl); ST(s, level_exit_at);
    /* presents the pacing hook has not consumed yet (the host present counter itself is not game state) */
    uint32_t pending = present_count - frame_present;
    ST(s, pending);
    if (st_loading(s)) frame_present = present_count - pending;
    void hooks_rcam_state(StateIO *s);
    hooks_rcam_state(s);
    void hooks_golf_state(StateIO *s);
    hooks_golf_state(s);
    void hooks_timer_state(StateIO *s);
    hooks_timer_state(s);
    if (st_loading(s)) { void hooks_dmg_reset(void); hooks_dmg_reset(); }
}

/* ---- player collision probe look-ahead (ledge hop loop at high fps) ---------------------------------------
   80064a34(obj, mode) is the movement collision probe (walk body 8005e1f4 calls it with mode 2 from ra 8005e26c,
   the idle handler 8005d11c too). Its look-ahead distance is bbox extent + speed (obj+0x184) x ticks (gp+156 =
   0x800C3F1C); in mode 2 its step-up check 800645d8 also scales obj+0x178.. by ticks. The PS1 never runs a frame
   shorter than 10 ticks (30 fps), so the probe always looked at least that far ahead. Above 30 fps the look-ahead
   shrinks (half at 60 fps). Found on a low ledge in the sewer (7 Voyages of Cynthia): after the ledge hop (state 6,
   anim 5) Spike stands ~150 units short of the ledge face (x -67585); the 10-tick probe point lands past that thin
   face (no hit, he walks on), the 5-tick point lands inside it (hit), so the walk body's low-speed branch
   (80058da4, 80062cf4) starts the hop again from the OLD start point (obj+0x104/0x10c, back at the bottom): Spike
   hopped forever while the stick was held, and stayed on top only after release (idle). Pressing forward again on
   top teleported him back down. Port at 30 fps / RUGRATS_TICKS=10: hops once, walks on (same state, same spot).
   Fix: while the player's probe runs it sees max(ticks, 10) (the real value is put back right after the call).
   Debug: RUGRATS_PROBEFIX=0 (original), RUGRATS_PROBELOG=1 (logs every player probe run with a raised tick count). */
#define PROBE_PLAYER 0x800E2B48u
void f_80064a34(void);
int rt_hook_80064a34(void) {
    static int inside, on = -1, log;
    if (on < 0) {
        const char *e = getenv("RUGRATS_PROBEFIX");
        on = e ? atoi(e) != 0 : 1;
        log = getenv("RUGRATS_PROBELOG") != NULL;
    }
    if (inside || !on || cpu.r[4] != PROBE_PLAYER) return 0;
    const uint32_t tp = cpu.r[28] + 156, t = mem_r32(tp);
    if (t >= 10) return 0;
    const uint32_t ra = cpu.r[31], mode = cpu.r[5];
    mem_w32(tp, 10);
    inside = 1;
    f_80064a34();                                    /* the original, with the PS1's minimum frame step */
    inside = 0;
    mem_w32(tp, t);
    if (log) rt_logf("probe: vbl %u mode %u ra %08x ticks %u->10 result %u\n", hooks_frame(), mode, ra, t, cpu.r[2]);
    return 1;
}

/* ---- player damage cooldown --------------------------------------------------------------------------
   80077864(msg) is the damage event (from the event dispatcher at 800771ac): msg+4 = object, msg+0x10 = other
   object (when set, it is the one damaged), msg+0x14 = amount (int32; negative heals). It subtracts the amount from
   obj+0x1a4 (health, s16, 0..255; 0 -> 8007fd0c death) and runs the hurt reaction 8008c124. The game has no
   invincibility time after a hit: touching a hazard (sewer pipe in 7 Voyages of Cynthia: 15 per hit, Spike 195 HP)
   sends a new event whenever contact repeats, so a few events close together took a big bite at once.
   Here the player (0x800E2B48) ignores a damage event within 120 ticks (0.4 s game time) of the last one
   that went through: the whole event is dropped (no damage, no new knockback), like i-frames. Heals always pass.
   Debug: RUGRATS_DMGCOOL=<ms> (0 = off, the original), RUGRATS_DMGLOG=1 logs every damage event. */
#define DMG_PLAYER 0x800E2B48u
static uint32_t dmg_since = 1u << 30; /* game ticks since the player's last accepted hit */
void hooks_dmg_tick(uint32_t t) { if (dmg_since < (1u << 30)) dmg_since += t; }
void hooks_dmg_reset(void) { dmg_since = 1u << 30; }
int rt_hook_80077864(void) {
    static int cool = -1, log;
    if (cool < 0) {
        const char *e = getenv("RUGRATS_DMGCOOL");
        cool = e ? atoi(e) * 300 / 1000 : 120; /* 0.4 s */
        log = getenv("RUGRATS_DMGLOG") != NULL;
    }
    uint32_t msg = cpu.r[4];
    uint32_t other = mem_r32(msg + 0x10), obj = other ? other : mem_r32(msg + 4);
    int32_t amount = (int32_t)mem_r32(msg + 0x14);
    int drop = obj == DMG_PLAYER && amount > 0 && cool > 0 && dmg_since < (uint32_t)cool;
    if (log)
        rt_logf("dmg: vbl %u obj %08x amount %d health %d since %u ticks%s\n", hooks_frame(), obj, amount,
                (int16_t)mem_r16(obj + 0x1a4), dmg_since, drop ? " -> ignored (cooldown)" : "");
    if (drop) return 1;
    if (obj == DMG_PLAYER && amount > 0) dmg_since = 0;
    return 0;
}

/* ---- right-stick camera (ini [input] rstick_camera / rstick_invert, F1 rows) ------------------------------
   The gameplay camera update 8002953c(cam, player) moves the eye (cam+0x14/18/1c, int32 world units, y down)
   with its own follow logic, then calls 8001e838(cam, ...) (from 80029df0), which builds the target point and
   calls LookAt 800211e8(eye*, target*, matrix*) (from 8001e928) and copies the matrix to cam+0. The camera is
   the struct at 0x800DE460 (MATRIX + eye), pointed to by player+0x224.
   At that LookAt call the eye is orbited around the target and also written to cam+0x14 (the drawing
   translation). At the next 8002953c entry the game's own eye is put back, so its follow logic never sees it.
   Yaw: once the stick turns the camera, its direction is held ABSOLUTE in the world (the game keeps distance,
   height and position). A plain offset spirals: movement is camera-relative, so the player turns toward the
   offset view, the game's follow camera swings behind the player, the offset adds again, and so on.
   Pitch: an offset on the game's elevation (no feedback there). Auto-center (rstick_camera=2): after 1.5 s
   without the stick, eases back to the game's own camera and hands control back. Nothing is changed while
   the stick hasn't been used (or after the hand-back). Other LookAt callers (cutscenes, transitions) are left
   alone; if the gameplay call stops for 1 s (cutscene, level change), the manual camera resets. */
static double rcam_az, rcam_pitch;      /* absolute azimuth (radians, atan2(dz, dx) of eye - target), pitch offset */
static int32_t rcam_game[3], rcam_ours[3];
static uint32_t rcam_cam, rcam_idle;    /* rcam_idle: game ticks (1/300 s) since the stick was last used */
static uint32_t rcam_active, rcam_miss; /* rcam_active: azimuth is ours; rcam_miss: ticks without the gameplay call */
static double rcam_pull;                /* 1 once rcam_eye is valid */
static double rcam_eye[3];             /* eye actually used (follows the placed eye with ~0.06 s lag) */
static double rcam_rest_h, rcam_rest_dy; /* the game's distance / height while the player stands still */
static double rcam_pp[3], rcam_still;   /* player x/z, target y last frame; seconds standing still */
static double rcam_tprev[3];            /* target last frame (teleport test) */
static double rcam_dn;                  /* debug: placed distance this frame (RUGRATS_RCAMLOG) */
static double rcam_r;                /* smoothed eye distance from the target (0 = take the placed one) */
#define RCAM_PLAYER 0x800E2B48u
static float rcam_sx, rcam_sy;          /* stick this frame */
static double rcam_dt;

/* the manual camera placed this frame's eye and the camera still holds it (portal hook: eye over the wall) */
static int rcam_eye_in_use(uint32_t cam) {
    if (rcam_pull <= 0 || !cam) return 0;
    for (int i = 0; i < 3; i++) if ((int32_t)mem_r32(cam + 20 + 4 * i) != rcam_ours[i]) return 0;
    return 1;
}

static double wrap_pi(double a) {
    const double pi2 = 6.283185307179586;
    while (a > pi2 / 2) a -= pi2;
    while (a < -pi2 / 2) a += pi2;
    return a;
}

/* Levels where the manual (right-stick) camera is off by default because the game's own camera works better there;
   opt.rcam_override (Enhancements "Override Camera") turns it back on everywhere. Level = database number at
   0x800C5678. 21 = Touchdown Tommy (2026-11-08: fixed sideline-style camera; turning it only made things worse).
   Add more levels here. */
static const int rcam_excluded[] = { 21 };
static int rcam_on(void) {
    if (!opt.rcam) return 0;
    if (opt.rcam_override) return 1;
    int db = (int32_t)mem_r32(0x800C5678u);
    for (unsigned i = 0; i < sizeof rcam_excluded / sizeof rcam_excluded[0]; i++)
        if (db == rcam_excluded[i]) return 0;
    return 1;
}

/* Camera mode (cam+0x4a, read by 800291f8): 1 = follow the player, 3 = follow (No More Cookies), 4 = fixed staged
   camera (zero offset) used by aiming minigames: the Incident on Aisle 7 pie gallery and the Circus Angelicus diving
   board. In mode 4 the player's facing / aim follows the camera, so a stick turn kept from earlier play turned Tommy
   ~175 degrees toward the kitchen doors (no targets in view, 2026-11-08, checked against PCSX-Redux). The manual camera
   never runs in mode 4 (Override Camera included) and forgets its turn, so the game's view also returns afterwards. */
static int rcam_fixed(uint32_t cam) { return (int16_t)mem_r16(cam + 0x4a) == 4; }

int rt_hook_8002953c(void) {
    uint32_t cam = cpu.r[4];
    if (rcam_cam && rcam_cam == cam) {
        int same = 1;
        for (int i = 0; i < 3; i++) same &= (int32_t)mem_r32(cam + 0x14 + i * 4) == rcam_ours[i];
        if (same) for (int i = 0; i < 3; i++) mem_w32(cam + 0x14 + i * 4, (uint32_t)rcam_game[i]);
    }
    rcam_cam = 0;
    rcam_sx = rcam_sy = 0;
    rcam_dt = 0;
    if (!rcam_on()) { rcam_active = 0; rcam_pitch = 0; return 0; }
    if (rcam_fixed(cam)) { rcam_active = 0; rcam_pitch = 0; rcam_pull = 0; return 0; }
    uint32_t ticks = mem_r32(cpu.r[28] + 156);
    if (ticks > 40) ticks = 40;
    rcam_dt = ticks / 300.0;
    host_rstick(&rcam_sx, &rcam_sy);
    if (opt.rcam_invert) rcam_sy = -rcam_sy;
    if (rcam_sx != 0 || rcam_sy != 0) rcam_idle = 0;
    else if (rcam_idle < 1000000) rcam_idle += ticks;
    if (rcam_miss < 1000000) rcam_miss += ticks;
    if (rcam_miss > 300) { rcam_active = 0; rcam_pitch = 0; rcam_pull = 0; rcam_rest_h = 0; } /* camera call gone for 1 s: back to the game */
    return 0;
}

/* While the manual camera is in charge (weight rcam_w, 1 = ours):
   - it aims at the player (0x800E2B48, pos at +0x18; checked via player+0x224 == cam) instead of the game's
     target, a pivot object (cam+0x4c = 0x800DE440) that swings up to ~1000 units around the player when the
     player turns quickly (spinning the left stick slid the camera sideways). The pivot's height is kept.
   - distance and height come from the game's eye, smoothed over about 1 s: during fast turns the game's eye
     cuts inside the circle (3200..5400 units instead of ~4000), which zoomed into the back of the head.
   Auto-center eases the weight back to 0 together with the yaw, so nothing snaps on the hand-back. */
static double rcam_h, rcam_dy, rcam_w;  /* smoothed horizontal distance / height of eye over target, weight */

/* Level collision grid: 8002b240(pos*, &ceiling, &floor, &cell) looks up the 1024-unit cell under pos (bounds at
   *(gp+0xbf8), columns at *(gp+0xc14)) and its stacked floor/ceiling spans; returns 0 when pos lies in open space
   (ceiling < y < floor, y down), 1 inside solid (under a floor / in a wall), 2 outside the grid. The game's own camera
   never asks (it stays behind the player); the manual camera does, so it can't swing through tunnel walls (golf
   pyramid maze, 2026-11-07). Called on a scratch area below the guest stack pointer; all registers restored. */
void f_8002b240(void);
static int rcam_grid(const double p[3]) {
    uint32_t save[32], hi = cpu.hi, lo = cpu.lo;
    memcpy(save, cpu.r, sizeof save);
    uint32_t b = (save[29] - 0x100u) & ~7u;
    for (int a = 0; a < 3; a++) mem_w32(b + 0x10 + a * 4, (uint32_t)(int32_t)lround(p[a]));
    cpu.r[29] = b;
    cpu.r[4] = b + 0x10; cpu.r[5] = b + 0x20; cpu.r[6] = b + 0x24; cpu.r[7] = b + 0x28;
    cpu.r[31] = save[31];
    f_8002b240();
    int res = (int32_t)cpu.r[2];
    memcpy(cpu.r, save, sizeof save);
    cpu.hi = hi; cpu.lo = lo;
    return res;
}

/* Level rooms are axis-aligned boxes: table *(gp+0xc0c), count *(gp+0xc1c), 0x70 bytes each, min x/y/z at +0x10,
   max at +0x20. After 8001e838 the game asks 8002b660(eye, cam+0x80 room) which room holds the eye (that room, its
   portal neighbours, then all rooms; -1 = none) and, when none, CLAMPS the eye into the box of room cam+0x80. A
   manual eye behind a wall therefore got pinned to a corner of the last room until the player walked far enough
   for the orbit point to land in a room again. rcam_room() is the same test (hint first, then all rooms). */
static int rcam_room(const double p[3], int hint) {
    uint32_t gp = 0x800C3E80u, tbl = mem_r32(gp + 0xc0c);
    int n = (int32_t)mem_r32(gp + 0xc1c);
    if (n <= 0 || n > 4096) return -1;
    for (int k = -1; k < n; k++) {
        int i = k < 0 ? hint : k;
        if (i < 0 || i >= n) continue;
        uint32_t b = tbl + (uint32_t)i * 0x70 + 0x10;
        int in = 1;
        for (int a = 0; a < 3 && in; a++) {
            double lo = (int32_t)mem_r32(b + a * 4), hi = (int32_t)mem_r32(b + 0x10 + a * 4);
            in = p[a] >= lo && p[a] <= hi;
        }
        if (in) return i;
    }
    return -1;
}
/* Is the eye at azimuth az (elevation el, distance r) clear of solid ground by the collision grid? Tests the line
   from the target to 300 units past the eye, and points 300 units left and right of the eye (the 16:9 view is wide;
   an eye right next to a tunnel wall still shows the outside past the wall's edge). */
static int rcam_az_open(const double t[3], double az, double el, double r) {
    double ch = cos(el), sh = sin(el), dx = ch * cos(az), dy = -sh, dz = ch * sin(az);
    double len = r + 300;
    int n = (int)(len / 150) + 1;
    for (int i = 1; i <= n; i++) {
        double s = len * i / n, p[3] = { t[0] + dx * s, t[1] + dy * s, t[2] + dz * s };
        if (rcam_grid(p) == 1) return 0;
    }
    double lx = -sin(az), lz = cos(az);
    for (int k = -1; k <= 1; k += 2) {
        double p[3] = { t[0] + dx * r + lx * 300 * k, t[1] + dy * r, t[2] + dz * r + lz * 300 * k };
        if (rcam_grid(p) == 1) return 0;
    }
    return 1;
}
/* how far (0..1) along t->e the line stays in open space: inside the room boxes (if the target is in one) and not
   inside solid ground by the collision grid (rcam_grid == 1; outside the grid counts as open). 1 = all the way.
   *last = the room of the last open point (-1 if the target is in no room); *solid = the line was stopped by the grid */
static int rcam_open(const double p[3], int room, int *r2) {
    *r2 = room < 0 ? -1 : rcam_room(p, room);
    if (room >= 0 && *r2 < 0) return 0;
    return rcam_grid(p) != 1;
}
static double rcam_reach(const double t[3], const double e[3], int hint, int *last, int *solid) {
    int room = rcam_room(t, hint), r2;
    *last = room;
    *solid = 0;
    if (rcam_grid(t) == 1) return 1;  /* target itself in solid (shouldn't happen): leave the eye alone */
    /* fixed 100-unit steps: with 24 steps over ~5000 units a thin pillar was hit on some frames and missed on
       others, so the eye jumped in and out for single frames */
    double len = sqrt((e[0] - t[0]) * (e[0] - t[0]) + (e[1] - t[1]) * (e[1] - t[1]) + (e[2] - t[2]) * (e[2] - t[2]));
    int N = (int)(len / 100) + 1;
    if (N < 24) N = 24;
    if (N > 200) N = 200;
    double ok = 0, bad = -1;
    for (int i = 1; i <= N; i++) {
        double s = (double)i / N, p[3];
        for (int a = 0; a < 3; a++) p[a] = t[a] + (e[a] - t[a]) * s;
        if (!rcam_open(p, room, &r2)) {
            int grid = room < 0 || r2 >= 0;
            if (grid) { /* thin solid (shelf edge, box corner; <= ~250 units along the line) with open space past it:
                           ignore it like the game's camera does. Grazing such edges pulled the eye in and out
                           while walking (toy store, 2026-11-07). Thick tunnel walls still stop the line. */
                int j = i + 1, maxrun = (int)(250.0 / (len / N)), r3 = -1, past = 0;
                for (; j <= N && j - i <= maxrun; j++) {
                    double q[3];
                    for (int a = 0; a < 3; a++) q[a] = t[a] + (e[a] - t[a]) * ((double)j / N);
                    if (rcam_open(q, room, &r3)) { past = 1; break; }
                }
                if (past) { i = j; room = r3; ok = (double)j / N; continue; }
            }
            bad = s; *solid = grid; break;
        }
        room = r2;
        ok = s;
    }
    *last = room;
    if (bad < 0) return 1;
    for (int it = 0; it < 6; it++) { /* refine the edge */
        double s = (ok + bad) / 2, p[3];
        for (int a = 0; a < 3; a++) p[a] = t[a] + (e[a] - t[a]) * s;
        if (!rcam_open(p, room, &r2)) bad = s;
        else { ok = s; room = r2; }
    }
    *last = room;
    return ok;
}
/* Where to put the eye: the wanted spot if the line from the target stays open; else slide it along the walls
   (clamp into the box of the room the line was in, like the game's own clamp but with the right room and a margin);
   if that is blocked too, pull it in along the line (never closer than ~700 units when that spot is open; solid
   ground from the grid keeps 250 units of margin, room walls 120). */
static void rcam_place(const double t[3], const double want[3], int hint, double out[3]) {
    int last, solid, l2, s2;
    double f = rcam_reach(t, want, hint, &last, &solid);
    for (int a = 0; a < 3; a++) out[a] = want[a];
    if (f >= 1) return;
    if (last >= 0 && !solid) {
        uint32_t b = mem_r32(0x800C3E80u + 0xc0c) + (uint32_t)last * 0x70 + 0x10;
        double c[3];
        const double m = 120;
        for (int a = 0; a < 3; a++) {
            double lo = (int32_t)mem_r32(b + a * 4) + m, hi = (int32_t)mem_r32(b + 0x10 + a * 4) - m;
            if (lo > hi) lo = hi = (lo + hi) / 2;
            c[a] = want[a] < lo ? lo : want[a] > hi ? hi : want[a];
        }
        double dx = c[0] - t[0], dz = c[2] - t[2];
        if (rcam_reach(t, c, hint, &l2, &s2) >= 1 && dx * dx + dz * dz > 900.0 * 900.0) {
            for (int a = 0; a < 3; a++) out[a] = c[a];
            return;
        }
    }
    double len = sqrt((want[0] - t[0]) * (want[0] - t[0]) + (want[1] - t[1]) * (want[1] - t[1]) +
                      (want[2] - t[2]) * (want[2] - t[2]));
    double g = f - (solid ? 250.0 : 120.0) / len, gmin = 700.0 / len;
    if (g < gmin) {
        double pm[3];
        for (int a = 0; a < 3; a++) pm[a] = t[a] + (want[a] - t[a]) * gmin;
        if (gmin <= 1 && rcam_reach(t, pm, hint, &l2, &s2) >= 1) g = gmin;
        else if (g < 150.0 / len) g = 150.0 / len;
    }
    if (g > 1) g = 1;
    for (int a = 0; a < 3; a++) out[a] = t[a] + (want[a] - t[a]) * g;
}

static int rcam_in_lookat;   /* running the original LookAt from inside its hook */
void f_800211e8(void);
int rt_hook_800211e8(void) {
    if (rcam_in_lookat) return 0;
    if (!rcam_on()) return 0;
    if (cpu.r[31] != 0x8001e928u || mem_r32(cpu.r[29] + 0xd0) != 0x80029df0u) return 0; /* gameplay camera only */
    rcam_miss = 0;
    uint32_t cam = cpu.r[18], pe = cpu.r[4], pt = cpu.r[5];
    if (rcam_fixed(cam)) { rcam_active = 0; rcam_pitch = 0; rcam_pull = 0; return 0; }
    double e[3], t[3];
    for (int i = 0; i < 3; i++) {
        e[i] = (int32_t)mem_r32(pe + i * 4);
        t[i] = (int32_t)mem_r32(pt + i * 4);
    }
    double dx = e[0] - t[0], dy = e[1] - t[1], dz = e[2] - t[2];
    double h = sqrt(dx * dx + dz * dz);
    if (h < 1) return 0;
    double game_az = atan2(dz, dx);
    const double yaw_speed = 3.0, pitch_speed = 1.6; /* radians per second at full tilt */
    {   /* The game's eye only shows its real follow distance once the player has stood still for a moment:
           while walking it lags behind or (in some rooms) barely moves, so its distance grew to ~6000 instead of
           ~4000 when the player walked away from it with the camera turned (toy store, 2026-11-07). Learn the
           distance / height only after 1 s standing still (tracked also while the game is in charge, so turning
           the stick mid-walk starts from the resting distance). */
        double px = (int32_t)mem_r32(RCAM_PLAYER + 0x18), pz = (int32_t)mem_r32(RCAM_PLAYER + 0x20);
        double mx = px - rcam_pp[0], mz = pz - rcam_pp[1], my = t[1] - rcam_pp[2];
        rcam_pp[0] = px; rcam_pp[1] = pz; rcam_pp[2] = t[1];
        if (mx * mx + mz * mz > 4.0 * 4.0 || fabs(my) > 4.0 || rcam_dt <= 0) rcam_still = 0;
        else if (rcam_still < 100) rcam_still += rcam_dt;
        if (rcam_rest_h <= 0) { rcam_rest_h = h; rcam_rest_dy = dy; }
        else if (rcam_still >= 1.0) {
            double k = 1 - exp(-rcam_dt / 0.5);
            rcam_rest_h += (h - rcam_rest_h) * k;
            rcam_rest_dy += (dy - rcam_rest_dy) * k;
        }
    }
    double az_before = rcam_az;   /* azimuth before this frame's stick turn (open last frame) */
    int was_active = rcam_active != 0;
    if (rcam_sx != 0) {
        if (!rcam_active) { rcam_active = 1; rcam_az = game_az; rcam_h = rcam_rest_h; rcam_dy = rcam_rest_dy; rcam_w = 1; }
        rcam_az = wrap_pi(rcam_az - rcam_sx * yaw_speed * rcam_dt); /* stick right = view turns right */
        rcam_w = 1;
    }
    rcam_pitch += rcam_sy * pitch_speed * rcam_dt;
    if (rcam_active) { /* ease toward the resting distance / height (time constant ~1 s) */
        double k = 1 - exp(-rcam_dt / 1.0);
        rcam_h += (rcam_rest_h - rcam_h) * k;
        rcam_dy += (rcam_rest_dy - rcam_dy) * k;
    }
    if (opt.rcam == 2 && rcam_idle > 450) { /* 1.5 s untouched: ease back to the game's camera */
        double k = exp(-rcam_dt * 3.0);
        if (rcam_active) {
            double d = wrap_pi(game_az - rcam_az);
            rcam_az = wrap_pi(rcam_az + d * (1 - k));
            rcam_w *= k;
            if (fabs(d) < 0.01 && rcam_w < 0.01) rcam_active = 0;
        }
        rcam_pitch *= k;
        if (fabs(rcam_pitch) < 0.005) rcam_pitch = 0;
    }
    {   /* debug: RUGRATS_RCAMLOG=1 logs the camera inputs every game frame */
        static int lg = -1;
        if (lg < 0) lg = getenv("RUGRATS_RCAMLOG") != NULL;
        if (lg)
            rt_logf("rcam f%u act %u w %.2f az %.1f game_az %.1f pitch %.2f | game eye %.0f %.0f %.0f tgt %.0f %.0f %.0f "
                    "h %.0f dy %.0f sm %.0f %.0f | player %d %d %d | mode %d bbc %08x c80 %d\n", hooks_frame(), rcam_active, rcam_w,
                    rcam_az * 57.2958, game_az * 57.2958, rcam_pitch, e[0], e[1], e[2], t[0], t[1], t[2], h, dy, rcam_h,
                    rcam_dy, (int32_t)mem_r32(RCAM_PLAYER + 0x18), (int32_t)mem_r32(RCAM_PLAYER + 0x1c),
                    (int32_t)mem_r32(RCAM_PLAYER + 0x20), (int16_t)mem_r16(cam + 0x4a), mem_r32(0x800C3E80u + 0xbbc), (int32_t)mem_r32(cam + 0x80));
    }
    if (!rcam_active && rcam_pitch == 0) { rcam_pull = 0; return 0; }
    double az = game_az, w = 0;
    if (rcam_active) {
        az = rcam_az;
        w = rcam_w;
        if (mem_r32(RCAM_PLAYER + 0x224) == cam) { /* aim at the player, not the swinging pivot */
            double px = (int32_t)mem_r32(RCAM_PLAYER + 0x18), pz = (int32_t)mem_r32(RCAM_PLAYER + 0x20);
            t[0] += (px - t[0]) * w;
            t[2] += (pz - t[2]) * w;
        }
        h += (rcam_h - h) * w;
        dy += (rcam_dy - dy) * w;
    }
    double r = sqrt(h * h + dy * dy);
    double el = atan2(-dy, h);                  /* y is down: eye above the target = positive elevation */
    const double el_min = -0.17, el_max = 1.35; /* about -10 .. 77 degrees */
    double want = el - rcam_pitch;              /* rcam_pitch > 0 = look up = eye lower */
    if (want > el_max) { rcam_pitch += want - el_max; want = el_max; }
    if (want < el_min) { rcam_pitch -= el_min - want; want = el_min; }
    if (!rcam_az_open(t, az, want, r) && fabs(el - want) > 0.01) { /* tilted into a ceiling / floor: tilt back first */
        double w2 = el;
        for (int k = 1; k <= 20; k++) {
            double c = want + (el - want) * k / 20;
            if (rcam_az_open(t, az, c, r)) { w2 = c; break; }
        }
        if (rcam_az_open(t, az, w2, r)) { rcam_pitch = el - w2; want = w2; }
    }
    double nh = r * cos(want);
    if (!rcam_az_open(t, az, want, r)) { /* turned into solid ground (tunnels): nearest open direction, else the game's */
        double sg = wrap_pi(game_az - az) >= 0 ? 1 : -1, found = game_az;
        /* the stick just turned it from an open azimuth: stop at the edge between the two. The 6-degree search below
           snapped the view back one whole step every few frames while the stick was held against a pillar (whole
           picture shaking, toy store blocks, 2026-11-08). */
        double d0 = wrap_pi(az - az_before);
        int edge = rcam_active && was_active && fabs(d0) > 1e-6 && fabs(d0) < 0.5 && rcam_az_open(t, az_before, want, r);
        if (edge) {
            double lo = 0, hi = 1;   /* lo: open fraction of the turn, hi: blocked */
            for (int it = 0; it < 10; it++) {
                double m = (lo + hi) / 2;
                if (rcam_az_open(t, wrap_pi(az_before + d0 * m), want, r)) lo = m; else hi = m;
            }
            found = wrap_pi(az_before + d0 * lo);
        }
        else for (int k = 1; k <= 30; k++) {
            double d = k * (6.0 / 57.2958);
            if (rcam_az_open(t, az + sg * d, want, r)) { found = wrap_pi(az + sg * d); break; }
            if (rcam_az_open(t, az - sg * d, want, r)) { found = wrap_pi(az - sg * d); break; }
        }
        az = found;
        if (rcam_active) rcam_az = found; /* the stick can't wind past the wall */
    }
    double want_e[3] = { t[0] + nh * cos(az), t[1] - r * sin(want), t[2] + nh * sin(az) };
    /* keep the eye inside the level's rooms (slide along walls); moves toward the result quickly but not in one jump
       when the wall handling switches between sliding and pulling in */
    double pe2[3];
    rcam_place(t, want_e, (int32_t)mem_r32(cam + 0x80), pe2);
    double dd = 0, off[3]; /* teleport = the target jumped (a wall pull-in used to count too and popped for one frame) */
    for (int a = 0; a < 3; a++) dd += (t[a] - rcam_tprev[a]) * (t[a] - rcam_tprev[a]);
    for (int a = 0; a < 3; a++) { off[a] = rcam_eye[a] - rcam_tprev[a]; rcam_tprev[a] = t[a]; }
    if (rcam_pull <= 0 || dd > 2000.0 * 2000.0) { /* first frame / teleport */
        for (int a = 0; a < 3; a++) rcam_eye[a] = pe2[a];
        rcam_r = 0;
    }
    else {
        /* distance from the target: in fast (walls), back out slowly. An edge the line clips on and off (climbing
           onto a box) made the distance pump between ~1500 and ~4000 every few frames. Direction is not slowed. */
        double dn = sqrt((pe2[0] - t[0]) * (pe2[0] - t[0]) + (pe2[1] - t[1]) * (pe2[1] - t[1]) +
                         (pe2[2] - t[2]) * (pe2[2] - t[2]));
        if (rcam_r <= 0) rcam_r = dn;
        rcam_r += (dn - rcam_r) * (1 - exp(-rcam_dt / (dn > rcam_r ? 0.5 : 0.04)));
        rcam_dn = dn;
        if (dn > 1) for (int a = 0; a < 3; a++) pe2[a] = t[a] + (pe2[a] - t[a]) * (rcam_r / dn);
        /* the lag is on the offset from the target, not on the world position: a world-space lag let the
           eye trail the (fast) player, so walking toward / away from it changed the distance by ~400 */
        double k = 1 - exp(-rcam_dt / 0.06);
        double no[3], lo = 0, ln = 0, lw = 0;
        for (int a = 0; a < 3; a++) {
            no[a] = off[a] + (pe2[a] - t[a] - off[a]) * k;
            lo += off[a] * off[a]; ln += no[a] * no[a]; lw += (pe2[a] - t[a]) * (pe2[a] - t[a]);
        }
        /* keep the length on a big swing (the wall search flips the azimuth): easing straight across the arc cut
           the corner and dipped the distance by ~400 for a few frames */
        double L = sqrt(lo) + (sqrt(lw) - sqrt(lo)) * k;
        if (ln > 1) for (int a = 0; a < 3; a++) no[a] *= L / sqrt(ln);
        for (int a = 0; a < 3; a++) rcam_eye[a] = t[a] + no[a];
    }
    rcam_pull = 1;
    static int lgd = -1; /* debug: RUGRATS_RCAMLOG also logs the placed eye */
    if (lgd < 0) lgd = getenv("RUGRATS_RCAMLOG") != NULL;
    if (lgd) {
        double ex = rcam_eye[0] - t[0], ey = rcam_eye[1] - t[1], ez = rcam_eye[2] - t[2];
        int l3, s3;
        double f3 = rcam_reach(t, want_e, (int32_t)mem_r32(cam + 0x80), &l3, &s3);
        rt_logf("rcamd f%u dist %.0f h %.0f dy %.0f want %.0f %.0f %.0f placed %.0f %.0f %.0f | t %.0f %.0f %.0f reach %.3f last %d solid %d gridt %d hint %d rest %.0f %.0f still %.2f\n", hooks_frame(),
                sqrt(ex * ex + ey * ey + ez * ez), sqrt(ex * ex + ez * ez), ey, want_e[0] - t[0], want_e[1] - t[1],
                want_e[2] - t[2], pe2[0] - t[0], pe2[1] - t[1], pe2[2] - t[2], t[0], t[1], t[2], f3, l3, s3,
                rcam_grid(t), (int32_t)mem_r32(cam + 0x80), rcam_rest_h, rcam_rest_dy, rcam_still);
        rt_logf("rcame eye %.2f %.2f %.2f r %.2f dn %.2f dt %.5f\n", rcam_eye[0], rcam_eye[1], rcam_eye[2], rcam_r, rcam_dn, rcam_dt);
    }
    int32_t ne[3] = { (int32_t)lround(rcam_eye[0]), (int32_t)lround(rcam_eye[1]), (int32_t)lround(rcam_eye[2]) };
    for (int i = 0; i < 3; i++) {
        rcam_game[i] = (int32_t)mem_r32(cam + 0x14 + i * 4);
        mem_w32(pe + i * 4, (uint32_t)ne[i]);
        mem_w32(pt + i * 4, (uint32_t)(int32_t)lround(t[i]));
        mem_w32(cam + 0x14 + i * 4, (uint32_t)ne[i]);
        rcam_ours[i] = ne[i];
    }
    rcam_cam = cam;
    /* The original LookAt now aims from the moved eye at the (moved) target. It builds the rotation from the whole-unit
       difference target - eye, so while walking (eye ~4300 away, its distance easing by ~0.1 unit a frame) the x and z
       of that difference stepped by one unit on different frames and the view direction zigzagged by ~0.01 degrees
       frame to frame: the whole picture shifted a pixel back and forth and thin edge-on faces (door frame inner face,
       db 5 doorway, 2026-11-12) flickered; also while the eye settled after a load or a stop. The game's own camera
       does not do this. Fix: after the original, rebuild the same matrix (rows as in 800211e8, then MatrixNormal
       8009f398) from the exact direction, so a steady direction gives a steady matrix. Position stays whole units
       (<= 0.5 unit off, invisible). RUGRATS_RCAMEXACT=0 = old. */
    static int exact = -1;
    if (exact < 0) { const char *e = getenv("RUGRATS_RCAMEXACT"); exact = e ? atoi(e) != 0 : 1; }
    if (!exact) return 0;
    uint32_t m = cpu.r[6];
    rcam_in_lookat = 1;
    f_800211e8();                /* the original: keeps all its side effects (GTE registers, stack) */
    rcam_in_lookat = 0;
    double f[3] = { t[0] - rcam_eye[0], t[1] - rcam_eye[1], t[2] - rcam_eye[2] };
    double fl = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1) return 1;
    for (int a = 0; a < 3; a++) f[a] /= fl;
    double r0[3] = { f[2], 0, -f[0] }, r1[3] = { -f[0] * f[1], f[0] * f[0] + f[2] * f[2], -f[1] * f[2] };
    double r2[3] = { r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0] };
    double q0[3] = { r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0] };
    double *rows[3] = { q0, r1, r2 };
    int16_t mv[9], gv[9];
    for (int i = 0; i < 3; i++) {
        double l = sqrt(rows[i][0] * rows[i][0] + rows[i][1] * rows[i][1] + rows[i][2] * rows[i][2]);
        if (l < 1e-9) return 1;
        for (int j = 0; j < 3; j++) mv[i * 3 + j] = (int16_t)lround(rows[i][j] / l * 4096.0);
    }
    for (int k = 0; k < 9; k++) { gv[k] = (int16_t)mem_r16(m + (uint32_t)k * 2); mem_w16(m + (uint32_t)k * 2, (uint16_t)mv[k]); }
    if (lgd) rt_logf("rcamm game %d %d %d %d %d %d %d %d %d ours %d %d %d %d %d %d %d %d %d\n", gv[0], gv[1], gv[2], gv[3],
                     gv[4], gv[5], gv[6], gv[7], gv[8], mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7], mv[8]);
    return 1;
}

void hooks_rcam_state(StateIO *s) {
    st_prefix(s, "rcam");
    ST(s, rcam_az); ST(s, rcam_pitch); ST(s, rcam_game); ST(s, rcam_ours); ST(s, rcam_cam); ST(s, rcam_idle);
    ST(s, rcam_active); ST(s, rcam_miss); ST(s, rcam_h); ST(s, rcam_dy); ST(s, rcam_w);
    ST(s, rcam_pull); ST(s, rcam_eye);
    ST(s, rcam_rest_h); ST(s, rcam_rest_dy); ST(s, rcam_pp); ST(s, rcam_still); ST(s, rcam_tprev); ST(s, rcam_r);
}

/* ---- golf aim speed (ini [game] golf_aim, F1 Game page) ---------------------------------------------------
   In the golf levels (databases 22..31) left/right while aiming adds a fixed nudge to the target heading
   (player+0x2c) EVERY GAME FRAME inside the movement code 8005be24: R16 = +-25 (or +-8 in its fine mode, flag at
   0x800C4984[player+0x1a7] bit 0), capped by stick * ticks / 64 (which is far above 25 with a d-pad), added at
   8005c884. The heading (+0x24) then chases the target. So aiming turned 750 units/s (of 4096 per turn) on the PS1
   (30 game fps), 1500 at 60 fps, 3000 at 120. The wrapper runs the original function, and when the target moved by
   such a nudge (0 < |d| <= 25) rescales it by the real time that passed (ticks of 1/300 s at gp+156; a PS1 frame =
   10 ticks) and the option (percent of the PS1 speed; 0 = leave the game alone). The fraction is carried over. */
void f_8005be24(void);
static double golf_rem;   /* sub-unit remainder carried between frames */

/* Same nudge in the Incident on Aisle 7 pie gallery (database 3 with flag 0x8 in the level word at 0x800C56A8; the
   branch at 8005c648): R16 = +-25 per game frame (8 in fine mode). On the PS1 (PCSX-Redux, 2026-11-08) that scene
   runs at ~17 game fps (frames of 15 and 20 ticks, average ~17), so the reticle turned ~440 units/s; the port turned
   750 at 30 fps and ~3600 at 144. Rescaled to 25 per 17 ticks, never above the game's own step (the game clamps the
   aim range; slow frames on the PS1 also got just one nudge). No option: always the PS1 speed. */
static int gallery_aim(int32_t db) { return db == 3 && (mem_r32(0x800C56A8u) & 8); }

/* Aim lock (player+0x188 bit 0; e.g. Tommy aiming on the shuffleboard in database 2): outside databases 20..31 the
   same code (R22 != 0, branch at 8005c740) turns the target heading +0x2c by
   step = min(R16 / 8 (fine mode / 16), rate x tick >> 6), where R16 = wanted - facing (clamped to +-1536) and rate is
   the stick length (stored at +0x1b0). Both are per GAME FRAME: the 1/8 ease settled 2x faster at 60 fps, 4x at 120,
   5x at 150 (measured on the shuffleboard: 0.5 s at 30 fps vs 0.1 s at 150), and the rate cap (partial stick) uses
   max(ticks, 10) (rt_load_8005c7ec) so it ran fps/30 too fast as well. Fix: the original runs, then its step is
   rescaled by the real time that passed (ticks of 1/300 s) against the PS1's own game frame (ref ticks): an eased
   step d becomes d x k (1 - ((k-1)/k)^(ticks/ref)) (k = 8 or 16), a capped step d x ticks/ref. The fraction is
   carried over. RUGRATS_LOCKAIM=0 = off, RUGRATS_LOCKAIMLOG=1 logs each step. */
static double lockaim_rem;
static int lock_aim_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_LOCKAIM"); on = !(e && *e == '0'); }
    return on;
}
static void lock_aim_fix(uint32_t pl, int32_t before, uint32_t ticks, int32_t db) {
    /* ticks per game frame on the PS1 (PCSX-Redux): shuffleboard (db 2, Grandpa's Teeth) ~14 (frames of 15 and 10 ticks, 30 steps in
       83 vsyncs); elsewhere assume 30 game fps */
    double ref = db == 2 ? 14.0 : 10.0;
    int32_t after = (int16_t)mem_r16(pl + 0x2c);
    int32_t d = ((after - before + 2048) & 0xFFF) - 2048;
    if (d == 0 || d > 640 || d < -640) { lockaim_rem = 0; return; }
    if (ticks > 40) ticks = 40;
    int32_t rate = (int16_t)mem_r16(pl + 0x1b0);
    int32_t cap = (rate * (int32_t)(ticks < 10 ? 10 : ticks)) >> 6;  /* the game's own cap this frame */
    int fine = mem_r32(mem_r32(0x800C4984u + mem_r8(pl + 0x1a7) * 4)) & 1;
    double k = fine ? 16.0 : 8.0, scale;
    if ((d < 0 ? -d : d) >= cap) scale = ticks / ref;
    else scale = k * (1.0 - pow((k - 1.0) / k, ticks / ref));
    double x = d * scale + lockaim_rem;
    int32_t n = (int32_t)x;                 /* toward zero */
    lockaim_rem = x - n;
    int32_t t = (before + n) & 0xFFF;
    mem_w16(pl + 0x2c, (uint32_t)t);
    if (getenv("RUGRATS_LOCKAIMLOG"))
        rt_logf("lockaim: step %d ticks %u cap %d fine %d -> %d (target %d)\n", d, ticks, cap, fine, n, t);
}

int rt_hook_8005be24(void) {
    static int inside;
    if (inside) return 0;
    /* Camera-cut latch: on a camera cut (80027304 -> 80062a08) the game copies the camera matrix to 0x800E31C8 and
       sets player+0x188 bit 0x800; while a direction stays held, movement uses that copy with left/right MIRRORED
       (the cut is assumed to turn the view around). With the right-stick camera in charge the cut never reaches the
       screen, so the mirror reversed left/right until release (also restored from save states taken that way).
       Drop the latch while our camera owns the view. RUGRATS_RCAMLATCH=0 = old behaviour. */
    {
        static int fix = -1;
        if (fix < 0) { const char *e = getenv("RUGRATS_RCAMLATCH"); fix = !(e && *e == '0'); }
        uint32_t f = mem_r32(cpu.r[4] + 0x188);
        if (fix && rcam_active && (f & 0x800)) mem_w32(cpu.r[4] + 0x188, f & ~0x800u);
    }
    int32_t db = (int32_t)mem_r32(0x800C5678u);
    int gallery = gallery_aim(db);
    uint32_t pl = cpu.r[4];
    if (!gallery && (db < 20 || db > 31) && (mem_r32(pl + 0x188) & 1) && lock_aim_on()) {
        int32_t before = (int16_t)mem_r16(pl + 0x2c);
        uint32_t ticks = mem_r32(cpu.r[28] + 156);
        inside = 1;
        f_8005be24();
        inside = 0;
        lock_aim_fix(pl, before, ticks, db);
        return 1;
    }
    if (!gallery && (opt.golf_aim <= 0 || db < 22 || db > 31)) return 0;
    int32_t before = (int16_t)mem_r16(pl + 0x2c);
    uint32_t ticks = mem_r32(cpu.r[28] + 156);
    inside = 1;
    f_8005be24();           /* the original */
    inside = 0;
    int32_t after = (int16_t)mem_r16(pl + 0x2c);
    int32_t d = ((after - before + 2048) & 0xFFF) - 2048;
    if (d == 0 || d > 25 || d < -25) { golf_rem = 0; return 1; }
    if (ticks > 40) ticks = 40;
    double scale = gallery ? (ticks < 17 ? ticks / 17.0 : 1.0) : (ticks / 10.0) * (opt.golf_aim / 100.0);
    double x = d * scale + golf_rem;
    int32_t n = (int32_t)x;                 /* toward zero */
    golf_rem = x - n;
    int32_t t = (before + n) & 0xFFF;       /* the game keeps it in 0..4095 */
    mem_w16(pl + 0x2c, (uint32_t)t);
    if (getenv("RUGRATS_GOLFLOG")) rt_logf("golf: nudge %d ticks %u -> %d (target %d)\n", d, ticks, n, t);
    return 1;
}
void hooks_meter_state(StateIO *s);
static double feather_acc;   /* goose feathers, see rt_hook_8008864c */
static void golfvox_state(StateIO *s);
static void circus_state(StateIO *s);
void hooks_golf_state(StateIO *s) {
    st_prefix(s, "golf");
    ST(s, golf_rem); ST(s, lockaim_rem); ST(s, feather_acc);
    hooks_meter_state(s);
    golfvox_state(s);
    circus_state(s);
}

/* ---- power meter speed (Triangle meter, e.g. Angelica's diving board) ------------------------------------
   80058a78 moves the meter (gp+0xd52, 0..220; second phase gp+0xd4e) by a fixed +-6 EVERY GAME FRAME. The PS1
   runs it at 30 game fps (checked in PCSX-Redux: full in ~73 vblanks), so at 120 fps it ran 4x fast. The three step
   instructions (STEP_HOOKS in recomp.py) now get 6 x (ticks / 10) per frame (ticks of 1/300 s at gp+156; a PS1
   frame = 10 ticks), with the fraction carried over, so the meter crosses at the PS1 speed at any frame rate. The
   game's own clamp at 220 / reset at 0 logic is unchanged. A step is at least 1 so the meter never stalls. */
static double meter_rem[2];   /* [0] = main meter (gp+0xd52), [1] = second phase (gp+0xd4e) */
static int32_t meter_step(int which, int32_t imm) {
    if (!opt.fixed_timestep) return imm;
    uint32_t ticks = mem_r32(cpu.r[28] + 156);
    if (ticks == 10 || ticks == 0) { meter_rem[which] = 0; return imm; }
    if (ticks > 40) ticks = 40;
    double sgn = imm < 0 ? -1 : 1;
    double x = fabs((double)imm) * ticks / 10.0 + meter_rem[which];
    int32_t n = (int32_t)x;
    if (n < 1) n = 1;
    meter_rem[which] = x - n;
    if (meter_rem[which] < -1) meter_rem[which] = 0;
    return (int32_t)(sgn * n);
}
int32_t rt_step_80058b34(int32_t imm) { return meter_step(0, imm); }
int32_t rt_step_80058b64(int32_t imm) { return meter_step(0, imm); }
int32_t rt_step_80058ba8(int32_t imm) { return meter_step(1, imm); }

/* ---- player turn step (turning around toward the camera at high fps) -------------------------------------
   In the player movement 8005be24, R20 = facing obj+0x24, R18 = wanted heading (stick through the camera matrix,
   atan2), R16 = wanted - facing (wrapped, clamped to +-1536). Turn step = rate (R23) x ticks >> 6 (the tick read at
   8005c7ec), clamped to |R16|. If what is left after that step is under gp+0x44c, the target obj+0x2c snaps to the
   wanted heading; otherwise obj+0x2c += step. That += builds on the target's own last value, not on the facing,
   so when it does not snap it keeps going round (-320 per frame at 60 fps) and the facing (which eases toward
   obj+0x2c) wobbles: Spike stood in idle shaking for ~0.75 s, turned slowly, then walked. On the PS1 (10 ticks per
   frame) a full-stick turn always snaps in its first frame (step 640 vs a 1536 clamp), so this never shows.
   Seen turning Spike around toward the camera (sewer save), with both the game camera and the right-stick camera
   (the camera is not involved: its yaw stayed constant). Fix: that one tick read sees max(ticks, 10), so the snap
   happens as on the PS1; the facing still turns at the real-time rate (its own code). RUGRATS_TURNFIX=0 = original. */
uint32_t rt_load_8005c7ec(uint32_t v) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_TURNFIX"); on = e ? atoi(e) != 0 : 1; }
    return on && v < 10 ? 10 : v;
}
void hooks_meter_state(StateIO *s) {
    st_prefix(s, "meter");
    ST(s, meter_rem);
}

/* ---- menu/HUD models in widescreen ------------------------------------------------------------------------
   8003f934(obj) draws a model with the menu camera (0x800E2580): the button icons next to "CANCEL" / "ACCEPT" on
   the title screen, "PRESS (X) TO CONTINUE", menu decorations. The text beside them is 2D and gets stretched to
   16:9 with the whole picture, but the GTE squeezes 3D x by 3/4 toward the centre, so the icons slid onto the
   words. Here the object's x position (obj+24) is widened by 4/3 for the draw: its centre lands where the 4:3
   layout put it, while its shape stays squeezed (not stretched). Restored right after. RUGRATS_HUDWIDE=0 = off. */
void f_8003f934(void);
int64_t ws_hud_num(void), ws_hud_den(void);
int rt_hook_8003f934(void) {
    static int inside, env = -1;
    if (env < 0) { const char *e = getenv("RUGRATS_HUDWIDE"); env = e ? atoi(e) != 0 : 1; }
    if (inside || !opt.widescreen || !env) return 0;
    uint32_t obj = cpu.r[4];
    if (!obj) return 0;
    static int hudlog = -1;
    if (hudlog < 0) hudlog = getenv("RUGRATS_HUDLOG") != NULL;
    if (hudlog) { unsigned hooks_frame(void);
        rt_logf("hud vbl %u obj %08x ra %08x pos %d,%d,%d\n", hooks_frame(), obj, cpu.r[31], (int32_t)mem_r32(obj + 24),
                (int32_t)mem_r32(obj + 28), (int32_t)mem_r32(obj + 32)); }
    int gpu_sq_mode(void);
    if (gpu_sq_mode()) {   /* Stretch 2D Off: the HUD layer is squeezed on the GPU, so draw the icon as on a 4:3 screen */
        inside = 1;
        opt.widescreen = 0;
        f_8003f934();
        opt.widescreen = 1;
        inside = 0;
        return 1;
    }
    int32_t x = (int32_t)mem_r32(obj + 24);
    mem_w32(obj + 24, (uint32_t)(int32_t)((int64_t)x * ws_hud_num() / ws_hud_den()));
    inside = 1;
    f_8003f934();
    inside = 0;
    mem_w32(obj + 24, (uint32_t)x);
    return 1;
}

/* ---- HUD quads projected through the GTE ------------------------------------------------------------------
   8008f5d8 draws a HUD quad (e.g. the frame of the golf swing meter) by building a rotation/translation matrix and
   projecting its 4 corners with 8002407c (the general GTE point projector, also used by 800243fc for the world).
   The widescreen squeeze (x*3/4) made that frame 3/4 as wide, while the purple bar inside it is plain 2D and keeps
   its 4:3 width (stretched with the whole picture), so the bar ran past the frame's right end. When called from the
   HUD routine, the projection runs without the squeeze, so the frame lines up with the bar like on a 4:3 screen.
   RUGRATS_HUDGTE=0 = old behaviour. */
void f_8002407c(void);
int rt_hook_8002407c(void) {
    static int inside, env = -1;
    if (env < 0) { const char *e = getenv("RUGRATS_HUDGTE"); env = e ? atoi(e) != 0 : 1; }
    if (inside || !env || !opt.widescreen || cpu.r[31] != 0x8008f720u) return 0;
    inside = 1;
    opt.widescreen = 0;
    f_8002407c();
    opt.widescreen = 1;
    inside = 0;
    return 1;
}

/* ---- hub level-name label (e.g. "Easy / Egg Hunt" over a level entrance) ---------------------------------
   The label is drawn by a timer callback (80049eec), not by the per-frame draw. The timer list 80072bf0 counts each
   timer down by game seconds and fires it every 1/30 s (period 136 in 4.12). On a PS1 that is every game frame; at
   120 fps it fires on 1 frame in 4, so the text flickered. After the timer pass we call the callback again for the
   label timers that did not fire this frame, with its sparkle spawn (80050fd0, at the end) skipped, so only the
   text is drawn. The callback decides by itself whether the text shows (player in the entrance, right level). */
void f_80072bf0(void);
void f_80049eec(void);
static int lbl_inside, lbl_redraw, lbl_nfired;
static uint32_t lbl_fired[16];
/* ---- TV to Final Level (part of opt.final_level) ---------------------------------------------------------------
   Once all 12 puzzle pieces are placed (80094b8c() == 12), the TV in the hub (model 135, found in the proximity list
   0x800C4C24 by its own enter callback 8004d0f0) becomes a level entrance like the game's own: obj+84 = 8004f78c
   (Triangle -> "leave for db obj+108"), obj+108 = 7 (Reptar 2010), obj+68 = 80049eec (label + sparkles timer callback).
   The TV keeps its own proximity callbacks (hint 203), so its label timer (obj+156) is started / stopped here: within
   2500 units (the game's entrance range), same room, and only while no other entrance label is running (the Egg Hunt
   basket is ~2490 away and labels draw at the same screen spot). The label's difficulty line says "Final" in red: for
   the TV's calls only, the level-7 entry of the difficulty jump table (0x80013790, normally the green "Easy" case) is
   pointed at the red "Hard" case 80049fa8 and the "Hard" string (0x800C4758, 8-byte slot) reads "Final"; both are put
   back right after. Other entrances are never touched. Runs once per game frame from the timer-pass hook. */
void f_80094b8c(void);
void f_80072b48(void);
void f_80072bac(void);
#define TV_MODEL 135u
#define TV_RANGE 2500
static uint32_t tv_obj;     /* the TV while it is an entrance this frame, else 0 */
static uint32_t tv_find(void) {
    int g = 0;
    for (uint32_t n = mem_r32(0x800C4C24u); n && g < 512; n = mem_r32(n + 4), g++) {
        uint32_t o = mem_r32(n + 8);
        if (o && mem_r16(o + 150) == TV_MODEL && mem_r32(o + 72) == 0x8004d0f0u) return o;
    }
    return 0;
}
static int tv_other_label(uint32_t tv) {   /* another entrance's label timer is running */
    int g = 0;
    for (uint32_t t = mem_r32(0x800C43B8u); t && g < 256; t = mem_r32(t), g++) {
        uint32_t obj = mem_r32(t + 16);
        if (obj && obj != tv && mem_r32(obj + 0x44) == 0x80049eecu) return 1;
    }
    return 0;
}
static void tv_frame(void) {
    tv_obj = 0;
    if (mem_r32(0x800C5678u) != 1) return;                    /* hub (Pickles' Home) only */
    uint32_t o = tv_find();
    if (!o) return;
    uint32_t save[32];
    memcpy(save, cpu.r, sizeof save);
    int on = 0;
    static int force = -1;                                     /* RUGRATS_TVFORCE=1: unlocked without the pieces (debug) */
    if (force < 0) force = getenv("RUGRATS_TVFORCE") != 0;
    if (opt.final_level) { cpu.r[31] = 0x80072bf0u; f_80094b8c(); on = cpu.r[2] == 12 || force; }
    uint32_t tmr = mem_r32(o + 156);
    int show = 0;
    if (on) {
        mem_w32(o + 84, 0x8004f78cu);
        mem_w8(o + 108, 7);
        mem_w8(o + 106, 0);
        mem_w32(o + 68, 0x80049eecu);
        tv_obj = o;
        const uint32_t P = 0x800E2B48u;
        double dx = (double)(int32_t)mem_r32(P + 24) - (int32_t)mem_r32(o + 24);
        double dz = (double)(int32_t)mem_r32(P + 32) - (int32_t)mem_r32(o + 32);
        show = dx * dx + dz * dz < (double)TV_RANGE * TV_RANGE && mem_r8(P + 59) == mem_r8(o + 59) && !tv_other_label(o);
    } else if (mem_r32(o + 84) == 0x8004f78cu) {               /* turned off / pieces gone: plain TV again */
        mem_w32(o + 84, 0);
        mem_w8(o + 108, 0);
    }
    if (show && !tmr) {
        cpu.r[4] = o; cpu.r[5] = 136; cpu.r[31] = 0x80072bf0u;
        f_80072b48();
        mem_w32(o + 156, cpu.r[2]);
    } else if (!show && tmr && mem_r32(o + 68) == 0x80049eecu) {
        cpu.r[4] = tmr; cpu.r[31] = 0x80072bf0u;
        f_80072bac();
        mem_w32(o + 156, 0);
    }
    memcpy(cpu.r, save, sizeof save);
}
int rt_hook_80049eec(void) {
    static int inside;
    if (inside) return 0;
    if (!lbl_redraw && lbl_nfired < 16) lbl_fired[lbl_nfired++] = cpu.r[4];
    uint32_t o = cpu.r[4];
    if (!o || o != tv_obj || mem_r8(o + 108) != 7) return 0;
    uint32_t jt = mem_r32(0x80013790u), s0 = mem_r32(0x800C4758u), s1 = mem_r32(0x800C475Cu);
    mem_w32(0x80013790u, 0x80049fa8u);                          /* level 7 -> the red "Hard" case */
    mem_w32(0x800C4758u, 0x616e6946u);                          /* "Fina" */
    mem_w32(0x800C475Cu, 0x0000006cu);                          /* "l\0\0\0" */
    inside = 1;
    f_80049eec();
    inside = 0;
    mem_w32(0x80013790u, jt); mem_w32(0x800C4758u, s0); mem_w32(0x800C475Cu, s1);
    return 1;
}
int rt_hook_80050fd0(void) {
    if (lbl_redraw) { cpu.r[2] = 0; return 1; }
    return 0;
}
/* ---- timer callbacks get the time since they last fired --------------------------------------------------
   The timer pass 80072bf0 (list head gp+0x538; node +0 next, +8 period, +0xc countdown, both 4.12 s; +0x10 obj whose
   +0x44 is the callback) subtracts this frame's game seconds (gp+160) from each countdown and, when it runs out,
   calls the callback once (obj in a0; a negative return removes the timer via 80072af4 / 80072ac0). Callbacks that
   move things use this frame's ticks (gp+156) / seconds, which is right on a PS1 (the 1/30 s timers fire every game
   frame) but at 120 fps they fire on 1 frame in 4 and moved by a quarter: cookie bonus level, Angelica at ~1/4 of
   her speed (2026-11-07). This native copy of the pass is the original plus: each timer collects the ticks and
   seconds of the frames since it last fired, and its callback sees those totals in gp+156/160 (frame values put
   back afterwards). With one firing per frame (30 fps and below) that is exactly the frame's values, as before.
   RUGRATS_TIMERSTEP=0 = callbacks see the frame's values (old behaviour). */
void f_80072af4(void);
void f_80072ac0(void);
#define TM_MAX 128
static uint32_t tm_node[TM_MAX], tm_ticks[TM_MAX], tm_secs[TM_MAX], tm_n;

/* ---- smooth platform movers between their 1/30 s steps (2026-11-13, space station elevator) ------------------
   Timer callback 8004ee00(obj) is the generic mover (elevators, moving platforms): per axis i (x, y, z) it adds a FIXED
   step obj+184+2i (sign: obj+190 bit i) to the travel obj+168+2i and the position obj+24+4i while |travel| < limit
   obj+176+2i (moving out) or travel != 0 (moving back), flips the bit at the end, then 8004a34c(obj) re-files the
   object into the room it now sits in. It does not use ticks, so at 120 fps it moves 60 units on 1 frame in 4 and
   stands still on the other 3: Angelica's elevator jerked while the camera glided. The fire frames stay exactly the
   game's: before the callback runs, the partial offset below is taken back off the position. In between, the object
   is placed at base + delta * (time since the fire / period), where delta is what the last fire moved on that axis,
   only while the next fire would move it again (same travel test). Rides follow since the player stands on the
   object's position. RUGRATS_MOVERSMOOTH=0 = off (30 Hz steps, as before). */
#define MV_MAX 16
static uint32_t mv_obj[MV_MAX], mv_n;
static int32_t mv_base[MV_MAX][3], mv_delta[MV_MAX][3], mv_part[MV_MAX][3];
void f_8004a34c(void);
static int mv_find(uint32_t obj) {
    for (uint32_t i = 0; i < mv_n; i++) if (mv_obj[i] == obj) return (int)i;
    return -1;
}
static void mv_drop(int k) { mv_n--; if ((uint32_t)k != mv_n) { mv_obj[k] = mv_obj[mv_n];
    memcpy(mv_base[k], mv_base[mv_n], 12); memcpy(mv_delta[k], mv_delta[mv_n], 12); memcpy(mv_part[k], mv_part[mv_n], 12); } }
static void mv_place(int k, const int32_t part[3]) {   /* position = base + part, then re-file the room */
    const uint32_t obj = mv_obj[k];
    int ch = 0;
    for (int i = 0; i < 3; i++) {
        if (part[i] != mv_part[k][i]) ch = 1;
        mem_w32(obj + 24 + 4 * i, (uint32_t)(mv_base[k][i] + part[i]));
        mv_part[k][i] = part[i];
    }
    if (ch) { cpu.r[4] = obj; cpu.r[31] = 0x80072c64u; f_8004a34c(); }
}
static int mv_will_move(uint32_t obj, int i) {         /* the next 8004ee00 call moves axis i (its travel test) */
    const int16_t lim = (int16_t)mem_r16(obj + 176 + 2 * i), tr = (int16_t)mem_r16(obj + 168 + 2 * i);
    if (!lim) return 0;
    if (mem_r16(obj + 190) & (1u << i)) { int a = tr < 0 ? -tr : tr, l = lim < 0 ? -lim : lim; return a < l; }
    return tr != 0;
}
/* ---- smooth timer-driven characters between their 1/30 s updates (2026-11-14, Chuckie following Tommy) --------
   Companions (Chuckie: callback 80084084) run their whole update, movement and animation, inside a 1/30 s timer
   callback, which calls the character update 8005a068 -> anim update 80056834(obj). At 144 fps that fires on 1 frame
   in 4-5, so the pose and the position stepped at 30 Hz with uneven 4/5 spacing while the player moved every frame.
   A timer object whose callback called 80056834 on itself is tracked here (cs_*). Only what is DRAWN changes; the game
   never sees the in-between values:
   - Pose: on frames between fires, the timer pass copies the anim channels (obj+0xa4..0xe8) and the pose block
     (obj+0x600..0xc00; bones ~0x6d8..0x914), runs 80056834 with the ticks since the last fire (the pose the next fire
     will have reached at this moment), keeps the result in cs_vis and writes the game's bytes straight back. The RAM
     diff of an extra 80056834 call touched only those ranges.
   - Position: interpolated along the real path, one step behind: base = position after the last fire, delta = what
     that fire moved; drawn at base - delta * (1 - ticks since the fire / that fire's ticks). (Extrapolating overshot:
     Chuckie's path zig-zags in z from one 30 Hz step to the next.)
   - Heading: the yaw obj+0x24 (s16, 4096 = 360; what the draw reads) is interpolated the same way (turns over 90
     degrees in one step stay snaps). Chuckie swings ~+-30 degrees around his target heading on every step.
   - The object draws 80031b84 / 8003205c swap cs_vis + the offset in around the call, only on the frame the timer
     pass computed them, and swap back after (byte by byte, only where memory still holds the swapped-in value).
   RUGRATS_AISMOOTH=0 = off. */
#define CS_MAX 8
#define CS_A0 0xa4
#define CS_A1 0x100
#define CS_B0 0x600
#define CS_B1 0xc00
#define CS_LEN ((CS_A1 - CS_A0) + (CS_B1 - CS_B0))
static uint32_t cs_obj[CS_MAX], cs_cb[CS_MAX], cs_n;
static uint8_t cs_true[CS_MAX][CS_LEN], cs_vis[CS_MAX][CS_LEN], cs_pose[CS_MAX];
static int32_t cs_delta[CS_MAX][3], cs_part[CS_MAX][3], cs_fticks[CS_MAX];
static int32_t cs_dyaw[CS_MAX], cs_vyaw[CS_MAX];   /* heading obj+0x24 (4096 = 360): last fire's turn, drawn yaw */
static uint32_t cs_stamp[CS_MAX];               /* present_count + 1 of the timer pass that computed cs_vis / cs_part */
static uint16_t cs_yaw_save, cs_yaw_vis; static int cs_yaw_on;
static uint32_t cs_watch, cs_seen, cs_inside;   /* set by rt_hook_80056834 while a timer callback runs */
void f_80056834(void);
static uint32_t cs_addr(uint32_t obj, int j) {
    return obj + (j < CS_A1 - CS_A0 ? (uint32_t)(CS_A0 + j) : (uint32_t)(CS_B0 + j - (CS_A1 - CS_A0)));
}
static int cs_find(uint32_t obj) {
    for (uint32_t i = 0; i < cs_n; i++) if (cs_obj[i] == obj) return (int)i;
    return -1;
}
static void cs_drop(int k) {
    cs_n--;
    if ((uint32_t)k == cs_n) return;
    cs_obj[k] = cs_obj[cs_n]; cs_cb[k] = cs_cb[cs_n]; cs_pose[k] = cs_pose[cs_n]; cs_fticks[k] = cs_fticks[cs_n];
    cs_dyaw[k] = cs_dyaw[cs_n]; cs_vyaw[k] = cs_vyaw[cs_n];
    cs_stamp[k] = cs_stamp[cs_n];
    memcpy(cs_true[k], cs_true[cs_n], CS_LEN); memcpy(cs_vis[k], cs_vis[cs_n], CS_LEN);
    memcpy(cs_delta[k], cs_delta[cs_n], 12); memcpy(cs_part[k], cs_part[cs_n], 12);
}
/* timer pass, every frame for a tracked object: at = ticks since its last fire (0 on the fire frame) */
static void cs_compute(int k, uint32_t at, int pose) {
    const uint32_t obj = cs_obj[k];
    cs_pose[k] = 0;
    if (pose) {
        const uint32_t gp = 0x800C3E80u, t = mem_r32(gp + 156);
        for (int j = 0; j < CS_LEN; j++) cs_true[k][j] = mem_r8(cs_addr(obj, j));
        mem_w32(gp + 156, at);
        cs_inside = 1;
        cpu.r[4] = obj; cpu.r[31] = 0x80072c64u;
        f_80056834();
        cs_inside = 0;
        mem_w32(gp + 156, t);
        for (int j = 0; j < CS_LEN; j++) {
            const uint32_t a = cs_addr(obj, j);
            cs_vis[k][j] = mem_r8(a);
            mem_w8(a, cs_true[k][j]);
        }
        cs_pose[k] = 1;
    }
    const int32_t ft = cs_fticks[k];
    int32_t f = ft > 0 ? (int32_t)at : 0;
    if (f > ft) f = ft;
    for (int i = 0; i < 3; i++)
        cs_part[k][i] = ft > 0 ? (int32_t)((int64_t)cs_delta[k][i] * (f - ft) / ft) : 0;
    cs_vyaw[k] = (int16_t)mem_r16(obj + 0x24) + (ft > 0 ? cs_dyaw[k] * (f - ft) / ft : 0);
    cs_stamp[k] = present_count + 1;
}
static int cs_draw_on(uint32_t obj) {
    const int k = cs_find(obj);
    if (k < 0 || cs_stamp[k] != present_count + 1) return -1;
    static int lg = -1;
    if (lg < 0) lg = getenv("RUGRATS_AISMOOTHLOG") != NULL;
    if (lg) rt_logf("aismooth f%u obj %08x ra %08x pose %d part %d %d %d\n", present_count, obj, cpu.r[31], cs_pose[k],
                    cs_part[k][0], cs_part[k][1], cs_part[k][2]);
    if (cs_pose[k])
        for (int j = 0; j < CS_LEN; j++) {
            const uint32_t a = cs_addr(obj, j);
            if (mem_r8(a) == cs_true[k][j]) mem_w8(a, cs_vis[k][j]);
        }
    for (int i = 0; i < 3; i++) if (cs_part[k][i]) mem_w32(obj + 24 + 4 * i, mem_r32(obj + 24 + 4 * i) + (uint32_t)cs_part[k][i]);
    /* heading: the draw builds the rotation from the yaw at obj+0x24 (s16, 4096 = 360; the matrix at obj+4 is not
       what it reads: swapping that alone changed nothing) */
    cs_yaw_on = 0;
    if (cs_vyaw[k] != (int16_t)mem_r16(obj + 0x24)) {
        cs_yaw_save = mem_r16(obj + 0x24); cs_yaw_vis = (uint16_t)cs_vyaw[k];
        mem_w16(obj + 0x24, cs_yaw_vis); cs_yaw_on = 1;
    }
    return k;
}
static void cs_draw_off(int k) {
    const uint32_t obj = cs_obj[k];
    if (cs_yaw_on && mem_r16(obj + 0x24) == cs_yaw_vis) mem_w16(obj + 0x24, cs_yaw_save);
    cs_yaw_on = 0;
    if (cs_pose[k])
        for (int j = 0; j < CS_LEN; j++) {
            const uint32_t a = cs_addr(obj, j);
            if (mem_r8(a) == cs_vis[k][j]) mem_w8(a, cs_true[k][j]);
        }
    for (int i = 0; i < 3; i++) if (cs_part[k][i]) mem_w32(obj + 24 + 4 * i, mem_r32(obj + 24 + 4 * i) - (uint32_t)cs_part[k][i]);
}
int rt_hook_80056834(void) {
    if (cs_watch && !cs_inside && cpu.r[4] == cs_watch) cs_seen = 1;
    return 0;
}

static void timer_pass(void) {
    static int on = -1, mvs = -1, css = -1;
    if (css < 0) { const char *e = getenv("RUGRATS_AISMOOTH"); css = e ? atoi(e) != 0 : 1; }
    if (on < 0) { const char *e = getenv("RUGRATS_TIMERSTEP"); on = e ? atoi(e) != 0 : 1; }
    if (mvs < 0) { const char *e = getenv("RUGRATS_MOVERSMOOTH"); mvs = e ? atoi(e) != 0 : 1; }
    const uint32_t gp = 0x800C3E80u;
    uint32_t ticks = mem_r32(gp + 156), secs = mem_r32(gp + 160);
    uint32_t keep_n = 0, keep_node[TM_MAX], keep_t[TM_MAX], keep_s[TM_MAX];
    uint32_t save[32];
    memcpy(save, cpu.r, sizeof save);
    int guard = 0;
    for (uint32_t node = mem_r32(gp + 0x538); node && guard < 4096; guard++) {
        uint32_t next = mem_r32(node);
        int32_t cd = (int32_t)(mem_r32(node + 0xc) - secs);
        mem_w32(node + 0xc, (uint32_t)cd);
        uint32_t at = ticks, as = secs;      /* time since this timer last fired, this frame included */
        for (uint32_t i = 0; i < tm_n; i++)
            if (tm_node[i] == node) { at += tm_ticks[i]; as += tm_secs[i]; break; }
        if (cd > 0) {
            if (keep_n < TM_MAX) { keep_node[keep_n] = node; keep_t[keep_n] = at; keep_s[keep_n] = as; keep_n++; }
            const uint32_t o = mem_r32(node + 0x10);
            if (css && o) { const int c = cs_find(o); if (c >= 0) cs_compute(c, at, 1); }
            const int k = (mvs && o && mem_r32(o + 0x44) == 0x8004ee00u) ? mv_find(o) : -1;
            if (k >= 0) {   /* between fires: the fraction of the coming step */
                const int32_t per = (int32_t)mem_r32(node + 8);
                int32_t part[3];
                for (int i = 0; i < 3; i++) {
                    int64_t p = per > 0 ? (int64_t)mv_delta[k][i] * (int64_t)as / per : 0;
                    if (p > 0 && p > mv_delta[k][i]) p = mv_delta[k][i];
                    if (p < 0 && p < mv_delta[k][i]) p = mv_delta[k][i];
                    part[i] = (int32_t)p;
                }
                mv_place(k, part);
            }
            node = next;
            continue;
        }
        int32_t period = (int32_t)mem_r32(node + 8);
        int g2 = 0;
        do { cd += period; mem_w32(node + 0xc, (uint32_t)cd); } while (cd <= 0 && ++g2 < 100000);
        uint32_t obj = mem_r32(node + 0x10), cb = obj ? mem_r32(obj + 0x44) : 0;
        const int mover = mvs && cb == 0x8004ee00u;
        int16_t tr0[3] = {0, 0, 0};
        if (mover) {    /* fire frame: take the partial offset back off, the game's step then runs unchanged */
            int k = mv_find(obj);
            if (k >= 0) {
                const int32_t z[3] = {0, 0, 0};
                for (int i = 0; i < 3; i++)   /* the game may have moved it itself meanwhile: keep that */
                    mv_base[k][i] = (int32_t)mem_r32(obj + 24 + 4 * i) - mv_part[k][i];
                mv_place(k, z);
            }
            for (int i = 0; i < 3; i++) tr0[i] = (int16_t)mem_r16(obj + 168 + 2 * i);
        }
        if (cb) {
            static int tlog = -1;
            if (tlog < 0) tlog = getenv("RUGRATS_TIMERLOG") != NULL;
            if (tlog) rt_logf("timer f%u node %08x obj %08x cb %08x period %d ticks %u\n", present_count, node, obj, cb,
                              period, at);
            int c = css ? cs_find(obj) : -1;
            int32_t p0[3];
            const int16_t yaw0 = obj ? (int16_t)mem_r16(obj + 0x24) : 0;
            for (int i = 0; i < 3; i++) p0[i] = (int32_t)mem_r32(obj + 24 + 4 * i);
            cs_watch = css ? obj : 0; cs_seen = 0;
            if (on) { mem_w32(gp + 156, at); mem_w32(gp + 160, as); }
            cpu.r[4] = obj;
            cpu.r[31] = 0x80072c64u;
            rt_dispatch(cb);
            mem_w32(gp + 156, ticks); mem_w32(gp + 160, secs);
            cs_watch = 0;
            if (css) {
                if (cs_seen && (int32_t)cpu.r[2] >= 0) {
                    if (c < 0 && cs_n < CS_MAX) { c = (int)cs_n++; cs_obj[c] = obj; }
                    if (c >= 0) {
                        cs_cb[c] = cb; cs_fticks[c] = (int32_t)at;
                        for (int i = 0; i < 3; i++) {
                            int32_t d = (int32_t)mem_r32(obj + 24 + 4 * i) - p0[i];
                            cs_delta[c][i] = (d > 1024 || d < -1024) ? 0 : d;   /* teleports are not motion */
                        }
                        int32_t dy = ((int16_t)mem_r16(obj + 0x24) - yaw0) & 4095;
                        if (dy >= 2048) dy -= 4096;
                        cs_dyaw[c] = (dy > 1024 || dy < -1024) ? 0 : dy;   /* snap turns stay snaps */
                        cs_compute(c, 0, 0);   /* fire frame: drawn at the previous fire's position */
                    }
                } else if (c >= 0) cs_drop(c);
            }
            if (mover) {
                int k = mv_find(obj), any = 0;
                int32_t d[3];
                for (int i = 0; i < 3; i++) {
                    d[i] = (int16_t)((int16_t)mem_r16(obj + 168 + 2 * i) - tr0[i]);
                    if (d[i] && !mv_will_move(obj, i)) d[i] = 0;
                    any |= d[i] != 0;
                }
                if ((int32_t)cpu.r[2] < 0 || !any) { if (k >= 0) mv_drop(k); }
                else {
                    if (k < 0 && mv_n < MV_MAX) { k = (int)mv_n++; mv_obj[k] = obj; }
                    if (k >= 0) {
                        for (int i = 0; i < 3; i++) {
                            mv_base[k][i] = (int32_t)mem_r32(obj + 24 + 4 * i); mv_delta[k][i] = d[i]; mv_part[k][i] = 0;
                        }
                    }
                }
            }
            if ((int32_t)cpu.r[2] < 0) {
                cpu.r[4] = node; cpu.r[31] = 0x80072c74u; f_80072af4();
                cpu.r[4] = node; cpu.r[31] = 0x80072c7cu; f_80072ac0();
            }
        }
        node = next;
    }
    tm_n = keep_n;
    memcpy(tm_node, keep_node, keep_n * 4); memcpy(tm_ticks, keep_t, keep_n * 4); memcpy(tm_secs, keep_s, keep_n * 4);
    for (uint32_t k = 0; k < mv_n;) {   /* forget movers whose timer is gone (level change, removed elsewhere) */
        int alive = 0, g = 0;
        for (uint32_t nd = mem_r32(gp + 0x538); nd && g < 4096; g++, nd = mem_r32(nd))
            if (mem_r32(nd + 0x10) == mv_obj[k] && mem_r32(mv_obj[k] + 0x44) == 0x8004ee00u) { alive = 1; break; }
        if (alive) k++; else mv_drop((int)k);
    }
    for (uint32_t k = 0; k < cs_n;) {   /* forget characters whose timer is gone */
        int alive = 0, g = 0;
        for (uint32_t nd = mem_r32(gp + 0x538); nd && g < 4096; g++, nd = mem_r32(nd))
            if (mem_r32(nd + 0x10) == cs_obj[k] && mem_r32(cs_obj[k] + 0x44) == cs_cb[k]) { alive = 1; break; }
        if (alive) k++; else cs_drop((int)k);
    }
    memcpy(cpu.r, save, sizeof save);
}
void hooks_timer_state(StateIO *s) {
    st_prefix(s, "timer");
    ST(s, tm_node); ST(s, tm_ticks); ST(s, tm_secs); ST(s, tm_n);
    ST(s, mv_obj); ST(s, mv_n); ST(s, mv_base); ST(s, mv_delta); ST(s, mv_part);
    ST(s, cs_obj); ST(s, cs_n); ST(s, cs_cb); ST(s, cs_delta); ST(s, cs_fticks); ST(s, cs_dyaw);   /* cs_vis is per frame: not saved */
}

/* Arrow over a character's head (cookie bonus level and others): 80031a28(obj) picks its colour by the game's frame
   counter (0x800C3F24, +1 per game frame): counter % 10 < 5 -> bright (0xd0) else normal (0x80), i.e. 5 frames each.
   PS1 (30 fps): 3 blinks per second; 120 fps: 12. The draw gets a 30 Hz real-time counter instead (vblanks / 2) and
   the real counter is put back after. Only above 30 fps with the real-time step; RUGRATS_BLINK=0 = old behaviour. */
void f_80031a28(void);
int rt_hook_80031a28(void) {
    static int inside, on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_BLINK"); on = e ? atoi(e) != 0 : 1; }
    if (inside || !on || !opt.fixed_timestep || PRESENT_FPS <= 30) return 0;
    uint32_t fc = mem_r32(0x800C3F24u);
    mem_w32(0x800C3F24u, hooks_frame() / 2);
    inside = 1;
    f_80031a28();
    inside = 0;
    mem_w32(0x800C3F24u, fc);
    return 1;
}

/* Proximity checker 80072d38(full): full != 0 checks every object on the list (head 0x800C4C20/gp+0xda4) and calls
   obj+0x48 for those in range; full == 0 (once per game frame from the level loops) steps through the list 5 objects
   at a time and calls obj+0x48 (player came in range) / obj+0x4c (player left) on each check. A door's "left"
   callback 8004d0b0 plays the closing sound 0x47 and is called again on every re-check until the door has shut, so at
   high fps the sound stacked up: 2 plays at 30 fps (PCSX-Redux: 2), 3 at 60, 5 at 120, 6 at 144. The incremental pass
   now runs at the PS1's 30 Hz game rate: once per 10 ticks of game time (remainder carried, at most one pass per frame;
   only with realtime_step on). RUGRATS_PROXRATE=0 = every frame (old), RUGRATS_SFXLOG shows the 0x47 calls. */
int rt_hook_80072d38(void) {
    static int on = -1, acc;
    if (on < 0) { const char *e = getenv("RUGRATS_PROXRATE"); on = e ? atoi(e) != 0 : 1; }
    if (!on || !opt.fixed_timestep || (cpu.r[4] & 0xff)) return 0;
    acc += (int32_t)mem_r32(0x800C3F1Cu);           /* gp+156: ticks this game frame */
    if (acc < 10) return 1;                         /* skip this frame's pass */
    acc -= 10;
    if (acc > 10) acc = 10;
    return 0;
}
int rt_hook_80072bf0(void) {
    if (!lbl_inside) tv_frame();      /* TV to Final Level: once per game frame, before the timers fire */
    if (lbl_inside || !opt.fixed_timestep) return 0;
    lbl_inside = 1;
    lbl_nfired = 0;
    timer_pass();           /* the original timer pass, callbacks with the time since they last fired */
    lbl_inside = 0;
    if (getenv("RUGRATS_LABEL_FLICKER")) return 1;
    uint32_t save[32];
    memcpy(save, cpu.r, sizeof save);
    int guard = 0;
    for (uint32_t t = mem_r32(0x800C43B8u); t && guard < 256; t = mem_r32(t), guard++) {   /* head = gp+1336 */
        uint32_t obj = mem_r32(t + 16);
        if (!obj || mem_r32(obj + 0x44) != 0x80049eecu) continue;
        int fired = 0;
        for (int i = 0; i < lbl_nfired; i++) fired |= lbl_fired[i] == obj;
        if (fired) continue;
        lbl_redraw = 1;
        cpu.r[4] = obj;
        cpu.r[31] = save[31];
        f_80049eec();
        lbl_redraw = 0;
    }
    memcpy(cpu.r, save, sizeof save);
    return 1;
}


/* Depth check (opt.depthfix, see gpu.c exec_poly): the game sorts room polygons by their farthest corner, so a big
   room face near the camera (a shelf top seen from above) can sort behind a world object that is actually behind it,
   and the object paints over it (the Reptar box through the toy-store shelf, 2026-11-08; same on a PS1, the
   right-stick camera's views just show it more). These wrappers tell the GTE which kind of geometry it projects:
   80010e14 = room geometry renderer (from 80033818), 80031b84 / 8003205c = world object draws (both end in the
   model renderer 80010000). HUD / menu models (8003f934 etc.) stay DCLS_NONE and are never depth tested. */
int gte_dcls = DCLS_NONE;
uint32_t gte_backface_frame = ~0u;   /* eye over the wall (rt_hook_80033278): room back faces drawn this frame */

/* Gap colour (2026-11-14, user: hedge maze seen from above). Some walls have no top polygon (hedge tops between
   two corridors), so looking down over them shows the frame's background clear (gpu.c exec_fill, full-screen fill;
   Grandpa's Teeth: 285078 = brown 120,80,40) as a thin band. In frames where the eye is over the wall (this or the
   previous frame: the clear comes before the room traversal) that fill takes the level's gap colour instead, a
   shade of its walls. Levels not in the table keep the game's colour. Colours 0xBBGGRR like GP0.
   RUGRATS_GAPCOLOR=<rrggbb> = that colour in every level, RUGRATS_GAPCOLOR=0 = off. */
static const struct { int db; uint32_t bgr; } gap[] = {   /* also the levels that get room back faces */
    { 2, 0x184018 },   /* Grandpa's Teeth (hedge maze): hedge shadow green 24,64,24 */
};
static int hooks_gap_level(void) {
    const int db = (int32_t)mem_r32(0x800C5678u);
    for (size_t i = 0; i < sizeof gap / sizeof gap[0]; i++)
        if (gap[i].db == db) return 1;
    return 0;
}
/* Levels that draw room / prop back faces (gte.c NCLIP) without being in the gap table: Reptar 2010 (db 7, user
   2026-11-14: "keep the buildings on"; the facades have no backs or roofs, seen from the high stick camera). */
static int hooks_backface_level(void) {
    static const int bf_levels[] = { 7 };
    if (hooks_gap_level()) return 1;
    const int db = (int32_t)mem_r32(0x800C5678u);
    for (size_t i = 0; i < sizeof bf_levels / sizeof bf_levels[0]; i++)
        if (bf_levels[i] == db) return 1;
    return 0;
}
uint32_t hooks_gap_fill(uint32_t col) {
    static int ov = -2; static uint32_t ovc;
    if (ov == -2) {
        const char *e = getenv("RUGRATS_GAPCOLOR");
        ov = -1;
        if (e && !strcmp(e, "0")) ov = 0;
        else if (e && *e) { unsigned v = (unsigned)strtoul(e, NULL, 16); ov = 1; ovc = (v >> 16 & 255) | (v & 0xff00) | (v & 255) << 16; }
    }
    if (!ov) return col;
    /* Out-of-bounds colour (2026-11-14, user): clear colours that show past the level edge over the hedges, swapped
       in every frame (keyed by the original colour, so the backyard sky of the same level is untouched).
       RUGRATS_GAPCOLOR=0 = off. */
    static const struct { int db; uint32_t from, to; } oob[] = {
        { 2, 0x3b0513, 0x102810 },   /* Grandpa's Teeth maze, night purple 19,5,59 -> tree canopy green 16,40,16 */
        { 7, 0x000100, 0x3a1c1c },   /* Reptar 2010, black 0,1,0 (missing roofs, past the edge) -> building blue 28,28,58 */
    };
    if (ov < 0) {
        const int db = (int32_t)mem_r32(0x800C5678u);
        for (size_t i = 0; i < sizeof oob / sizeof oob[0]; i++)
            if (oob[i].db == db && oob[i].from == col) return oob[i].to;
    }
    if (ow_over_frame == ~0u || present_count - ow_over_frame > 1) return col;
    if (ov == 1) return ovc;
    const int db = (int32_t)mem_r32(0x800C5678u);
    for (size_t i = 0; i < sizeof gap / sizeof gap[0]; i++)
        if (gap[i].db == db) return gap[i].bgr;
    return col;
}
/* Characters stay in front of props behind them (part of opt.objsort; gpu.c OID_ACTOR). The scene renderer 80033818
   draws each room's objects from two lists: room+0x48 = props (draw calls return to 80033a10) and room+0x4c = the
   moving characters, player included (return to 80033a7c). Each face is depth-sorted on its own, so a big prop face
   (a tall curtain whose top is near the camera) can sort in front of a character standing in front of it and cut a
   piece out of him (Tommy's head, 2026-11-09). While the player object is drawn, its model ids carry OID_ACTOR;
   props that never move carry OID_STATIC, and their faces clearly behind a player pixel are hidden (gpu.c
   exec_poly; not faces seen from above: the game draws shadows and floors like the toy store soap over a
   character's feet on purpose). Props that move (the lazy tongs Tommy carries, vehicles, thrown things) keep the
   game's order, also while they rest: a prop counts as fixed after ACTOR_STILL draws with the words at
   obj+0x10..0x3f unchanged, and never again once they changed (forgotten when it was not drawn for 5 s).
   Other characters (pie gallery clerks next to the stands) are left alone.
   RUGRATS_ACTORFRONT=0 off, 1 (default), 2 = every character, every prop and every face (the first try; debug). */
#define ACTOR_LIST_RA 0x80033a7cu
#define ACTOR_STILL 10
unsigned gte_oid_bits;
int gte_obj_backface;
int hooks_actor_front(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("RUGRATS_ACTORFRONT"); m = e ? atoi(e) : 1; }
    return opt.objsort ? m : 0;
}
static int prop_still(uint32_t obj) {
    static struct { uint32_t obj, sum, seen; int n, moved; } t[256];
    uint32_t sum = 0;
    for (uint32_t o = 0x10; o < 0x40; o += 4) sum = sum * 31u + mem_r32(obj + o);
    const unsigned i = (obj >> 2) * 2654435761u >> 24;
    const uint32_t now = hooks_frame();
    if (t[i].obj != obj || now - t[i].seen > 300) { t[i].obj = obj; t[i].sum = sum; t[i].n = 0; t[i].moved = 0; }
    t[i].seen = now;
    if (t[i].sum != sum) { t[i].sum = sum; t[i].moved = 1; }
    if (t[i].moved) return 0;
    if (t[i].n < ACTOR_STILL) t[i].n++;
    return t[i].n >= ACTOR_STILL;
}
static int dcls_run(void (*fn)(void), int *inside, int cls) {
    if (*inside || !opt.depthfix) return 0;
    int prev = gte_dcls;
    const unsigned prev_bits = gte_oid_bits;
    const int prev_bf = gte_obj_backface;
    *inside = 1; gte_dcls = cls;
    if (cls == DCLS_OBJ) {
        const int m = hooks_actor_front();
        const int actor = m == 2 ? cpu.r[31] == ACTOR_LIST_RA : cpu.r[4] == RCAM_PLAYER;
        /* prop that has not moved (prop list, not the player): also the only kind the occlusion test may skip */
        const int still = cpu.r[31] != ACTOR_LIST_RA && cpu.r[4] != RCAM_PLAYER ? prop_still(cpu.r[4]) : 0;
        occl_still_obj = still ? cpu.r[4] : 0;
        gte_oid_bits = !m ? 0 : actor ? OID_ACTOR : cpu.r[31] == ACTOR_LIST_RA ? 0 : (m == 2 || still) ? OID_STATIC : 0;
        /* Tree backs (2026-11-14, user, hedge maze): props (the prop list, not characters) get their back faces drawn
           in the gap-table levels, so flat tree cards seen from behind stay. RUGRATS_TREEBACK=0 = off. */
        static int tb = -1;
        if (tb < 0) { const char *e = getenv("RUGRATS_TREEBACK"); tb = e ? atoi(e) != 0 : 1; }
        gte_obj_backface = tb && cpu.r[31] != ACTOR_LIST_RA && cpu.r[4] != RCAM_PLAYER &&
                           (hooks_gap_level() || (hooks_backface_level() && gte_backface_frame == present_count));
    }
    fn();                                   /* the original */
    *inside = 0; gte_dcls = prev; gte_oid_bits = prev_bits; gte_obj_backface = prev_bf;
    if (cls == DCLS_OBJ) occl_still_obj = 0;
    return 1;
}
void f_80010e14(void);
void f_80031b84(void);
void f_8003205c(void);
int rt_hook_80010e14(void) { static int in; return dcls_run(f_80010e14, &in, DCLS_ROOM); }
static int obj_draw(void (*fn)(void), int *in, int cls) {   /* object draw + smoothed characters (cs_*) */
    if (*in) return 0;
    const int k = cs_n ? cs_draw_on(cpu.r[4]) : -1;
    if (k < 0) return dcls_run(fn, in, cls);
    if (!dcls_run(fn, in, cls)) { *in = 1; fn(); *in = 0; }
    cs_draw_off(k);
    return 1;
}
/* Back-to-back signs (2026-11-14, user: Reptar 2010 "L&K" sign mirrored at the middle). The sign is TWO objects,
   front and back, placed ~33 units apart and facing opposite ways; each model is a flat panel whose faces are all
   double-sided (face +0 bit 0: the mesh code skips NCLIP), so both panels are drawn from either side and the
   ordering table decides which one ends up on top: the lower half showed the mirrored back panel (same with the CPU
   renderer and every depth fix off: game data / original behaviour).
   Model draw 80010000(model) from the object draw (ra 80031d18, s0 = object): +0x10 vertex count, +0x14 vertices
   (8 bytes: x, y, z shorts), +0x1c face groups (word: type low 16, count high 16; type 0xff ends): 0x34 / 0x24 tri
   0x18 bytes, 0x30 / 0x20 tri 0xc, 0x3c / 0x2c quad 0x18, 0x28 / 0x38 quad 0xc; face +4 = vertex indices (bytes).
   Each model is checked once: "panel" = every face double-sided and every vertex on one plane. A panel object with
   another panel object of the same size within PAIR_DIST (world position obj+0x18/1c/20, seen this or last frame)
   is drawn one-sided: faces with screen area <= 0 (the game's NCLIP rule) are skipped, so each side shows only its
   own panel. Lone double-sided panels (leaves, flags) are untouched. RUGRATS_PAIRCULL=0 = off, RUGRATS_PAIRLOG=1. */
#define PAIR_DIST 96
static int pair_model;                       /* drawing a panel object that has a back-to-back partner */
static struct { uint32_t model, sig; uint8_t used, panel; int16_t lo[3], hi[3]; } pair_mc[1024];
static struct { uint32_t obj, model, frame; int32_t pos[3]; int16_t lo[3], hi[3]; } pair_obj[64];
static int pair_level = -2;
static int pair_panel(uint32_t m, int16_t lo[3], int16_t hi[3]) {
    const uint32_t verts = mem_r32(m + 0x14);
    const int nverts = (int16_t)mem_r16(m + 0x10);
    if (nverts < 3 || nverts > 64) return 0;
    double v[64][3];
    for (int k = 0; k < 3; k++) { lo[k] = 32767; hi[k] = -32768; }
    for (int i = 0; i < nverts; i++)
        for (int k = 0; k < 3; k++) {
            const int16_t c = (int16_t)mem_r16(verts + i * 8 + k * 2);
            v[i][k] = c;
            if (c < lo[k]) lo[k] = c;
            if (c > hi[k]) hi[k] = c;
        }
    uint32_t p = mem_r32(m + 0x1c);
    int faces = 0;
    double nrm[3] = { 0, 0, 0 }, d0 = 0;
    for (int g = 0; g < 64; g++) {
        const uint32_t w = mem_r32(p);
        const int type = (int)(w & 0xffffu), cnt = (int)(w >> 16);
        if (type == 0xff || cnt <= 0) break;
        int size;
        switch (type) {
        case 0x34: case 0x24: case 0x3c: case 0x2c: size = 0x18; break;
        case 0x30: case 0x20: case 0x28: case 0x38: size = 0x0c; break;
        default: return 0;
        }
        p += 4;
        for (int i = 0; i < cnt; i++, p += size) {
            if (!(mem_r32(p) & 1u)) return 0;   /* a one-sided face: not a panel */
            const uint32_t ix = mem_r32(p + 4);
            const int a = ix & 0xff, b = (ix >> 8) & 0xff, c = (ix >> 16) & 0xff;
            if (a >= nverts || b >= nverts || c >= nverts) return 0;
            if (!faces) {
                const double ax = v[b][0] - v[a][0], ay = v[b][1] - v[a][1], az = v[b][2] - v[a][2];
                const double bx = v[c][0] - v[a][0], by = v[c][1] - v[a][1], bz = v[c][2] - v[a][2];
                nrm[0] = ay * bz - az * by; nrm[1] = az * bx - ax * bz; nrm[2] = ax * by - ay * bx;
                const double l = sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                if (l < 1e-6) return 0;
                for (int k = 0; k < 3; k++) nrm[k] /= l;
                d0 = nrm[0] * v[a][0] + nrm[1] * v[a][1] + nrm[2] * v[a][2];
            }
            faces++;
        }
    }
    if (!faces) return 0;
    for (int i = 0; i < nverts; i++)
        if (fabs(nrm[0] * v[i][0] + nrm[1] * v[i][1] + nrm[2] * v[i][2] - d0) > 2.0) return 0;
    return 1;
}
void f_80010000(void);
int rt_hook_80010000(void) {
    static int in, on = -1, lg = -1;
    if (in) return 0;
    if (on < 0) { const char *e = getenv("RUGRATS_PAIRCULL"); on = e ? atoi(e) != 0 : 1; }
    if (lg < 0) lg = getenv("RUGRATS_PAIRLOG") != NULL;
    const uint32_t m = cpu.r[4], obj = cpu.r[16];
    if (!on || !m || cpu.r[31] != 0x80031d18u) return 0;
    if (hooks_level != pair_level) {   /* new level: models are reloaded */
        pair_level = hooks_level;
        memset(pair_mc, 0, sizeof pair_mc); memset(pair_obj, 0, sizeof pair_obj);
    }
    const uint32_t sig = mem_r32(m + 0x10) ^ mem_r32(m + 0x14) * 3u ^ mem_r32(m + 0x1c) * 7u ^ mem_r32(mem_r32(m + 0x1c));
    uint32_t h = (m >> 2) * 2654435761u >> 22;
    int k = 0;
    while (k < 1024 && pair_mc[h].used && pair_mc[h].model != m) { h = (h + 1) & 1023; k++; }
    if (k >= 1024) return 0;
    if (!pair_mc[h].used || pair_mc[h].sig != sig) {
        pair_mc[h].used = 1; pair_mc[h].model = m; pair_mc[h].sig = sig;
        pair_mc[h].panel = (uint8_t)pair_panel(m, pair_mc[h].lo, pair_mc[h].hi);
        if (lg && pair_mc[h].panel) rt_logf("pair: panel model %08x\n", m);
    }
    if (!pair_mc[h].panel) return 0;
    const int32_t pos[3] = { (int32_t)mem_r32(obj + 0x18), (int32_t)mem_r32(obj + 0x1c), (int32_t)mem_r32(obj + 0x20) };
    int me = -1, old = 0, partner = 0;
    for (int i = 0; i < 64; i++) {
        if (pair_obj[i].obj == obj) me = i;
        else if (pair_obj[i].obj && present_count - pair_obj[i].frame <= 2) {
            int near = 1, same = 1;
            for (int c = 0; c < 3; c++) {
                near &= abs(pair_obj[i].pos[c] - pos[c]) <= PAIR_DIST;
                same &= abs((pair_obj[i].hi[c] - pair_obj[i].lo[c]) - (pair_mc[h].hi[c] - pair_mc[h].lo[c])) <= 8;
            }
            if (near && same) partner = 1;
        }
        if ((int32_t)(pair_obj[old].frame - pair_obj[i].frame) > 0) old = i;
    }
    if (me < 0) me = old;
    pair_obj[me].obj = obj; pair_obj[me].model = m; pair_obj[me].frame = present_count;
    memcpy(pair_obj[me].pos, pos, sizeof pos);
    memcpy(pair_obj[me].lo, pair_mc[h].lo, sizeof pair_obj[me].lo); memcpy(pair_obj[me].hi, pair_mc[h].hi, sizeof pair_obj[me].hi);
    if (!partner) return 0;
    if (lg) rt_logf("f%u pair: obj %08x model %08x drawn one-sided\n", present_count, obj, m);
    pair_model = 1;
    in = 1; f_80010000(); in = 0;
    pair_model = 0;
    return 1;
}
static int near_pass;   /* this object draw was let through the near cull (rt_hook_8001eb4c): lens faces dropped */
static int near_hide;   /* ... and most of its faces were at the lens last frame: drop them all (camera inside it) */
static int near_slot = -1;
static struct { uint32_t obj, frame; int kept, lens, hidden; } near_tab[16];
/* Per object, faces drawn vs dropped at the lens in its last near-passed draw. When the camera moves into a big prop
   (the kitchen counter, radius 3901: the eye ends up 250 units from its centre) its faces pass the lens one by one
   and a stray far face is left floating (counter-top wedge). Hide the whole object once lens faces outnumber the drawn
   ones; show it again once fewer than a third are at the lens (hysteresis: no flicker at the switch). The counts keep
   running while it is hidden (faces are set up and counted, then dropped). */
static void near_begin(uint32_t obj) {
    int k = -1, old = 0;
    for (int i = 0; i < 16; i++) {
        if (near_tab[i].obj == obj) { k = i; break; }
        if ((int32_t)(near_tab[old].frame - near_tab[i].frame) > 0) old = i;
    }
    if (k < 0) { k = old; near_tab[k].obj = obj; near_tab[k].kept = near_tab[k].lens = near_tab[k].hidden = 0; }
    else if (present_count - near_tab[k].frame > 2) near_tab[k].kept = near_tab[k].lens = near_tab[k].hidden = 0;
    const int kept = near_tab[k].kept, lens = near_tab[k].lens;
    if (!near_tab[k].hidden && lens > kept) near_tab[k].hidden = 1;
    else if (near_tab[k].hidden && lens * 3 < kept + lens) near_tab[k].hidden = 0;
    near_tab[k].frame = present_count;
    near_tab[k].kept = near_tab[k].lens = 0;
    near_slot = k;
    near_hide = near_tab[k].hidden;
}
int rt_hook_80031b84(void) {
    static int in;
    const int r = obj_draw(f_80031b84, &in, DCLS_OBJ);
    if (r) { near_pass = 0; near_hide = 0; near_slot = -1; }
    return r;
}
/* Mesh face setup 800102dc (quads) / 80010174 (triangles), rooms + objects: s0 = face, t6 = projected vertex table
   (8 bytes per vertex, +0 SXY, +4 SZ (low 16) | outcode bits), face +4 = vertex indices (bytes). Returns t9 > 0 =
   draw. Objects the game never drew this close (near_pass) have corners at or behind the lens; the GTE gives those
   SZ 0 and a screen position anywhere, or pins them at the +-1024 screen limit (a slab from the counter edge to
   Tommy). Such faces are skipped: any corner SZ < RUGRATS_NEARFACE (default 48; 0 = keep all) or pinned.
   RUGRATS_NEARLOG=1 also logs every face of those objects. */
static void near_face(void (*fn)(void), int *in, int nv) {
    static int lim = -1, lg = -1;
    if (lim < 0) { const char *e = getenv("RUGRATS_NEARFACE"); lim = e ? atoi(e) : 48; }
    if (lg < 0) lg = getenv("RUGRATS_NEARLOG") != NULL;
    const uint32_t ix = mem_r32(cpu.r[16] + 4), tab = cpu.r[14];
    *in = 1; fn(); *in = 0;
    if ((int32_t)cpu.r[25] <= 0) return;
    if (pair_model) {   /* back-to-back panel (rt_hook_80010000): only the side facing the camera */
        int32_t sx[3], sy[3];
        for (int i = 0; i < 3; i++) {
            const uint32_t off = i == 0 ? (ix & 0xffu) << 3 : (ix >> (i * 8 - 3)) & 0x7f8u;
            sx[i] = (int16_t)mem_r16(tab + off); sy[i] = (int16_t)mem_r16(tab + off + 2);
        }
        const int64_t a = (int64_t)(sx[1] - sx[0]) * (sy[2] - sy[0]) - (int64_t)(sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (a <= 0) { cpu.r[25] = 0; return; }
    }
    if (!near_pass || lim <= 0) return;
    int lens = 0;
    for (int i = 0; i < nv && !lens; i++) {
        const uint32_t off = i == 0 ? (ix & 0xffu) << 3 : (ix >> (i * 8 - 3)) & 0x7f8u;
        const uint32_t sz = mem_r32(tab + off + 4) & 0xffffu;
        const int sx = (int16_t)mem_r16(tab + off), sy = (int16_t)mem_r16(tab + off + 2);
        lens = sz < (uint32_t)lim || sx <= -1024 || sx >= 1023 || sy <= -1024 || sy >= 1023;
    }
    if (near_slot >= 0) { if (lens) near_tab[near_slot].lens++; else near_tab[near_slot].kept++; }
    if (lg) {
        char b[200]; int n = 0;
        for (int i = 0; i < nv; i++) {
            const uint32_t off = i == 0 ? (ix & 0xffu) << 3 : (ix >> (i * 8 - 3)) & 0x7f8u;
            n += snprintf(b + n, sizeof b - n, " %d,%d/%u", (int16_t)mem_r16(tab + off), (int16_t)mem_r16(tab + off + 2),
                          mem_r32(tab + off + 4) & 0xffffu);
        }
        rt_logf("f%u nearface %s%s\n", present_count, lens ? "lens" : near_hide ? "hidden" : "kept", b);
    }
    if (lens || near_hide) cpu.r[25] = 0;
}
void f_800102dc(void);
void f_80010174(void);
int rt_hook_800102dc(void) { static int in; if (in || !(near_pass || pair_model)) return 0; near_face(f_800102dc, &in, 4); return 1; }
int rt_hook_80010174(void) { static int in; if (in || !(near_pass || pair_model)) return 0; near_face(f_80010174, &in, 3); return 1; }
int rt_hook_8003205c(void) { static int in; return obj_draw(f_8003205c, &in, DCLS_OBJ); }
/* Object near cull in widescreen (2026-11-14, user: kitchen counter cabinet popping at the left screen edge while
   turning the stick camera right). The object draw 80031b84 gets the object's camera-space centre z from 8001eb4c
   (ra 80031c3c) and skips the object when z < H (projection distance, 315 here) unless obj+0 bit 0x800 is set; only
   then does the sphere test 80031834 run. In 4:3 an object that close is either in front of the lens (hiding it is
   intended) or off to the side and out of view anyway; the 16:9 side bands show the side ones, so a big prop (the
   kitchen counter, obj 800ee864, centre x -3584 at z 314) vanished whole while its far end was still on screen.
   Widescreen + widecull: a centre past the 4:3 frustum side (|x| * H > half width * z, screen width at 0x800C4A04, or z <= 0)
   gets z = H returned, so it reaches 80031834, which keeps the original rules there (fully behind: cull, touching
   the camera plane: draw, else the squeezed circle vs the rect). Objects in front of the lens keep the near cull.
   The multi-part draw 8003205c (ra 80032284) keeps the z for sorting and is left alone.
   RUGRATS_NEARSIDE=0 = off, RUGRATS_NEARLOG=1 logs every near-culled object (and whether it was let through). */
void f_8001eb4c(void);
int rt_hook_8001eb4c(void) {
    static int in, lg = -1, on = -1;
    if (in) return 0;
    if (lg < 0) lg = getenv("RUGRATS_NEARLOG") != NULL;
    if (on < 0) { const char *e = getenv("RUGRATS_NEARSIDE"); on = e ? atoi(e) != 0 : 1; }
    const uint32_t ra = cpu.r[31], obj = cpu.r[16];
    in = 1; f_8001eb4c(); in = 0;
    if (ra != 0x80031c3cu) return 1;
    near_pass = 0; near_hide = 0; near_slot = -1;
    const int32_t z = (int32_t)cpu.r[2], h = (int32_t)gte_read(58);
    if (z >= h || z > 0xfde8) return 1;
    const int32_t x = (int32_t)gte_read(37);
    const int32_t hw = (int16_t)mem_r16(0x800C4A04u) / 2;   /* screen half width (256) */
    const int side = z <= 0 || (int64_t)(x < 0 ? -(int64_t)x : x) * h > (int64_t)hw * z;
    const int pass = on && opt.widescreen && opt.widecull && side && !(mem_r32(obj) & 0x800u);
    if (pass) { cpu.r[2] = (uint32_t)h; near_pass = 1; near_begin(obj); }
    if (lg)
        rt_logf("f%u near obj %08x z %d h %d x %d r %d flags %08x%s\n", present_count, obj, z, h, x,
                (int)mem_r16(mem_r32(obj + 0x88) + 8), mem_r32(obj),
                !pass ? "" : near_hide ? " -> sphere test, hidden (lens)" : " -> sphere test");
    return 1;
}

/* Room geometry chunk visibility 80032cf4(box*, rect*) (called by the scene renderer 80033818 per chunk, room +0x40
   list, box = node+4: min x,y,z at +0/+2/+4, max at +8/+a/+c, room-local shorts). It projects the 8 corners through
   the GTE (SXY to scratch 0x1F800100.., SZ to 0x1F800180..), returns 0 when every SZ is 0 or every SZ > 65000,
   else outcodes each corner against the rect and culls when all share a side. */
void f_80032cf4(void);
int rt_hook_80032cf4(void) {
    static int in, lg = -1;
    if (in) return 0;
    if (lg < 0) lg = getenv("RUGRATS_CHUNKLOG") != NULL;
    const uint32_t box = cpu.r[4], rect = cpu.r[5], ra = cpu.r[31];
    in = 1; f_80032cf4(); in = 0;
    /* Occlusion (2026-11-16, see occl_box): a chunk that passed goes to the GPU as an occlusion query of its box
       (the GTE holds the room's matrix here); once the answers say it is behind nearer room walls it is skipped.
       Levels of the gap table only (where the low-wall rule draws many rooms the PS1 never did).
       RUGRATS_OCCLCHUNK=0 = off. */
    {
        static int on = -1;
        if (on < 0) { const char *e = getenv("RUGRATS_OCCLCHUNK"); on = e ? atoi(e) != 0 : 1; }
        if (on && cpu.r[2] && occl_on() && hooks_gap_level()) {
            const double lo[3] = { (int16_t)mem_r16(box), (int16_t)mem_r16(box + 2), (int16_t)mem_r16(box + 4) };
            const double hi[3] = { (int16_t)mem_r16(box + 8), (int16_t)mem_r16(box + 10), (int16_t)mem_r16(box + 12) };
            /* a chunk known to be visible is tested again only every 8th present (staggered by address): its box
               passes the depth test over its whole area, which costs GPU fill at high internal resolutions (40
               boxes a frame: +0.4 ms at 9x); hidden and unknown ones are tested every frame */
            int hw_occl_seen(uint32_t id, int age);
            if (hw_occl_seen(box, 12) && ((present_count + (box >> 4)) & 7)) {
            } else if (occl_send(box, lo, hi) && hw_occl_hidden(box, OCCL_HOLD)) {
                cpu.r[2] = 0;
                if (lg) rt_logf("f%u chunk %08x hidden (occlusion)\n", present_count, box);
            }
        }
    }
    if (lg) {
        char b[400]; int n = 0;
        for (int i = 0; i < 8; i++)
            n += snprintf(b + n, sizeof b - n, " %d,%d/%d", (int16_t)mem_r16(0x1F800100u + i * 4),
                          (int16_t)mem_r16(0x1F800102u + i * 4), (int)mem_r32(0x1F800180u + i * 4));
        rt_logf("f%u chunk %08x ra %08x box %d,%d,%d..%d,%d,%d rect %d,%d %dx%d -> %d |%s\n", present_count, box, ra,
                (int16_t)mem_r16(box), (int16_t)mem_r16(box + 2), (int16_t)mem_r16(box + 4), (int16_t)mem_r16(box + 8),
                (int16_t)mem_r16(box + 10), (int16_t)mem_r16(box + 12), (int16_t)mem_r16(rect), (int16_t)mem_r16(rect + 2),
                (int16_t)mem_r16(rect + 4), (int16_t)mem_r16(rect + 6), (int)cpu.r[2], b);
    }
    return 1;
}

/* Room water surfaces (sewer brown water, 2026-11-10): the scene renderer 80033818 calls 80030ce0 per room water plane
   (room+0x3c list); it projects a grid with RotTransPers3 and draws translucent quads. The grid runs on under the
   raised floor next to it, and the game sorts those quads after the floor, so square bits of water showed through
   the ledge (they popped in and out as the camera turned). Its points now get object depth: the depth check hides
   water clearly behind nearer room geometry. No model-sort bits (water is not a prop or the player).
   RUGRATS_WATERDEPTH=0 = old (never tested). */
void f_80030ce0(void);
int rt_hook_80030ce0(void) {
    static int in, on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_WATERDEPTH"); on = e ? atoi(e) != 0 : 1; }
    if (in || !opt.depthfix || !on) return 0;
    const int prev = gte_dcls;
    const unsigned prev_bits = gte_oid_bits;
    in = 1; gte_dcls = DCLS_OBJ; gte_oid_bits = 0;
    f_80030ce0();
    in = 0; gte_dcls = prev; gte_oid_bits = prev_bits;
    return 1;
}
/* The room renderer splits near polygons in 2D: 800120fc (triangle) and 80011854 (quad) put the edge midpoints
   ((a+b)>>1 of the packed screen x/y in s1..s4) in t1..t4 / t9. Those points never went through the GTE, so they had
   no depth and every piece touching one skipped the depth check (box showing through a split wall). Give each
   midpoint the average depth of its two ends; nested splits chain through the earlier midpoints. */
void f_800120fc(void);
void f_80011854(void);
void subpx_split(int n, const uint32_t *v, const uint32_t *src, int nm, const int (*mids)[3]);
int rt_hook_800120fc(void) {
    static int in;
    if (in || !opt.depthfix) return 0;
    const uint32_t v[3] = { cpu.r[17], cpu.r[18], cpu.r[19] }, src[3] = { tag_ld[17], tag_ld[18], tag_ld[19] };
    in = 1; f_800120fc(); in = 0;
    static const int mids[3][3] = { { 9, 0, 1 }, { 11, 0, 2 }, { 10, 1, 2 } };   /* register, ends */
    subpx_split(3, v, src, 3, mids);
    return 1;
}
int rt_hook_80011854(void) {
    static int in;
    if (in || !opt.depthfix) return 0;
    const uint32_t v[4] = { cpu.r[17], cpu.r[18], cpu.r[19], cpu.r[20] };
    const uint32_t src[4] = { tag_ld[17], tag_ld[18], tag_ld[19], tag_ld[20] };
    in = 1; f_80011854(); in = 0;
    /* register, ends (0..3 = corners, 4.. = earlier midpoints): ab, ac, cd, bd, centre = mid(bd, ac) */
    static const int mids[5][3] = { { 9, 0, 1 }, { 12, 0, 2 }, { 11, 2, 3 }, { 10, 1, 3 }, { 25, 7, 5 } };
    subpx_split(4, v, src, 5, mids);
    return 1;
}

/* 80011a28: the quad split's texture coordinates ((a+b)>>1 of s1..s4 into t1..t4/t9, right after 80011854 split the
   positions): keep each split point's exact texture fraction (gpu.c subpx_split_uv). */
void f_80011a28(void);
void subpx_split_uv(const uint32_t *uv);
int rt_hook_80011a28(void) {
    static int in;
    if (in || !opt.depthfix || !opt.perspective) return 0;
    const uint32_t uv[4] = { cpu.r[17], cpu.r[18], cpu.r[19], cpu.r[20] };
    in = 1; f_80011a28(); in = 0;
    subpx_split_uv(uv);
    return 1;
}

/* Idle voice lines (opt.chatter). 800818e0 runs the current level's chatter function (table 0x800B9618 by database;
   e.g. 80080a20 in the toy store). Each one counts the timer gp+0xe40 down by the frame's ticks (gp+156, 1/300 s of
   real time, so the same on a PS1 at any frame rate) and when it runs out plays a random line (8008da4c = XA voice
   line) and resets it, mostly to 1800 + rand(0..255) * 16 ticks = 6.0 .. 19.6 s (some levels rand * 8 = 0 .. 6.8 s).
   Every chatter output is gated by that timer, so the option only touches it: after a reset the new gap is scaled
   (x2 / x4); Off keeps it from running out. Cutscene and event lines call 8008da4c from elsewhere: unchanged. */
void f_800818e0(void);
static int chatter_inside; /* 1 while the level's chatter function runs (Restored Lines below) */
static int chatter_db;     /* the level 800818e0 dispatches on (0x800C5678; -1 = golf hole) */
static int hub_force;      /* 1 while hub_chatter (timed chatter) asks 8008da4c for a restored line (no 50% gate, no fallback) */
void f_8008da4c(void);
static int golf_frame(int32_t dt);   /* Golf Reactions (below) */
static void circus_frame(int32_t dt); /* Circus target line (below) */
static int golf_mute;                /* 1 while the held hole finish runs: its own voice requests are skipped */
/* Timed chatter (part of Restored Lines, user 2026-11-17) for levels with no chatter function (0x800B9618[db] = 0)
   whose restored lines would stay unheard: hub (db 1: 29 159 193 196 200 225, Tommy), Egg Hunt (db 18: 686 687
   688); none of them used by those levels' triggers / hint objects (RAM scan of in-level states, 2026-11-17).
   Not The Return of Mr. Friend (db 14): it runs with the HUB's voice bank loaded (gp+0x77c ISF and the 34-line
   table = db 1's), its own bank with 1135 never loads; 1135 is also in The Mysterious Mr. Friend's bank (db 9,
   has chatter; there it only stands in for a chase line, RS_CHASE). After a gap of game time (timed_chatter_gap;
   gp+156 ticks, 1/300 s, the same clock the level chatter uses; paused / menus don't call 800818e0) this asks the game's voice player for one of them
   (unheard first, then repeats; see rt_hook_8008da4c). Off with Restored Lines Off or Chatter Frequency Off. A
   line already playing -> retry in 5 s. The timer re-arms each time such a level is entered. (No More Cookies has
   a chatter function, 8008046c, so its lines come through the normal swap.) */
static int timed_chatter_level(int db) { return db == 1 || db == 18; }
/* Gap until the next timed line: hub 60..120 s (user); Egg Hunt like the game's own level chatter, 1800 +
   rand(0..255) * 16 ticks = 6.0..19.6 s, x2 / x4 for Chatter Frequency Less / Rare (user, 2026-11-17). */
static int32_t timed_chatter_gap(uint32_t r) {
    if (chatter_db == 1) return 18000 + (int32_t)(r % 18001u);
    int32_t g = 1800 + (int32_t)(r & 0xffu) * 16;
    return opt.chatter == 1 ? g * 2 : opt.chatter == 2 ? g * 4 : g;
}
static void hub_chatter(int32_t dt) {
    static int32_t left = -1;
    static int last_db = -2;
    static uint32_t rng = 0x9E3779B9u;
    if (chatter_db != last_db) { left = -1; last_db = chatter_db; }
    if (!timed_chatter_level(chatter_db) || !opt.voice_restore || opt.chatter == 3 || (chatter_db == 1 && !opt.voice_hub)) {
        left = -1; return;                               /* hub: own switch, Unused Hub Lines (opt.voice_hub) */
    }
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    if (left < 0) { left = timed_chatter_gap(rng); return; }
    if (dt > 0 && dt < 3000) left -= dt;
    if (left > 0) return;
    const int16_t st = (int16_t)mem_r16(cpu.r[28] + 0x758);
    if (st != 0 && st != 9) { left = 1500; return; }     /* a line is playing (cutscene, hint) */
    uint32_t save[32], hi = cpu.hi, lo = cpu.lo;
    memcpy(save, cpu.r, sizeof save);
    cpu.r[4] = 0;                                        /* replaced by the hook; never played as is */
    chatter_inside = 1; hub_force = 1;
    f_8008da4c();
    chatter_inside = 0; hub_force = 0;
    memcpy(cpu.r, save, sizeof save);
    cpu.hi = hi; cpu.lo = lo;
    left = timed_chatter_gap(rng);
}
int rt_hook_800818e0(void) {
    static int inside;
    if (inside) return 0;
    const uint32_t tmr = cpu.r[28] + 0xe40u;
    int32_t t0 = (int32_t)mem_r32(tmr);
    if (opt.chatter == 3) {
        if (t0 < 0x01000000) mem_w32(tmr, 0x7F000000u); /* never runs out */
    } else if (t0 >= 0x01000000) {
        mem_w32(tmr, 1800u);                             /* back from Off: the game's minimum gap */
    }
    t0 = (int32_t)mem_r32(tmr);
    int32_t dt = (int32_t)mem_r32(cpu.r[28] + 156);
    chatter_db = mem_r8(0x800C5699u) == 1 ? -1 : (int)mem_r32(0x800C5678u);
    {   static int vlog = -1; static uint32_t last_who = 0xffffffffu;
        if (vlog < 0) vlog = getenv("RUGRATS_VOXLOG") != NULL;
        uint32_t who = mem_r32(0x800C5680u);                 /* playing character = player obj+0x15c (8005928c..) */
        if (vlog && who != last_who) rt_logf("vox character %u (db %d) vbl %u\n", who, chatter_db, hooks_frame());
        last_who = who; }
    circus_frame(dt);                                    /* Circus: 762 waiting for a free voice */
    if (golf_frame(dt)) return 1;                        /* Golf Reactions: holding a hole end -> no chatter */
    inside = 1; chatter_inside = 1;
    f_800818e0();                                        /* the original */
    inside = 0; chatter_inside = 0;
    int32_t t1 = (int32_t)mem_r32(tmr);
    if ((opt.chatter == 1 || opt.chatter == 2) && t1 > t0 - dt && t1 >= 0 && t1 < 0x00100000)
        mem_w32(tmr, (uint32_t)(t1 * (opt.chatter == 1 ? 2 : 4)));
    hub_chatter(dt);
    return 1;
}

/* Restored Lines (opt.voice_restore). Each level's SPEECHTB.DAT lists its voice clips by id; 160 ids (177 entries,
   ~7.6 min) are never requested by the game: not by any call site, id list in the EXE (0x800B9220.., 0x800B96C8..),
   level trigger or hint object (survey 2026-11-17, HANDOFF). Every clip on the disc has an entry, so these are all
   of them. When the level's chatter function (800818e0 above) asks 8008da4c for a random line, this swaps the id,
   half the time, for one of those that the CURRENT level's table holds and that has not played yet (byte at
   gp+0x778 + index, bit 0 = played: ids < 2040 get it when they play. The game itself only blocks a line whose
   bits are exactly 1; table entries with initial flag 2 (135 of 811) stay repeatable that way, so this tests bit 0
   alone: unheard restored lines go first; once all have played they repeat, see below). The game's own player does the rest (line already playing -> skipped,
   Dialogue volume). Levels without a chatter function (hub, Cookie Race, Return of Mr. Friend,
   Egg Hunt: 13 entries) never call it, so their lines stay unheard. Own RNG: the game's rand sequence is untouched. */
static const uint16_t restore_ids[] = {
    29, 33, 34, 35, 36, 37, 70, 73, 82, 94, 95, 97, 103, 123, 136, 137, 141, 142, 143, 147, 148, 159, 193, 196,
    200, 215, 220, 221, 224, 225, 230, 231, 233, 268, 270, 275, 279, 280, 288, 296, 297, 298, 299, 300, 303, 308,
    310, 315, 336, 337, 338, 347, 360, 397, 398, 404, 407, 470, 517, 524, 528, 537, 552, 553, 581, 586, 587, 588,
    589, 593, 598, 617, 618, 624, 625, 626, 627, 628, 634, 635, 636, 642, 647, 648, 653, 656, 658, 666, 667, 670,
    671, 685, 686, 687, 688, 692, 693, 728, 762, 769, 770, 771, 772, 773, 774, 775, 809, 810, 811, 812, 820, 859,
    868, 897, 898, 899, 900, 901, 904, 920, 930, 939, 949, 980, 981, 983, 984, 986, 995, 1052, 1073, 1080, 1083,
    1084, 1118, 1123, 1124, 1125, 1126, 1128, 1129, 1130, 1131, 1132, 1133, 1135, 1181, 1182, 1185, 1186, 1187,
    1188, 1189, 1190, 1191, 1193, 1194, 1211, 1212, 1218,
};
static int restore_listed(unsigned id) {
    int lo = 0, hi = (int)(sizeof restore_ids / sizeof restore_ids[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (restore_ids[m] == id) return 1;
        if (restore_ids[m] < id) lo = m + 1; else hi = m - 1;
    }
    return 0;
}
/* Who says a restored line, in levels where the player character changes (sorted by ear, 2026-11-17). who = the
   playing character, 0x800C5680 (= player object 0x800E2B48 +0x15c, written by the player swaps 800585b0 /
   8005928c / 80059328; also what 8008df78 adds to 2040 for the baby's own lines): 1 Tommy, 2 Chuckie, 3 Phil,
   4 Lil (seen on golf hole states: 4 = Lil; 3 = Phil by elimination - NOTE: on the golf holes 3 is Angelica and
   5 Phil, seen 2026-11-18; no entry below depends on 3), 6 Angelica, 9 Reptar. Toy Palace's own
   chatter (80080a20) branches on it too (1 = Tommy half, 2 = Chuckie half). RS_ANY = plays whoever is playing
   (Angelica, present but not playable); RS_SKIP = never restore in that level (a copy of a line the game already
   plays, or a line in the wrong level). Ids not listed for a level play in any part of it. */
#define RS_ANY 0
#define RS_SKIP 0xff
#define RS_CHASE 0xfe   /* only in place of one of the level's enemy chase lines (Mr. Friend, db 9) */
enum { RS_TOMMY = 1, RS_CHUCKIE = 2, RS_PHIL = 3, RS_LIL = 4 };
static const struct { uint8_t db; uint8_t who; uint16_t id; } restore_speaker[] = {
    /* Toy Palace */
    {6, RS_TOMMY, 94}, {6, RS_TOMMY, 95}, {6, RS_TOMMY, 136},
    {6, RS_CHUCKIE, 279}, {6, RS_CHUCKIE, 280}, {6, RS_CHUCKIE, 288}, {6, RS_CHUCKIE, 310}, {6, RS_CHUCKIE, 315},
    {6, RS_CHUCKIE, 398}, {6, RS_CHUCKIE, 470},
    {6, RS_SKIP, 1135},
    /* Touchdown Tommy: its chatter (8008046c, shared with No More Cookies) picks from 31 lines of all babies and
       Angelica whoever is playing, so restored lines do too (user, 2026-11-17). Sorted by ear: Tommy 34-37,
       Chuckie 298 299 300 303 308, Phil 772-775, Lil 899 900 901 904 920 930, Angelica 685. */
    {21, RS_SKIP, 360},                                  /* sounds misplaced (user) */
    /* The Mysterious Mr. Friend: 1135 is the 11th of Mr. Friend's own clips (1135..1146 = ISF offsets 1..8 + 3
       more). Its chatter 80080e08 speaks only while gp+0xe58 > 0 and one of objects 0x2e / 0x320 / 0x5b (800461f4)
       has +0x54 set, i.e. while Mr. Friend is after the player, and then picks rand % 10 of 1136..1146 (list
       0x800B9468); otherwise it asks for id 0. User: 1135 only while that enemy chases, never ambient -> RS_CHASE:
       swapped in only for a request of one of those 10 lines. (The Return of Mr. Friend, db 14, runs with the hub's
       voice bank: none of Mr. Friend's lines are loaded there, the game's own included.) */
    {9, RS_CHASE, 1135},
    /* Incident on Aisle 7: its chatter 80080330 (timer gp+0xe40), while the pie gallery runs (level word 0x800C56A8
       & 8), picks rand & 7 of the deli clerks' list 0x800B92DC [1067 1074 1075 1078 1081 1082 1085 1086], gap 2000
       ticks (6.7 s); outside it Tommy's lines. 1073 1080 1083 1084 are the clerks' other clips (sorted by ear by the
       user, 2026-11-18): only in place of one of those 8 (deli_line). Tommy's restored lines (103 215 220 221 224
       230 231 233) never replace a clerk's line. Rule in restore_allowed (db 3). */
};
static int mrfriend_chase_line(unsigned id) { return id >= 1136 && id <= 1146 && id != 1143; }
static int deli_line(unsigned id) {
    return id == 1067 || id == 1074 || id == 1075 || id == 1078 || id == 1081 || id == 1082 || id == 1085 || id == 1086;
}
static int restore_allowed(int db, unsigned id, unsigned requested) {
    if (db >= 23 && db <= 31) return 0;  /* golf holes: all 27 restored DB08 lines have their own trigger (Golf Reactions) */
    if (db == 3) return deli_line(requested) ? (id >= 1073 && id <= 1084) : id < 1073; /* clerks <-> clerks, Tommy <-> Tommy */
    /* Circus Angelicus: 33 97 141 are leftover copies of No More Cookies' cookie lines (they stay in db 20's chatter);
       762 plays only on the trike target landing (rt_hook_8007db6c). Angelica's 667 670 671 stay. */
    if (db == 11 && (id == 33 || id == 97 || id == 141 || id == 762)) return 0;
    for (size_t i = 0; i < sizeof restore_speaker / sizeof restore_speaker[0]; i++)
        if (restore_speaker[i].db == db && restore_speaker[i].id == id) {
            const unsigned who = restore_speaker[i].who;
            if (who == RS_SKIP) return 0;
            if (who == RS_CHASE) return mrfriend_chase_line(requested);
            return who == RS_ANY || who == mem_r32(0x800C5680u);
        }
    return 1;
}
int rt_hook_8008da4c(void) {
    static uint32_t rng = 0x2545F491u;
    static int log = -1;
    if (log < 0) log = getenv("RUGRATS_VOXLOG") != NULL;
    const uint32_t gp = cpu.r[28];
    if (log) rt_logf("vox play %u ra %08x chatter %d state %d vbl %u\n", cpu.r[4], cpu.r[31], chatter_inside,
                     (int16_t)mem_r16(gp + 0x758), hooks_frame());
    if (golf_mute) { if (log) rt_logf("vox golf: muted %u (reaction already played)\n", cpu.r[4]); return 1; }
    if (!opt.voice_restore || !chatter_inside) return 0;
    const int none = hub_force;                          /* hub chatter: nothing to swap in -> play nothing */
    int16_t st = (int16_t)mem_r16(gp + 0x758);
    if (st != 0 && st != 9) return none;                 /* a line is playing: the game skips this request anyway */
    const uint32_t ids = mem_r32(gp + 0x76c), played = mem_r32(gp + 0x778);
    const int n = (int16_t)mem_r16(gp + 0x76a);
    if (!ids || !played || n <= 0 || n > 1024) return none;
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    if (!hub_force && (rng & 1)) return 0;               /* half the picks stay the game's own */
    /* fresh = restored lines of this level / part that have not played; when none are left, all of them repeat
       (repeat = 1): then the played bit of the pick is cleared (the game refuses a line whose flags are exactly 1
       and sets bit 0 again when it plays), the line just restored is not picked twice in a row, and the swap
       chance drops to nc / (nc + 8) so a few restored lines don't crowd out the level's own chatter. */
    static unsigned last_pick;
    int cand[1024], nc = 0, repeat = 0;
    for (int pass = 0; pass < 2 && !nc; pass++) {
        for (int i = 0; i < n; i++) {
            unsigned id = mem_r16(ids + 2u * (uint32_t)i) & 0xfffu;
            if (!restore_listed(id) || !restore_allowed(chatter_db, id, cpu.r[4] & 0xfffu)) continue;
            if (pass == 0 && (mem_r8(played + (uint32_t)i) & 1u)) continue; /* bit 0 = played */
            cand[nc++] = i;
        }
        repeat = pass;
    }
    if (repeat && nc > 1)                                /* no line twice in a row */
        for (int k = 0; k < nc; k++)
            if ((mem_r16(ids + 2u * (uint32_t)cand[k]) & 0xfffu) == last_pick) { cand[k] = cand[--nc]; break; }
    if (log) rt_logf("vox candidates %d repeat %d (db %d part %u)\n", nc, repeat, chatter_db, mem_r32(0x800C5680u));
    if (!nc) return none;
    if (!hub_force && repeat && (rng >> 20) % (uint32_t)(nc + 8) >= (uint32_t)(2 * nc)) return 0; /* 50% x 2nc/(nc+8) */
    const int pi = cand[(rng >> 8) % (uint32_t)nc];
    unsigned pick = mem_r16(ids + 2u * (uint32_t)pi) & 0xfffu;
    if (repeat) mem_w8(played + (uint32_t)pi, (uint8_t)(mem_r8(played + (uint32_t)pi) & ~1u));
    last_pick = pick;
    if (log) rt_logf("vox restored %u instead of %u%s (db %d part %u)\n", pick, cpu.r[4], repeat ? " (repeat)" : "",
                     chatter_db, mem_r32(0x800C5680u));
    cpu.r[4] = pick;
    return 0;                                            /* the original plays the swapped id */
}

/* ---- Golf Reactions (opt.voice_golf, user 2026-11-18; HANDOFF has the trace) ------------------------------------
   Golf holes are db 23..31 (all use Ice Cream Mountain's DB08 voice bank). Golf character = player obj +0x15c:
   1 Tommy, 2 Chuckie, 3 Angelica, 4 Lil, 5 Phil. Hole index = 80057d50(db) (table below), par = byte 0x800B7FFC[idx],
   strokes = player +0x1b6[idx] (counted at ball launch, 8005fcb8).
   Hole finish 8007a9d8(player) (ball in cup, 800580a8) asks the voice player for ONE line and in the same frame
   queues the next hole (holes 1-8: the scorecard music's Setloc takes the drive before a voice sector is read) or
   the "You were over par" screen (8008dbec pauses the CD), so in the shipped game these lines are never heard.
   Only hole 9 at / under par (announcer + course end) is audible. Here: pick the line the game would (plus the
   restored ones, per character, sorted by ear by the user), start it, and run 8007a9d8 only when it has finished
   (its own voice requests muted; pad ignored meanwhile). No line for this result -> 8007a9d8 runs at once.
   Ball rolling: the game's chatter plays these only if its timer runs out during the 0.67 s swing (state 15);
   here 1 swing in 3 gets one 0.3 s after launch (unheard first, never the same twice in a row). */
void f_8007a9d8(void);
static uint32_t golf_hold;                  /* player object while a hole end waits for its line, else 0 */
static int32_t golf_hold_left, golf_hold_tail, golf_ball_left = -1;
static uint32_t golf_rng = 0x6C8E9CF5u;
static uint32_t golf_ball_last, golf_hold_id;  /* golf_hold_id: reaction still to say once the playing line ends */
static void golfvox_state(StateIO *s) {
    ST(s, golf_hold); ST(s, golf_hold_left); ST(s, golf_hold_tail); ST(s, golf_ball_left); ST(s, golf_ball_last);
    ST(s, golf_hold_id);
}
int hooks_golf_holding(void) { return golf_hold != 0; }
static int golf_log(void) { static int l = -1; if (l < 0) l = getenv("RUGRATS_VOXLOG") != NULL; return l; }
static uint32_t golf_rand(void) {
    static int seeded;
    if (!seeded) {                               /* else the first swing after every launch would get a ball line */
        seeded = 1;
        const char *e = getenv("RUGRATS_GOLFSEED");      /* tests: fixed seed */
        golf_rng ^= e ? (uint32_t)strtoul(e, 0, 0) : (uint32_t)time(NULL) * 2654435761u;
        if (!golf_rng) golf_rng = 1;
    }
    golf_rng ^= golf_rng << 13; golf_rng ^= golf_rng >> 17; golf_rng ^= golf_rng << 5; return golf_rng;
}
/* golf db -> hole index (80057d50), -1 = not a golf hole / title demo */
static int golf_idx(void) {
    static const int8_t idx[9] = { 4, 2, 0, 6, 8, 1, 3, 5, 7 };   /* db 23..31 */
    const uint32_t db = mem_r32(0x800C5678u);
    if (db < 23 || db > 31 || mem_r8(0x800C5699u) == 1) return -1;
    return idx[db - 23];
}
enum { GT = 1, GC = 2, GA = 3, GL = 4, GP = 5 };
#define GPICK(a) ((a)[golf_rand() % (sizeof(a) / sizeof((a)[0]))])
/* the line for this hole result, or 0 = none (then the game runs as usual) */
static unsigned golf_pick(int who, int s, int p, int idx) {
    if (s <= p) {
        if (idx >= 8) return 0;                  /* hole 9: the game's announcer line, already heard */
        if (s == 1) {                            /* hole in one */
            if (who == GT) return 70;
            if (who == GA) return 586;
            if (who == GP) return 812;
        }
        if (s == p - 1) {                        /* birdie (Tommy 73 restored: the game has no Tommy birdie) */
            if (who == GT) return 73;
            if (who == GC) return 338;
            if (who == GA) return 589;
            if (who == GP) return 811;
            if (who == GL) return 939;
        }
        /* the game's own pick (lists 0x800B92A8 / B4 / BC / C4); Tommy has none */
        static const uint16_t c[] = { 1195, 1206, 305, 335, 339 }, a[] = { 1195, 1206, 590, 620 },
                              ph[] = { 1195, 1206, 813 }, l[] = { 1195, 1206, 936, 940, 994 };
        switch (who) {
        case GC: return GPICK(c);
        case GA: return GPICK(a);
        case GP: return GPICK(ph);
        case GL: return GPICK(l);
        }
        return 0;
    }
    if (s == p + 1) {                            /* bogey */
        if (who == GC) return 336;
        if (who == GA) return 587;
        if (who == GP) return 809;
    }
    if (s == p + 2) {                            /* double bogey */
        if (who == GT) return 72;
        if (who == GC) return 337;
        if (who == GA) return 588;
        if (who == GP) return 810;
        if (who == GL) return 938;
    }
    /* other over-par results: the game's fail list for the character (0x800B9250 / 60 / 70 / 84 / 98) plus the
       restored announcer lines 1211 1212 1218 (anyone) and Angelica's 524 593 */
    static const uint16_t ft[] = { 1209, 1213, 1216, 79, 81, 78, 77, 12, 1211, 1212, 1218 },
                          fc[] = { 1209, 1213, 1216, 342, 343, 344, 346, 1211, 1212, 1218 },
                          fa[] = { 1209, 1213, 1216, 523, 525, 594, 595, 592, 596, 597, 1211, 1212, 1218, 524, 593 },
                          fp[] = { 1209, 1213, 1216, 743, 814, 815, 817, 818, 819, 1211, 1212, 1218 },
                          fl[] = { 1209, 1213, 1216, 942, 944, 946, 947, 1211, 1212, 1218 };
    switch (who) {
    case GT: return GPICK(ft);
    case GC: return GPICK(fc);
    case GA: return GPICK(fa);
    case GP: return GPICK(fp);
    case GL: return GPICK(fl);
    }
    return 0;
}
/* index of id in the loaded voice table (gp+0x76c ids, n at gp+0x76a), -1 = not loaded */
static int golf_table_index(unsigned id) {
    const uint32_t gp = cpu.r[28], ids = mem_r32(gp + 0x76c);
    const int n = (int16_t)mem_r16(gp + 0x76a);
    if (!ids || n <= 0 || n > 1024) return -1;
    for (int i = 0; i < n; i++) if ((mem_r16(ids + 2u * (uint32_t)i) & 0xfffu) == id) return i;
    return -1;
}
static int golf_voice_idle(void) { int16_t st = (int16_t)mem_r16(cpu.r[28] + 0x758); return st == 0 || st == 9; }
/* ask the game's voice player for id; 1 = it started */
static int golf_say(unsigned id) {
    const int i = golf_table_index(id);
    if (i < 0 || !golf_voice_idle()) {
        if (golf_log()) rt_logf("vox golf: say %u refused (table %d, state %d)\n", id, i, (int16_t)mem_r16(cpu.r[28] + 0x758));
        return 0;
    }
    const uint32_t played = mem_r32(cpu.r[28] + 0x778);
    if (played) mem_w8(played + (uint32_t)i, (uint8_t)(mem_r8(played + (uint32_t)i) & ~1u)); /* may repeat */
    uint32_t save[32], hi = cpu.hi, lo = cpu.lo;
    memcpy(save, cpu.r, sizeof save);
    cpu.r[4] = id;
    f_8008da4c();
    memcpy(cpu.r, save, sizeof save);
    cpu.hi = hi; cpu.lo = lo;
    if (golf_log()) rt_logf("vox golf: say %u (table %d) state now %d\n", id, i, (int16_t)mem_r16(cpu.r[28] + 0x758));
    return !golf_voice_idle();
}
static void golf_finish(uint32_t player) {           /* the original hole finish, its own voice requests muted */
    uint32_t save[32], hi = cpu.hi, lo = cpu.lo;
    memcpy(save, cpu.r, sizeof save);
    cpu.r[4] = player;
    golf_mute = 1;
    f_8007a9d8();
    golf_mute = 0;
    memcpy(cpu.r, save, sizeof save);
    cpu.hi = hi; cpu.lo = lo;
}
int rt_hook_8007a9d8(void) {
    if (golf_mute || !opt.voice_golf) return 0;
    const uint32_t pl = cpu.r[4];
    const int idx = golf_idx();
    if (idx < 0) return 0;
    const int who = (int16_t)mem_r16(pl + 0x15c), s = mem_r8(pl + 0x1b6 + (uint32_t)idx),
              p = mem_r8(0x800B7FFCu + (uint32_t)idx);
    const unsigned id = golf_pick(who, s, p, idx);
    int ok = 0, wait = 0;
    if (id && golf_table_index(id) >= 0) {
        if (golf_voice_idle()) ok = golf_say(id);
        else wait = 1;                                   /* a line still playing (ball rolling): ours right after it */
    }
    if (golf_log()) rt_logf("vox golf: hole %d par %d strokes %d character %d -> line %u%s vbl %u\n", idx + 1, p, s, who,
                            id, !id ? " (none)" : ok ? " (holding)" : wait ? " (holding, after the line playing)" : " (not started)",
                            hooks_frame());
    if (!ok && !wait) return 0;                          /* nothing to say: the game goes on as usual */
    golf_hold = pl;
    golf_hold_id = wait ? id : 0;
    golf_hold_left = 3000;                               /* 10 s safety (longest golf line 5.3 s, + a ball line) */
    golf_hold_tail = 60;                                 /* 0.2 s after the line before the screen changes */
    golf_ball_left = -1;
    return 1;
}
/* every gameplay frame (from rt_hook_800818e0, before the level chatter); 1 = skip the chatter this frame */
static int golf_frame(int32_t dt) {
    if (dt <= 0 || dt >= 3000) dt = 0;
    if (golf_hold) {
        golf_hold_left -= dt;
        if (golf_hold_id) {                              /* waiting for the line that was playing */
            if (golf_hold_left > 0 && !golf_voice_idle()) return 1;
            const unsigned id = golf_hold_id;
            golf_hold_id = 0;
            if (golf_hold_left <= 0 || !golf_say(id)) golf_hold_tail = 0;
            else if (golf_log()) rt_logf("vox golf: line %u after the one playing vbl %u\n", id, hooks_frame());
        } else if (golf_voice_idle()) golf_hold_tail -= dt;
        if (golf_hold_tail > 0 && golf_hold_left > 0) return 1;
        const uint32_t pl = golf_hold;
        golf_hold = 0;
        if (golf_log()) rt_logf("vox golf: release%s vbl %u\n", golf_hold_left <= 0 ? " (timeout)" : "", hooks_frame());
        golf_finish(pl);
        return 1;
    }
    if (golf_ball_left < 0) return 0;
    if (!opt.voice_golf || golf_idx() < 0) { golf_ball_left = -1; return 0; }
    golf_ball_left -= dt;
    if (golf_ball_left > 0) return 0;
    golf_ball_left = -1;
    static const uint16_t bt[] = { 270, 82 }, bc[] = { 347 }, ba[] = { 598 }, bl[] = { 949, 995 }, bp[] = { 820, 868 };
    const int who = (int16_t)mem_r16(0x800E2B48u + 0x15c);
    const uint16_t *list = who == GT ? bt : who == GC ? bc : who == GA ? ba : who == GL ? bl : who == GP ? bp : NULL;
    const int n = (who == GT || who == GL || who == GP) ? 2 : list ? 1 : 0;
    if (!n || !golf_voice_idle()) return 0;
    const uint32_t played = mem_r32(cpu.r[28] + 0x778);
    unsigned cand[2]; int nc = 0;
    for (int pass = 0; pass < 2 && !nc; pass++)          /* unheard first, then any but the last one */
        for (int k = 0; k < n; k++) {
            const int i = golf_table_index(list[k]);
            if (i < 0) continue;
            if (pass == 0 && played && (mem_r8(played + (uint32_t)i) & 1u)) continue;
            if (pass == 1 && n > 1 && list[k] == golf_ball_last) continue;
            cand[nc++] = list[k];
        }
    if (!nc) return 0;
    const unsigned id = cand[golf_rand() % (uint32_t)nc];
    const int ok = golf_say(id);
    if (ok) golf_ball_last = id;
    if (golf_log()) rt_logf("vox golf: ball rolling, character %d -> line %u%s vbl %u\n", who, id, ok ? "" : " (not started)",
                            hooks_frame());
    return 0;
}
void f_8005fcb8(void);
int rt_hook_8005fcb8(void) {
    static int inside;
    if (inside) return 0;
    const uint32_t pl = cpu.r[4];
    const unsigned before = mem_r8(pl + 0x1bf);
    inside = 1;
    f_8005fcb8();                                        /* the original */
    inside = 0;
    if (opt.voice_golf && golf_idx() >= 0 && !golf_hold && mem_r8(pl + 0x1bf) != before && golf_rand() % 3u == 0) {
        golf_ball_left = 90;                             /* 0.3 s: the ball is rolling */
        if (golf_log()) rt_logf("vox golf: swing, ball line armed vbl %u\n", hooks_frame());
    }
    return 1;
}

/* ---- Circus Angelicus: Phil's "landed on the target" line 762 (part of Restored Lines, user 2026-11-18) ----------
   Trike act (stage byte gp+0xd50: 9 ride, 2 landing check 80058998 = surface flags & 3 == 1 under the trike, 3
   result): on the target (player +248 = object type 812) stage 3 calls 8007db6c EVERY frame, which queues event 57
   -> 8007dbf4 (player swap to object 600, object 508's callback starts a cinematic from CINEPAKA.BIN, which takes
   the CD drive at once, event 58 -> diving board, stage 5); miss -> event 56, ride again. The game says nothing
   there; 762 has no call site, list or trigger (HANDOFF). Here: the first 8007db6c call starts 762 (or waits for a
   ride line 743 766 804 still playing) and is skipped (circus_hold). While it plays, stage 3 keeps running each
   frame: both its outcomes are skipped, 8007db6c (hit) and 8007db90 (miss, event 56: the hit must not turn into a
   miss if the object under the trike changes meanwhile). When the voice is idle again (or after 5 s) we call the
   original 8007db6c once ourselves (event 57 queued, the game goes on as usual); further calls of either stay
   skipped until the stage leaves 3 (circus_done). Off with Restored Lines Off. Not in random chatter
   (restore_allowed, db 11). */
void f_8007db6c(void);
static int circus_hold, circus_say, circus_done, circus_inside;  /* holding / 762 still to start / result queued */
static int32_t circus_left;
static uint32_t circus_player;
static void circus_state(StateIO *s) {
    ST(s, circus_hold); ST(s, circus_say); ST(s, circus_done); ST(s, circus_left); ST(s, circus_player);
}
static void circus_frame(int32_t dt) {
    if (!circus_hold && !circus_done) return;
    if (mem_r32(0x800C5678u) != 11) { circus_hold = circus_say = circus_done = 0; return; }
    if (circus_done) { if (mem_r8(0x800C4BD0u) != 3) circus_done = 0; return; }
    if (dt > 0 && dt < 3000) circus_left -= dt;
    if (circus_say && golf_voice_idle()) {
        circus_say = 0;
        if (golf_say(762)) { if (golf_log()) rt_logf("vox circus: 762 (target, after the ride line) vbl %u\n", hooks_frame()); return; }
    }
    if ((!circus_say && golf_voice_idle()) || circus_left <= 0) {
        if (golf_log()) rt_logf("vox circus: release%s vbl %u\n", circus_left <= 0 ? " (timeout)" : "", hooks_frame());
        circus_hold = circus_say = 0; circus_done = 1;
        uint32_t save[32], hi = cpu.hi, lo = cpu.lo;
        memcpy(save, cpu.r, sizeof save);
        cpu.r[4] = circus_player;
        circus_inside = 1;
        f_8007db6c();                                    /* the original: queues event 57 (success) */
        circus_inside = 0;
        memcpy(cpu.r, save, sizeof save);
        cpu.hi = hi; cpu.lo = lo;
    }
}
int rt_hook_8007db6c(void) {
    if (circus_inside || !opt.voice_restore || mem_r32(0x800C5678u) != 11) return 0;
    if (circus_hold || circus_done) return 1;
    if (golf_table_index(762) < 0) return 0;
    circus_player = cpu.r[4];
    circus_hold = 1; circus_left = 1500;                     /* 5 s safety (762 is 1.7 s) */
    circus_say = 1;
    if (golf_voice_idle()) {
        circus_say = 0;
        if (!golf_say(762)) { circus_hold = 0; return 0; }   /* refused: go on as usual */
        if (golf_log()) rt_logf("vox circus: 762 (target) vbl %u\n", hooks_frame());
    } else if (golf_log()) rt_logf("vox circus: 762 waits for the playing line vbl %u\n", hooks_frame());
    return 1;
}
int rt_hook_8007db90(void) {                                 /* trike missed (event 56): not while a hit is held */
    if (circus_inside || !opt.voice_restore || mem_r32(0x800C5678u) != 11) return 0;
    if (circus_hold || circus_done) { if (golf_log()) rt_logf("vox circus: miss skipped (hit held)\n"); return 1; }
    return 0;
}

/* debug: RUGRATS_SFXLOG=1 logs every call of the game's sound-effect wrappers and the libsnd key-on (args, caller) */
static void sfx_log(const char *fn) {
    static int on = -1;
    if (on < 0) on = getenv("RUGRATS_SFXLOG") != NULL;
    if (!on) return;
    uint32_t sp = cpu.r[29];
    rt_logf("sfx %s a %x %x %x %x st %x %x %x %x ra %08x vbl %u\n", fn, cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7],
            mem_r32(sp + 0x10), mem_r32(sp + 0x14), mem_r32(sp + 0x18), mem_r32(sp + 0x1c), cpu.r[31], hooks_frame());
}
/* Reptar bar "Chime only": set by rt_hook_8006f26c for its own SsUtKeyOnV call (the next one; a0 = voice) */
static int bar_cut_armed, bar_cut_at, bar_cut_fade;
static uint32_t bar_cut_vbl;   /* only a key-on in the same vblank (the call itself) counts */
void spu_cut_next(int voice, int samples, int fade);
int rt_hook_800a99a8(void) {
    sfx_log("keyonv");
    if (bar_cut_armed && hooks_frame() == bar_cut_vbl) { bar_cut_armed = 0; spu_cut_next((int)cpu.r[4], bar_cut_at, bar_cut_fade); }
    return 0;
}
/* Reptar bar pickup (opt.bar_roar): 800517a8 (bar collected: sound, then the HUD counter) plays sound 0x1c through
   8006f26c (program 15, sample at SPU 0x61b40). That one sample is the chime (rising tones, first ~0.42 s) followed
   by the roar (~1.2 s of noise). opt.bar_roar: 1 chime + roar (original); 0 chime only: let it start and have the
   SPU fade that voice out from 0.42 s to 0.43 s (the voice is taken from the SsUtKeyOnV call this sound makes next:
   rt_hook_800a99a8; the sample's SPU address differs per level) (user's pick after trying 0.40 / 0.415 / 0.45 s; RUGRATS_BARCUT=
   <start ms>,<fade ms> to try others); 2 no sound: skip the call (its return value is not used there).
   (8007013c from 8005ea28, program 0 notes 0x1b / 0x19 around the same time, is a different sound.) */
int rt_hook_8006f26c(void) {
    sfx_log("f26c");
    if (opt.bar_roar != 1 && cpu.r[4] == 0x1c && cpu.r[31] == 0x800517b0u) {
        if (opt.bar_roar == 2) return 1;
        static int at = -1, fade;
        if (at < 0) { at = 420; fade = 10; const char *e = getenv("RUGRATS_BARCUT"); if (e) sscanf(e, "%d,%d", &at, &fade); }
        bar_cut_armed = 1; bar_cut_vbl = hooks_frame(); bar_cut_at = at * 441 / 10; bar_cut_fade = fade * 441 / 10;
    }
    return 0;
}
/* After Last Piece (part of opt.final_level): 800781f8(obj, db) queues a message (type 3) whose handler 800771d4 writes db to
   0x800C4918, so the level loop exits to that database. When the 12th puzzle piece is earned / placed the game sends
   db 7 (final level) from the bar pickup 800517a8 (ra 8005180c, via 80094978 = all 12 pieces) or from the end of the
   puzzle screen 80092c68 (ra 80092d7c, 80091d28() == 12). The puzzle and its "You did it!" screen run inside that
   frame, so dropping the message just lets the level carry on from where the bar was taken. Other db 7 senders
   (80018364, 8004d414) are left alone. */
/* Debug (RUGRATS_ENTLOG=1): hub level entrances. 800631d8(player) = Triangle target finder (sets player+244);
   8004f78c(obj, player) = entrance handler (obj+108 = db). Logs only, behaviour unchanged. */
static int entlog = -1;
static int entlog_on(void) { if (entlog < 0) entlog = getenv("RUGRATS_ENTLOG") != 0; return entlog; }
void f_800631d8(void);
int rt_hook_800631d8(void) {
    static int inside;
    if (inside || !entlog_on()) return 0;
    uint32_t p = cpu.r[4], ra = cpu.r[31], before = mem_r32(p + 244);
    inside = 1;
    f_800631d8();
    inside = 0;
    uint32_t t = mem_r32(p + 244);
    rt_logf("ent: finder ra %08x player %08x target %08x -> %08x (cand %08x n %d) model %d\n", ra, p, before, t,
            mem_r32(p + 256), mem_r8(p + 255), t ? mem_r16(t + 150) : -1);
    return 1;
}
int rt_hook_8004f78c(void) {
    if (!entlog_on()) return 0;
    uint32_t o = cpu.r[4];
    rt_logf("ent: entrance handler obj %08x model %d db %d sub %d ra %08x\n", o, mem_r16(o + 150), mem_r8(o + 108),
            mem_r8(o + 106), cpu.r[31]);
    return 0;
}
int rt_hook_800781f8(void) {
    if (cpu.r[5] != 7) return 0;
    uint32_t ra = cpu.r[31];
    rt_logf("level: go-to-db-7 message from ra %08x (db %d)%s\n", ra, hooks_level,
            opt.final_level && (ra == 0x8005180cu || ra == 0x80092d7cu) ? " dropped: Final Level Behavior = Enhanced" : "");
    if (opt.final_level && (ra == 0x8005180cu || ra == 0x80092d7cu)) return 1;
    return 0;
}
/* "Final level unlocked!" screen (part of opt.final_level): 80093048 = one
   puzzle message screen (fade in, wait for X, fade out). Its caller 80094978 (all 12 pieces, ra 800949d0) first sets
   up the "You did it!" text with 80094178: three line objects (0x2c bytes each) at 0x800E7300 (yellow, pink, pink)
   and their drop shadows at 0x800E7390 (x + 2, y + 2); +0x14 = string, +8 = x, +0xa = y; 8002e144(obj, 1) centres
   a line. After that screen we point the lines at our two strings, hide the third and run 80093048 again.
   The strings borrow 64 bytes of memory-card / "You did it!" text at 0x80015EBC (unused here), put back after. */
void f_80093048(void);
void f_8002e144(void);
int rt_hook_80093048(void) {
    static int inside;
    uint32_t ra = cpu.r[31];
    if (inside || ra != 0x800949d0u || !opt.final_level) return 0;
    inside = 1;
    f_80093048();                                              /* "You did it!" */
    const uint32_t T = 0x800E7300u, SH = 0x800E7390u, STR = 0x80015EBCu, L = 0x2cu;
    static const char top[] = "Final level unlocked!", bot[] = "Go see what's on TV!";
    uint8_t keep[64];
    for (int i = 0; i < 64; i++) keep[i] = mem_r8(STR + i);
    uint32_t a_top = STR, a_bot = STR + sizeof top, a_none = a_bot + sizeof bot;
    for (unsigned i = 0; i < sizeof top; i++) mem_w8(a_top + i, (uint8_t)top[i]);
    for (unsigned i = 0; i < sizeof bot; i++) mem_w8(a_bot + i, (uint8_t)bot[i]);
    mem_w8(a_none, 0);
    uint32_t okeep[2][3][3];                                   /* string, x, y of each line + shadow */
    for (int s = 0; s < 2; s++)
        for (int n = 0; n < 3; n++) {
            uint32_t o = (s ? SH : T) + n * L;
            okeep[s][n][0] = mem_r32(o + 0x14); okeep[s][n][1] = mem_r16(o + 8); okeep[s][n][2] = mem_r16(o + 0xa);
        }
    uint32_t str[3] = { a_top, a_bot, a_none };
    int16_t y[3] = { -53, -28, -15 };                          /* two lines centred where the three were (-65..-15) */
    uint32_t save[32];
    memcpy(save, cpu.r, sizeof save);
    for (int n = 0; n < 3; n++) {
        uint32_t o = T + n * L, sh = SH + n * L;
        mem_w32(o + 0x14, str[n]); mem_w32(sh + 0x14, str[n]);
        mem_w16(o + 0xa, (uint16_t)y[n]); mem_w16(sh + 0xa, (uint16_t)(y[n] + 2));
        if (n < 2) { cpu.r[4] = o; cpu.r[5] = 1; cpu.r[31] = 0x800949d0u; f_8002e144(); }
        mem_w16(sh + 8, (uint16_t)(mem_r16(o + 8) + 2));
    }
    memcpy(cpu.r, save, sizeof save);
    rt_logf("puzzle: all pieces -> extra screen \"%s / %s\"\n", top, bot);
    cpu.r[31] = ra;
    f_80093048();                                              /* "Final level unlocked! / Go see what's on TV!" */
    for (int s = 0; s < 2; s++)
        for (int n = 0; n < 3; n++) {
            uint32_t o = (s ? SH : T) + n * L;
            mem_w32(o + 0x14, okeep[s][n][0]); mem_w16(o + 8, (uint16_t)okeep[s][n][1]); mem_w16(o + 0xa, (uint16_t)okeep[s][n][2]);
        }
    for (int i = 0; i < 64; i++) mem_w8(STR + i, keep[i]);
    inside = 0;
    return 1;
}
/* Walk After Shooting (opt.shot_walk): 80057214(obj, speed) stores the animation speed obj+0xe4 (4096 = 1x, clamped
   2048..16384); the keyframe step 80055d40 uses ticks * e4 >> 12 for both animation layers (anim ids obj+0xa8,
   obj+0xb4). Firing while walking (Angelica's laser, Triangle) sets anim 0x27 at 0x2000 through 800572a8 (from
   8005a7a4; the store has ra 800572fc and the anim in s1); the layers play anim 0x32 and then go back to the walk
   anims without touching e4, so the walk goes on at 2x until the player stops (a state change through 8005b2d8 sets
   0x1000 again). Same in PCSX-Redux, i.e. the original game. Fixed (1): the walk body hook below puts the walk's
   own speed back (0x800 for model 8, else 0x1000, as 8005a068 picks) on the first walking frame where neither layer
   plays 0x27 / 0x32 any more. RUGRATS_SHOTWALK=0/1, RUGRATS_ANIMSPEEDLOG=1 logs the player's speed writes. */
#define ANIM_PLAYER 0x800E2B48u
static int shot_walk_pending;   /* the player's last speed write was a shot's 0x2000 */
int rt_hook_80057214(void) {
    static int logon = -1;
    if (logon < 0) logon = getenv("RUGRATS_ANIMSPEEDLOG") != NULL;
    if (cpu.r[4] != ANIM_PLAYER) return 0;
    if (logon) rt_logf("animspeed: %d ra %08x s1 %d state %d vbl %u\n", (int32_t)cpu.r[5], cpu.r[31],
                       (int16_t)cpu.r[17], mem_r8(cpu.r[4] + 397), hooks_frame());
    shot_walk_pending = cpu.r[31] == 0x800572fcu && (int16_t)cpu.r[17] == 0x27 && cpu.r[5] == 0x2000;
    return 0;
}
static void shot_walk_check(uint32_t obj) {
    if (!shot_walk_pending || obj != ANIM_PLAYER) return;
    if (!opt.shot_walk) { shot_walk_pending = 0; return; }
    const int a = (int16_t)mem_r16(obj + 0xa8), b = (int16_t)mem_r16(obj + 0xb4);
    if (a == 0x27 || a == 0x32 || b == 0x27 || b == 0x32) return;   /* shot still playing */
    shot_walk_pending = 0;
    if ((int16_t)mem_r16(obj + 0xe4) != 0x2000) return;
    mem_w16(obj + 0xe4, mem_r16(obj + 0x96) == 8 ? 0x800 : 0x1000);
    if (getenv("RUGRATS_ANIMSPEEDLOG")) rt_logf("animspeed: walk after shot -> %d vbl %u\n", (int16_t)mem_r16(obj + 0xe4), hooks_frame());
}
/* Wall Bonk (opt.wall_bonk): the player's walk state body 8005e1f4 (only caller: state 2 handler 8005ec7c) asks for
   state 9 = bonk (byte obj+396; anim 6, sound 8 for Tommy, ~0.4 s without control; handler 8005d030) when the player
   hits a wall with the stick still held (request sites 8005e530 / 8005e69c, the only ones in the game). Off: that
   request becomes 0 (idle), which the game itself picks for a soft stop; holding into the wall from idle stays idle. */
void f_8005e1f4(void);
int rt_hook_8005e1f4(void) {
    static int inside;
    if (!inside) shot_walk_check(cpu.r[4]);              /* Walk After Shooting */
    if (inside || opt.wall_bonk) return 0;
    uint32_t obj = cpu.r[4];
    inside = 1;
    f_8005e1f4();                                        /* the original */
    inside = 0;
    if (mem_r8(obj + 396) == 9) mem_w8(obj + 396, 0);
    return 1;
}
/* Player walk step 8005e134(obj), called by the walk body every frame: adds dir * step to this frame's velocity
   (obj+216/218/220, int16; reset every frame), with dir = VectorNormal(obj+376..384) (8009f278, 4096 = 1) and
   step = trunc(speed obj+388 * ticks * 400 / 4096), each axis then truncated again (80076944: v * step >> 12, rounded
   toward 0). Fine at 30 fps (10 ticks), but at high fps the per-frame steps are small and the rounding eats them: in
   the soap (speed 17) Tommy walked 454 u/s at 30 fps, 424 at 60, 300 at 144, 120 at 240 (PCSX-Redux: ~463). Normal
   walking lost ~1-2% at 120 fps the same way. Native version: the 30 fps step (10 ticks, the game's own rounding),
   times ticks / 10 per frame, with the remainder carried per object, so every frame rate moves exactly like 30 fps.
   RUGRATS_WALKSTEP=0 = original (A/B). */
void f_8009f278(void);
int rt_hook_8005e134(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("RUGRATS_WALKSTEP"); on = e ? atoi(e) != 0 : 1; }
    if (!on) return 0;
    static struct { uint32_t obj; int32_t rem[3]; } acc[8];
    const uint32_t obj = cpu.r[4], sp = cpu.r[29], ra = cpu.r[31];
    const int32_t ticks = (int32_t)mem_r32(cpu.r[28] + 156);
    int32_t step = (int32_t)mem_r32(obj + 388) * 10 * 400;   /* the original step at 10 ticks */
    if (step < 0) step += 4095;
    step >>= 12;
    /* dir = VectorNormal(obj+376) into free stack below sp (the guest's own routine, same rounding) */
    const uint32_t tmp = sp - 0x40;
    cpu.r[4] = obj + 376; cpu.r[5] = tmp; cpu.r[29] = sp - 0x80;
    f_8009f278();
    cpu.r[29] = sp; cpu.r[31] = ra;
    int k = 0;
    while (k < 8 && acc[k].obj != obj && acc[k].obj) k++;
    if (k == 8) { k = 0; }
    if (acc[k].obj != obj) { acc[k].obj = obj; acc[k].rem[0] = acc[k].rem[1] = acc[k].rem[2] = 0; }
    for (int i = 0; i < 3; i++) {
        int32_t c = (int32_t)mem_r32(tmp + 4 * i) * step;
        if (c < 0) c += 4095;
        c >>= 12;                                            /* this axis' 30 fps step, game rounding */
        int32_t t = c * ticks + acc[k].rem[i];               /* tenths of it per frame, remainder kept */
        int32_t d = t / 10;
        acc[k].rem[i] = t - d * 10;
        const uint32_t va = obj + 216 + 2 * i;
        mem_w16(va, (mem_r16(va) + (uint32_t)d) & 0xFFFF);
    }
    return 1;
}
/* Streamed in-engine cutscenes: 8007412c (once per cutscene frame) waits for the CD callback 80019c88's flag gp+104,
   takes the packet at gp+96 and returns 1 = "cutscene over" if its frame number (halfword +0) is below the current
   frame gp+3508 (or the packet is 0xDDDDDDDD). OPEN.BIN (opening cinematic, 1412 frames) has empty padding sectors
   (submode 0, all zero, on the filter's channel: LBA 27, 29-31) between the first frames. A PS1 renders slowly enough
   that a real frame sector has replaced them by its next fetch (PCSX-Redux plays all 1412 frames); our fast loop
   fetched one at once, read it as frame 0 < 1 and ended the cinematic after one frame. So: a ready packet whose first
   8 bytes are zero is dropped (flag cleared) and we keep advancing time until a real one arrives. A real packet never
   starts with 8 zero bytes (frame number + size, or a header magic). RUGRATS_CUTPAD=0 = old behaviour.
   Debug: RUGRATS_CUTLOG=1 logs each packet taken (frame number vs counter) and each new stream header. */
void f_8007412c(void);
int rt_hook_8007412c(void) {
    static int on = -1, pad = -1, inside;
    if (on < 0) on = getenv("RUGRATS_CUTLOG") != NULL;
    if (pad < 0) { const char *e = getenv("RUGRATS_CUTPAD"); pad = e ? atoi(e) != 0 : 1; }
    cut_last_vbl = hooks_frame(); cut_seen = 1;   /* cutscene_prop(): a streamed cutscene is running */
    if (inside || (!on && !pad)) return 0;
    uint32_t gp = cpu.r[28];
    uint32_t cnt0 = mem_r16(gp + 3508);
    if (pad) {
        uint64_t budget = (uint64_t)CPU_HZ * 2, spent = 0;   /* at most 2 s of game time, then let the original run */
        unsigned dropped = 0;
        for (;;) {
            if (mem_r8(gp + 104)) {
                uint32_t p = mem_r32(gp + 96);
                if (!p || mem_r32(p) != 0 || mem_r32(p + 4) != 0) break;   /* a real packet: hand it over */
                mem_w8(gp + 104, 0);                                         /* padding sector: drop, wait on */
                dropped++;
                continue;
            }
            if (spent >= budget) break;
            uint32_t c0 = cpu.cycles;
            if ((int32_t)(cpu.next_event - cpu.cycles) > 0) cpu.cycles = cpu.next_event;
            else cpu.cycles += 64;
            rt_event();
            spent += (uint32_t)(cpu.cycles - c0);
        }
        if (on && dropped) rt_logf("cut: dropped %u padding packet(s) at frame %u vbl %u\n", dropped, cnt0, hooks_frame());
    }
    inside = 1; f_8007412c(); inside = 0;
    if (!on) return 1;
    uint32_t p = mem_r32(gp + 3520) - 24, v0 = cpu.r[2];
    rt_logf("cut pkt %08x w0 %08x w1 %08x frame %u cnt %u -> %u ret %u vbl %u\n", p, mem_r32(p), mem_r32(p + 4),
            mem_r16(p), cnt0, mem_r16(gp + 3508), v0, hooks_frame());
    {   static uint32_t lasth; uint32_t h = mem_r32(gp + 100); if (cnt0 == 0 && h != lasth) { lasth = h;
            rt_logf("cut len gp+1376 %d gp+1384 %d gp+1380 %d hdr@%08x %08x %08x %08x\n", (int16_t)mem_r16(gp + 1376),
                    (int16_t)mem_r16(gp + 1384), mem_r32(gp + 1380), h, mem_r32(h), mem_r32(h + 4), mem_r32(h + 8)); } }
    cpu.r[2] = v0;
    return 1;
}
int rt_hook_8006f2fc(void) { sfx_log("f2fc"); return 0; }
int rt_hook_8006f3a4(void) { sfx_log("f3a4"); return 0; }
int rt_hook_8006f438(void) { sfx_log("f438"); return 0; }
/* Landing stomp (frame-rate bug): the player's land state (state 10, entry 8005b624) sets the per-frame callback
   obj+0x658 = 8005cba8, which in the Reptar 2010 level (0x800C5680 == 9) plays stomp sound 0x5d (ra 8005cbfc) plus a
   camera shake EVERY game frame of the landing (~0.4-0.8 s; each call keys on 2 voices). PCSX-Redux (2026-11-10,
   user's slot 3 save): this level runs ~8 game fps on a PS1 (a frame every ~7.5 vblanks), so a landing plays it 3
   times. The port played it 10x at 30 fps and 50x at 120 fps, piling up on the voices = extremely loud. Now at most
   one per 38 ticks (1/300 s; = 7.5 vblanks) of game time, the first frame of a landing always plays.
   RUGRATS_STOMPFIX=0 = original, 2 = first stomp of a landing only (debug); RUGRATS_STOMPGAP=<ticks>. */
int rt_hook_8006f644(void) {
    sfx_log("f644");
    static int on = -1, gap, acc;
    static uint32_t last_fc = 0xFFFFFFFFu;
    if (on < 0) {
        const char *e = getenv("RUGRATS_STOMPFIX"); on = e ? atoi(e) : 1;
        e = getenv("RUGRATS_STOMPGAP"); gap = e ? atoi(e) : 38; if (gap < 1) gap = 1;
    }
    {   /* debug: RUGRATS_SFXSKIP=<hex id>,... mutes those sound ids (this wrapper only) */
        static int nskip = -1; static unsigned skip[16];
        if (nskip < 0) { nskip = 0; const char *e = getenv("RUGRATS_SFXSKIP");
            while (e && *e && nskip < 16) { skip[nskip++] = (unsigned)strtoul(e, (char **)&e, 16); if (*e == ',') e++; else break; } }
        for (int i = 0; i < nskip; i++) if (cpu.r[4] == skip[i]) { cpu.r[2] = 0; return 1; }
    }
    if (!on || cpu.r[4] != 0x5d || cpu.r[31] != 0x8005cbfcu) return 0;
    uint32_t fc = mem_r32(0x800C3F24u);              /* game frame counter, +1 per game frame */
    int ticks = (int32_t)mem_r32(0x800C3F1Cu);       /* gp+156: this frame's ticks of 1/300 s */
    if (fc != last_fc + 1 && fc != last_fc) acc = gap; /* new landing: play now */
    else if (fc != last_fc) acc += ticks;
    last_fc = fc;
    if (acc >= gap) {
        if (on == 2) acc = -1000000;
        else { acc -= gap; if (acc > gap) acc = gap; }
        return 0;
    }
    cpu.r[2] = 0;                                    /* skipped (return value unused at 8005cbfc) */
    return 1;
}
int rt_hook_80070064(void) { sfx_log("0064"); return 0; }
int rt_hook_8007013c(void) { sfx_log("013c"); return 0; }

/* ---- particle spawn (8008864c, a0 = descriptor) ----------------------------------------------------------
   Goose feathers on the shuffleboard (database 2, Grandpa's Teeth): the goose's emitter (call at 8005389c ->
   helper 80052b60 -> 8008864c, descriptor type 0x505) runs from a 1/30 s timer and spawns one feather per fire. The
   timer fires at most once per game frame, and the PS1 (PCSX-Redux, 2026-11-14) runs that scene at ~20.7 game fps
   (frames of 15 and 10 ticks), so it showed 20.7 feathers/s (~21 on screen); the port fired every 10 ticks at any
   frame rate = 30/s. Fix: spawn only once 14.5 ticks (1/20.7 s) of game time have built up since the last feather
   (the ticks the timer pass hands the callback); skipped calls return 0 (the helper ignores the result).
   RUGRATS_FEATHERS=0 = old 30/s, RUGRATS_PARTLOG=1 logs every particle spawn. */
int rt_hook_8008864c(void) {
    static int lg = -1, on = -1;
    if (lg < 0) lg = getenv("RUGRATS_PARTLOG") != NULL;
    if (on < 0) { const char *e = getenv("RUGRATS_FEATHERS"); on = !(e && *e == '0'); }
    const uint32_t ticks = mem_r32(0x800C3E80u + 156);
    const int32_t db = (int32_t)mem_r32(0x800C5678u);
    int skip = 0;
    if (on && db == 2 && cpu.r[31] == 0x80052b90u && mem_r32(cpu.r[29] + 0x18u) == 0x800538a4u) {
        const double ref = 300.0 / 20.7;
        feather_acc += ticks > 40 ? 40 : ticks;
        if (feather_acc < ref) skip = 1;
        else { feather_acc -= ref; if (feather_acc > ref) feather_acc = 0; }   /* no bursts after a pause */
    }
    if (lg) rt_logf("part f%u ra %08x up %08x desc %08x type %08x ticks %u db %d%s\n", present_count, cpu.r[31],
                    mem_r32(cpu.r[29] + 0x18u), cpu.r[4], mem_r32(cpu.r[4]), ticks, (int)db, skip ? " skip" : "");
    if (skip) { cpu.r[2] = 0; return 1; }
    return 0;
}

/* ---- Ceiling fan off in Let There Be Light (2026-11-18, user) ------------------------------------------------------
   The level's story is a power cut, but the kitchen ceiling fan (obj 800eef6c in the user's save, model 94 = obj+0x96)
   kept turning. It is a generic spinning prop: timer callback 8004e2ac (obj+0x44, 1/30 s) adds the s16 speeds
   obj+0x2c/+0x2e/+0x30 (0x20 per vblank around y for the fan) x vblanks since the last call (VSync(-1) - obj+0x64) to the
   angles obj+0x24/+0x26/+0x28. Here, only in db 12 and only for model 94, the speeds are zeroed before the callback
   runs, so the blades keep their spawn angle. Every other level and every other spinning prop is untouched.
   F1 Enhancements "Ceiling Fan" (opt.fan_still, ini [game] fan_still, RUGRATS_FANSTILL): switched back to Original
   while the level runs, the fan gets its speed back (the game's value seen before zeroing; 32 around y if this run
   never saw it, e.g. after loading a state saved with the fan stopped). */
#define FAN_DB    12
#define FAN_MODEL 94u
int rt_hook_8004e2ac(void) {
    static int16_t spd[3] = {32, 0, 0};   /* the fan's own speeds (last seen non-zero) */
    const uint32_t obj = cpu.r[4];
    static int lg = -1;   /* debug: RUGRATS_FANLOG=1 logs each spinner call (db, obj, model, speeds, angles) */
    if (lg < 0) lg = getenv("RUGRATS_FANLOG") != NULL;
    if (lg)
        rt_logf("spin f%u db %d obj %08x model %u speed %d,%d,%d angle %u,%u,%u\n", present_count,
                (int)mem_r32(0x800C5678u), obj, mem_r16(obj + 0x96), (int16_t)mem_r16(obj + 0x2c),
                (int16_t)mem_r16(obj + 0x2e), (int16_t)mem_r16(obj + 0x30), mem_r16(obj + 0x24), mem_r16(obj + 0x26),
                mem_r16(obj + 0x28));
    if ((int32_t)mem_r32(0x800C5678u) == FAN_DB && (obj >> 24) == 0x80u && mem_r16(obj + 0x96) == FAN_MODEL) {
        const int moving = mem_r16(obj + 0x2c) | mem_r16(obj + 0x2e) | mem_r16(obj + 0x30);
        if (opt.fan_still) {
            if (moving)
                for (int i = 0; i < 3; i++) spd[i] = (int16_t)mem_r16(obj + 0x2c + 2 * i);
            mem_w16(obj + 0x2c, 0); mem_w16(obj + 0x2e, 0); mem_w16(obj + 0x30, 0);
        } else if (!moving) {
            for (int i = 0; i < 3; i++) mem_w16(obj + 0x2c + 2 * i, (uint16_t)spd[i]);
        }
    }
    return 0;
}
