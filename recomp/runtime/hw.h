#pragma once
#include <stdint.h>
#include <stdio.h>
#include "cpu.h"

/* Virtual clock. clk_mul > 1 "overclocks" the guest CPU: every device timing is multiplied,
   so the game gets clk_mul times more CPU time per real second while video/audio/CD stay real-time. */
extern uint32_t clk_mul;
#define CPU_HZ (33868800u * clk_mul)
#define CYCLES_PER_FRAME (564480u * clk_mul)    /* 263 lines * 2146 cycles, ~60 Hz */
#define CYCLES_PER_LINE (2146u * clk_mul)
extern uint32_t present_count;  /* frames shown so far */
/* frames presented per second right now: opt.fps, or lower while the PC can't keep up (set by the host) */
extern int present_fps;
#define PRESENT_FPS (present_fps > 0 ? present_fps : opt.fps)

enum { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5,
       IRQ_TMR2 = 6, IRQ_PAD = 7, IRQ_SIO = 8, IRQ_SPU = 9 };

extern uint32_t i_stat, i_mask;
void irq_raise(int n);
void irq_check(void);

/* scheduler: absolute cycle deadlines; 0 = unused */
enum { EV_VBLANK, EV_CDROM, EV_DMA, EV_SPU, EV_SIO, EV_PRESENT, EV_COUNT };
void sched(int ev, uint32_t delay);
void sched_cancel(int ev);
void sched_update(void);

/* logging */
extern FILE *rt_log;
void rt_logf(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);

/* devices */
void gpu_init(void);
void gpu_gp0(uint32_t v);
void gpu_gp1(uint32_t v);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void gpu_vblank(void);
/* sub-pixel vertices: the GTE records the fraction it drops from each projected X/Y, keyed by the packed
   SXY word the game copies into GPU packets; the GPU looks vertex words up for hi-res drawing */
int32_t gte_ws_x(int32_t x);                          /* widescreen squeeze of a camera-space x */
/* also the vertex depth for the hi-res depth check (iz = 4096 / camera z, < 0 = none) and its kind (gte_dcls) */
void subpx_put(uint32_t sxy, int has_fx, int fx, int fy, float iz, int cls); /* fractions in 1/256 pixel */
int subpx_get(uint32_t sxy, int *fx, int *fy);
int subpx_z(uint32_t sxy, int cls, float *iz);         /* depth of a cls point on that pixel */
void subpx_mid(uint32_t a, uint32_t b, uint32_t m);   /* m = 2D midpoint of a and b: average their depths */
void subpx_put3(uint32_t sxy, double x, double y, double z); /* camera-space room vertex (gpu.c nearplane) */
void subpx_putg(uint32_t sxy, float iz);             /* depth of any sub-pixel point (gpu.c poly_grow) */
void subpx_world(uint32_t sxy);                       /* room / object point without usable depth (HUD protection) */
enum { DCLS_NONE, DCLS_ROOM, DCLS_OBJ };
extern uint32_t gte_world_n;                           /* GTE points of room geometry since the frame's first layer (gte.c; Stretch 2D) */
extern int gpu_world_frame;                            /* 1 = the frame being drawn shows the 3D world (hw.c layer_note) */
extern int gte_near_sort;                              /* 1 = SZ at 1/4: the game sorts the faces near the front (gte.c; hooks.c dark_panel) */
extern int gte_dcls;                                   /* kind of geometry the GTE projects now (hooks.c) */
extern uint32_t gte_backface_frame;                    /* present_count when room back faces are drawn (hooks.c, eye over the wall) */
void gpu_backface_note(const int16_t sxy[3][2]);        /* gte.c NCLIP: this screen triangle was a back face we let through */
extern int gte_obj_backface;                           /* prop being drawn gets back faces (hooks.c dcls_run, tree backs) */
int hooks_pkt_room(void);   /* hooks.c: this frame's draw packets still under the extra-faces budget */
int hooks_pkt_full(void);   /* hooks.c: this frame's draw packets near the end of the game's ring (drop faces) */
extern unsigned subpx_cur_oid;                         /* model id of the object point being put (gte.c; 1..4095, | OID_ACTOR) */
#define OID_ACTOR 0x1000u                               /* id bit: a character (hooks.c dcls_run) */
#define OID_STATIC 0x2000u                              /* id bit: a prop that has not moved lately; in a polygon's oid
                                                          (gpu.c Vtx) the same bit means "hidden behind characters" */
#define OID_FLOOR 0x4000u                               /* polygon oid only: a still prop's face seen from above
                                                          (gpu.c exec_poly): hidden behind characters only past gpu_floor_k */
#define OID_GLASS 0x8000u                               /* polygon oid only: see-through object face drawn at the end of
                                                          the list (gpu.c glass_q): hidden behind any stored model pixel */
void gpu_glass_flush(void);                            /* draws the held-back glass (gpu.c; end of each DMA2 list) */
#define OID_MASK 0x1FFFu                               /* the part stored with object depth and compared */
int gpu_propsort(void);                                /* still props hide each other's faces behind them (gpu.c) */
extern unsigned gte_oid_bits;                          /* OID_ACTOR / OID_STATIC of the object being drawn (hooks.c) */
int subpx_oid(uint32_t sxy);                           /* model id of the object point(s) on that pixel, 0 = none / mixed */
extern uint16_t vram[512 * 1024];
void gpu_set_scale(int s);          /* internal resolution 1..4 (shadow VRAM, see gpu.c) */
int gpu_scale(void);
int gpu_hw_on(void);                /* 1 when the D3D11 renderer draws the hi-res picture */
const uint16_t *gpu_vram_hr(void);  /* (1024*s) x (512*s), == vram when s is 1 */

void cd_init(const char *path);
uint8_t cd_read(int reg);
void cd_write(int reg, uint8_t v);
void cd_event(void);
uint32_t cd_dma_read(void);
int cd_read_sector_raw(uint32_t lba, uint8_t *out2352);

void spu_init(void);
uint16_t spu_read(uint32_t off);
void spu_write(uint32_t off, uint16_t v);
void spu_dma_write(uint32_t w);
uint32_t spu_dma_read(void);
void spu_frame(void);
void spu_cd_audio(const int16_t *lr, int frames, int rate);

void dma_init(void);
uint32_t dma_read(uint32_t off);
void dma_write(uint32_t off, uint32_t v);
void dma_event(void);

uint32_t timer_read(uint32_t off);
void timer_write(uint32_t off, uint32_t v);
void timer_vblank(void);

uint32_t sio_read(uint32_t off, int size);
void sio_write(uint32_t off, uint32_t v, int size);
void sio_event(void);

uint32_t mdec_read(uint32_t off);
void mdec_write(uint32_t off, uint32_t v);

/* host */
void host_init(void);
void host_present(void);          /* show a frame, pump messages, pace */
uint16_t host_pad_buttons(int port); /* PS1 active-low button mask */
uint16_t host_pad_raw(int port);     /* same, ignoring game-side blocks (freecam input) */
void host_lstick(float *x, float *y);  /* left stick, analog only (no keys): x right, y up, -1..1, dead zone applied */
void host_rstick(float *x, float *y);  /* right stick for the camera: x right, y up, -1..1 */
void host_audio_push(const int16_t *lr, int frames);
int host_quit_requested(void);

/* bios */
void bios_init(void);
void bios_vblank(void);
void bios_irq_exception(void);
void hooks_vblank(void);

/* highest internal resolution: 9 x 240 = 2160 lines (4K) */
#define MAX_INTERNAL 9

/* options */
typedef struct {
    int widescreen;      /* 1 = 16:9 GTE projection */
    int stretch2d;       /* 1 = 2D (menus, HUD, text) stretched to 16:9 with the picture; 0 = kept at 4:3 (gpu.c sq_*) */
    int widecull;        /* 1 = widen the game's portal culling to the 16:9 view (needs widescreen) */
    int depthfix;        /* 1 = hi-res depth check: world objects stay hidden behind nearer room geometry */
    int dupfix;          /* 1 = hi-res picture leaves out exact repeats of a triangle drawn earlier in the frame */
    int objsort;         /* 1 = front-facing opaque object triangles also store depth (needs depthfix), see gpu.c DM_TW */
    int scale;           /* window scale */
    int fps_unlock;      /* unused (kept for config compatibility) */
    int fps;             /* frames shown per second (60, 120, 144, ...) */
    int overclock;       /* virtual CPU multiplier, 1 = original speed */
    int fixed_timestep;  /* 1 = game time from real elapsed time (fixes double speed, enables high fps) */
    int vsync;
    int volume;          /* master volume 0..100 */
    int vol_music;       /* 0..100: SPU voices the libsnd sequencer owns (0 .. reserved count - 1) */
    int vol_sfx;         /* 0..100: the other SPU voices (the game's own key-ons: effects, ambient loops) */
    int vol_voice;       /* 0..100: CD / XA audio outside movies (voice lines, in-engine cutscene speech) */
    int fullscreen;      /* start in fullscreen */
    int smooth;          /* 1 = bilinear upscaling, 0 = sharp pixels */
    int aa;              /* anti-aliasing at present (D3D11): 0 off, 1 downsample (area filter), 2 downsample + FXAA */
    int aniso;           /* anisotropic filtering (GPU renderer): most texel taps per pixel, 1 off, 2 / 4 / 8 / 16 */
    int msaa;            /* MSAA samples on the GPU renderer's target: 1 off, 2 / 4 / 8 */
    int internal;        /* internal resolution multiplier for 3D/2D drawing, 1..MAX_INTERNAL */
    int late_present;    /* 1 = show each hi-res frame one present later so raster workers overlap the guest */
    int renderer;        /* 1 = D3D11 hardware renderer for internal resolution above 1x, 0 = CPU threads */
    int perspective;     /* 1 = perspective-correct textures on 3D polygons with known corner depths (hi-res only; needs depthfix) */
    int subpixel;        /* keep the GTE's sub-pixel screen positions for hi-res drawing (no wobble): 0 off, 1 near only, 2 all */
    int rcam;            /* right-stick camera: 0 off, 1 on, 2 on + drifts back behind the player when released */
    int rcam_invert;     /* 1 = stick up looks down */
    int chatter;         /* idle voice lines: 0 original, 1 less often (2x gap), 2 rarely (4x), 3 off */
    int bar_roar;        /* Reptar bar pickup sound 0x1c: 1 chime + roar (original), 0 chime only, 2 none */
    int view_ext;        /* Extended View Distance: extra rooms of the low-wall / over-wall rules (hooks.c vd_ok):
                            0 off, 1 short, 2 medium, 3 full (no limit) */
    int voice_hub;       /* 1 = Restored Lines also adds timed chatter in the hub (db 1, hooks.c hub_chatter) */
    int voice_golf;      /* 1 = Golf Reactions: score / fail / ball-rolling lines on the golf holes (hooks.c golf_*) */
    int voice_restore;   /* 1 = idle chatter also plays voice lines the game never requests (hooks.c), 0 = original */
    int note_fade;       /* 1 = released notes fade over >= ~3 ms (game's instant release rate 0 clicks), 0 = original */
    int rcam_override;   /* 1 = right-stick camera also in levels where it is off by default (hooks.c rcam_excluded) */
    int wall_bonk;       /* 1 = original: walking into a wall bonks (anim, sound, short lock); 0 = just stop */
    int cut_smooth;      /* 1 = in-engine cutscenes blended between their 18.75/s CD packets (hooks.c), 0 = original */
    int shot_walk;       /* 1 = Fixed: the walk goes back to 1x after shooting while walking; 0 = original (stays 2x) */
    int light_fixes;     /* "Let There Be Light" Fixes (F1 Enhancements, ini [game] light_fixes). 1 = Fixed: in db 12
                            (power out) the kitchen ceiling fan stands still and the daytime-coloured furniture textures
                            get night colours; 0 = original. Parts below can be turned off alone for debugging. */
    int fan_still;       /* part (debug RUGRATS_FANSTILL, default 1): fan stands still; hooks.c rt_hook_8004e2ac */
    int hedge_leaves;    /* part (debug RUGRATS_HEDGELEAVES, default 1): hedges shed leaves, not boxes; hooks.c leaf_* */
    int night_furn;      /* part (debug RUGRATS_NIGHTFURN, default 1): furniture night colours; hooks.c night_frame */
    int final_level;     /* Final Level Behavior. 1 = Enhanced: after the last puzzle piece, a "Final level unlocked!"
                            screen, then stay in the level where the bar was taken; the hub TV is the entrance to the
                            final level (db 7) once all 12 pieces are placed. 0 = Default (original: straight on to db 7) */
    int dither;          /* 1 original PS1 dithering, 0 off (15-bit, may band), 2 off + full colour (GPU renderer keeps
                            8-bit shading in the hi-res target; CPU renderer = same as 0) */
    int golf_aim;        /* golf aiming turn speed in percent of the PS1's (frame-rate independent); 0 = uncorrected */
    int tex_dump;        /* 1 = texture dump mode (texpack.c: textures_dump\ next to the exe; [textures] dump) */
    int skip_intro;      /* 1 = boot straight to the title: no legal screen, no THQ / Nickelodeon / n-Space movies */
    int discord;         /* 1 = Discord Rich Presence on (discord.c; needs discord_id) */
    char discord_id[32]; /* Discord application ID ([discord] client_id) */
    char discord_image[64]; /* optional art asset key of that application ([discord] image) */
    char disc[1024];
    char saves[1024];
} Options;
extern Options opt;
int hooks_movie_active(void);       /* 1 while the movie player (8008b448) runs */

/* mods.c (see recomp/MODDING.md) */
void mods_init(const char *exe_dir, const char *ini);
void mods_vblank(void);
int mods_count(void);
int mods_active(void);
const char *mods_dir(void);
const char *mods_name(int i);
const char *mods_version(int i);
const char *mods_author(int i);
const char *mods_desc(int i);
const char *mods_error(int i);   /* NULL when it loaded */
int mods_enabled(int i);
int mods_toggle(int i);          /* 1 if it changed */
int mods_restart(int i);         /* switched in F1 but takes effect at the next start (mods with files) */
void mods_summary(int i, char *out, int n);
void mods_list(char *out, int n);
void mods_save(void);
