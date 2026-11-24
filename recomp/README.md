# Rugrats: Search for Reptar — native PC build (static recompilation)

Turns the USA disc image (SLUS-00650, SHA1 `b547ef42310a5b04ccfd62d12aa9c41af1c5d6f5`)
into `Rugrats.exe`, a native x86-64 Windows program. No emulator core: the game's MIPS
code is translated to C and compiled. A small native runtime stands in for the PS1 hardware
(GPU, GTE, SPU, CD, pad, BIOS).

## Build

```
python -I recomp/build.py "<path>/Nickelodeon Rugrats - Search for Reptar (USA).bin"
```

Output goes to `build/`: `Rugrats.exe`, `rugrats.ini`, `saves/`, `rugrats.log`.
The build refuses any other disc image (hash check). The original `.bin` is only read.

Needs Python 3 and clang (llvm-mingw). No downloads.

## Run

`build/Rugrats.exe` (disc path comes from `rugrats.ini`, or pass it as the first argument).

| Key | PS1 |
|---|---|
| Arrows | D-pad |
| Z / X / A / S | Cross / Circle / Square / Triangle |
| Enter | Start |
| Backspace | Select |
| Q / E / 1 / 3 | L1 / R1 / L2 / R2 |
| Xbox pad | standard mapping |
| Control Bindings | F1 > Audio/Input > Control Bindings... rebinds every PS1 button: one keyboard key and one controller button each (taking one already in use swaps the two; Reset to defaults). Reserved: Esc, Tab, F1-F12, J/L/I/K, L3/R3/PS, right stick; menus keep the default controls. ini `[bindings]` `key_<button>` = Windows VK code, `pad_<button>` = cross/circle/square/triangle/l1/r1/l2/r2/select/start/up/down/left/right (buttons: up down left right cross circle square triangle l1 r1 l2 r2 start select). Menu test `RUGRATS_MENU_BIND=1`. |
| DualSense / DualSense Edge / DualShock 4 | native (USB or Bluetooth, no Steam/DS4Windows needed); same layout as the PS1 pad, Create/Share = Select, Options = Start, PS button = options menu |
| Right stick (Xbox / Sony pad), J / L / I / K | Turn the camera around the player / look up-down (F1 rows "Right-stick camera" Off / On / On, auto-center and "Camera up/down" Normal / Inverted; ini `[input] rstick_camera` 0/1/2 default 1, `rstick_invert`). Once turned, the camera keeps that world direction and always eases back (about 1 s, walking or not) to the game's own follow distance after walls pushed it in; auto-center hands it back to the game after 1.5 s without the stick. R1 (the game's own "camera behind the player" button, whatever it is bound to) snaps the turned camera behind the player at once, like the original; the stick keeps control afterwards. In tunnels it stops at the walls / ceiling (uses the level's collision grid). Debug: `RUGRATS_RCAM`, test input `RUGRATS_RSTICK=<t60 from>:<t60 to>:<x>:<y>,...` |
| F1, either stick click (pad) | Options menu (pauses the game; saves to `rugrats.ini`) |
| F3 | FPS counter on/off (top-right; also an F1 menu row, ini `[video] fps_counter`) |
| F5 / F7 | quick save / quick load (slot 1-4; also in the F1 menu for pads) |
| F6 | next quick save slot |
| F9 | toggle widescreen |
| F8 | cycle frame rate (60/75/100/120/144/150/240) |
| F11, Alt+Enter | fullscreen (Esc no longer leaves fullscreen) |
| Esc | quit, after a "Quit Rugrats?" Yes/No prompt (No is pre-selected; game paused while it shows). The F1 menu's Quit game row uses the same prompt. Test: `RUGRATS_MENU=<frame> RUGRATS_MENU_ESC=1` (cancel) / `=2` (quit) |
| Tab (hold) | fast forward |

Saves: `build/saves/card1.mcd` (standard 128 KB memory card image, emulator compatible).

## Pipeline

1. `tools/extract.py` — verify SHA1, read ISO9660, pull out `SLUS_006.50`.
2. `tools/analyze.py` — instruction stats, BIOS call stubs, pointers.
3. `tools/recomp.py` — find functions (recursive descent, jump tables, setjmp re-entry points,
   code pointers), emit one C function per guest function into `build/gen/`.
4. `runtime/` — CPU state, memory map, HLE BIOS, GTE, software GPU, CD controller (reads the
   `.bin` at runtime), SPU, pad, Win32 host.

## High frame rate

Set `fps=` in `rugrats.ini` (or press F8). Gameplay runs at that rate at correct game speed.

How it works (`runtime/hooks.c`):
- Presentation has its own clock; the 60 Hz vblank interrupt (music, callbacks, counters) is untouched.
- `8001ceb4` (frame pacing) is replaced: one game frame per presented frame.
- `8001ce48` (time step) is replaced: game time = virtual time that really passed. The game keeps time
  in 1/300 s ticks, so 60/75/100/150/300 fps give perfectly even motion; 120/144/240 alternate tick counts.
- Draw and flip (fixed 2026-11-18): the vblank callback `8001d0d8` is a two-step pipeline (`gp+132` bit 1 = the
  finished frame waits to be drawn, `8001c408` sends its lists; bit 0 = the drawn buffer waits to be shown,
  `8001c088`/`8001c020`); on a PS1 one call per vblank does "show N-1, draw N". Above 60 fps it used to run partly
  from the flip-done poll `8001d3c4` (at the end of the NEXT frame) and partly from the real 60 Hz interrupt, so
  presents alternated 2 flips / 0 flips: only half the frames were ever shown (120 fps = 60 distinct pictures) and
  every other present drew two frames (the circus slowdown to 80 fps). Now, in high-fps gameplay, the pacing hook
  runs the callback once at the end of every game frame, before the present, and the interrupt's own calls are
  skipped: one flip and one frame's drawing per present. Menus / intro / title and 60 fps or less: unchanged.
  `RUGRATS_FRAMECB=0` = old behaviour. Debug: `RUGRATS_FLIPLOG=1` (flips per present), `RUGRATS_FRAMEPROF=<from>,<to>`
  (per present: game ms, game frames, projections, polygons, list sends), `RUGRATS_SENDSTACK=<from>,<to>`.
- `overclock=` gives the virtual PS1 CPU headroom; device timings scale with it, so CD/XA stay real-time.

Measured (test script, gameplay in the house hub): 60, 100, 120, 144, 150, 240 fps all at 100% game speed.

Bug fixed on the way: the original pacing assumes `VSync(0)` always waits a full vblank. When a frame
finishes early it doesn't, so the game advanced 2 vblanks of time per 1 real vblank (gameplay at 200% speed).

Slow walking surfaces (hook `8005e134`, fixed 2026-11-09): on the soap floor the game moves the player by
`dir * trunc(speed * ticks * 400 / 4096)`, each axis truncated again. With small high-fps steps the rounding ate
the movement (soap: 454 u/s at 30 fps, 300 at 144, 120 at 240; PCSX-Redux ~463). The hook takes the 30 fps step
(10 ticks, same rounding) and gives `ticks / 10` of it per frame, carrying the remainder: 454 u/s at every rate,
identical to the original at 30 fps. Normal walking does not use this routine. `RUGRATS_WALKSTEP=0` = original.

Reptar landing stomp (hook `8006f644`, fixed 2026-11-10): in the Reptar 2010 level the landing state's per-frame
callback `8005cba8` plays stomp sound 0x5d (2 voices) every game frame. On a PS1 that level runs ~8 fps, so a
landing plays it 3 times (PCSX-Redux); the port played it 10x at 30 fps and 50x at 120 fps. Now at most once per 38
ticks (7.5 vblanks) of game time: 3-4 per landing at any rate. `RUGRATS_STOMPFIX=0` = original, `RUGRATS_STOMPGAP`.
Debug: `RUGRATS_SFXSKIP=<hex id>,...` mutes those sound ids in that wrapper.

SPU mix overflow (fixed 2026-11-10): the PS1 clamps the voice + reverb sum to 16 bits before the main volume. The
port did not, and the int32 `sum * main volume` overflowed once the sum passed ~65535: samples flipped sign, heard
as loud static. A Reptar 2010 landing stacks up to 5 near-full-scale stomp voices (sum ~3x full scale), so every
landing hit it. Now clamped like the hardware (= plain clipping, as in PCSX-Redux). An Audio Limiter option was
tried the same day and removed again at the user's request once the overflow was fixed.


Limits: menus stay 60 fps; in-engine cutscenes are paced by their CD/audio stream (~19 fps), like on a console.

### Streamed in-engine cutscenes (fixed 2026-11-08)

Player `8007424c`; stream start `80019da4` (Setloc + ReadS, `VSync(3)`, Setfilter channel = stream index, then the
FIRST data sector that arrives is taken as the stream header: magic `BABED00D` / `F00DBABE`, version 3, `DEAD`, frame
count at +8 -> `gp+1376`); CD callback `80019c88` keeps packets of the wanted stream (word at sector data +0) in an
8-buffer ring and sets `gp+104`; `8007412c` takes one packet per frame (frame number -> `gp+3508`) and ends the
cutscene on 0xDDDDDDDD or a frame number below the current one. `DATA/CINEMAT/CINEPAK/CINEPAK*.BIN` interleave 4
cutscenes (data sectors start with stream id 0..3; XA channel = stream id); `OPEN.BIN` (opening cinematic) is one
stream with empty padding sectors after the header. Two port bugs, both checked against PCSX-Redux:
- **CD seek time** (`cdrom.c` `seek_cycles`): ReadN/ReadS after a Setloc to another position now waits like a drive
  (forward hop < 32 sectors: 20 ms + reading through them; else 100 ms + up to 300 ms by distance). Without it the
  first sectors arrived during that `VSync(3)` and the game read the wrong stream's header: Grandpa's Teeth intro
  stopped at 844 of 1491 frames (45 of 80 s), the house intro at 176 of 1001. `RUGRATS_CDSEEK=0` = old.
- **Padding packets** (hook `8007412c`): our fast loop fetched OPEN.BIN's all-zero padding sectors as "frame 0" and
  ended the 1412-frame opening cinematic after one frame (it now plays before the title, as on a PS1). Packets whose
  first 8 bytes are zero are dropped. `RUGRATS_CUTPAD=0` = old. Debug: `RUGRATS_CUTLOG=1`.

### Smooth Cutscenes (2026-11-21, Enhancements row, ini `[video] cut_smooth`, default On)

The stream gives one packet per 4 data sectors (18.75/s in `CINEPAK*.BIN`, ~30/s in `OPEN.BIN`) and the player
renders once per packet. A packet holds only absolute transforms: camera position + quaternion (header), objects
(quaternion -> matrix obj+4, position obj+0x18, id), characters (position + one quaternion per bone into the
skeleton obj+0x154). With the option on (and the real-time step on):
- the cutscene chain (`8001d3e8` called from `0x800745dc`) gets the gameplay pacing (one loop pass per presented
  frame, early flip);
- `8007412c` (hook) returns at once when no packet is ready, after writing camera / object / bone transforms blended
  between the two packet copies around the display position (positions lerp, quaternions nlerp keeping the 13-bit
  range); a ready packet runs the original code (faces, mouths, sounds on time) and is then blended the same way;
- display position: an even frame clock fitted to the last 32 packet arrivals (arrival = the CD callback `80019c88`
  setting the ready flag), trailing the newest packet by the arrival spread + 1.1 frames (~60-90 ms behind the sound);
- no blend over a camera cut (eye moves > 1500 units or turns > 30 degrees in one packet), a frame gap > 4, or an
  object / character that jumps > 2000 units;
- the fridge door halves (0x259 / 0x25a, companions of objects 0x1fe / 0x1ff, drawn from the angle obj+0x24, which
  `80073c34` raises by 50 per packet) are blended from the value after each packet; the true value is put back
  before each packet so the `+= 50` doesn't drift;
- the loop's fade-out step (`s2 += 25` per pass in the last 13 frames) is undone on the extra passes.
Game state after a cutscene is the same as with the option off (checked: fridge cutscene, frames after it identical
apart from random sparkle particles). `RUGRATS_CUTSMOOTH=0` / `RUGRATS_CUTSMOOTHOPT=0` = off;
`RUGRATS_CUTSMLOG=1` (packets, camera deltas) / `2` (+ display position per frame) / `3` (+ record ids) / `4` (+ every
object / bone write); `RUGRATS_CUTSMDBG=<n>` raises blended objects by n (proves the writes reach the screen).
Not handled: the opening cinematic's frame-range callback `8008ea70` (places one object by the integer frame number:
that object still steps at the packet rate and leads the blended picture by ~1 frame).

## Audio and auto frame rate

- Output keeps ~4 buffers (~67 ms) queued; each chunk is resampled by up to ±0.5% to hold that level
  (the game's sample clock and the sound card never match exactly). After a stall, a little silence
  refills the queue once instead of every later chunk arriving late.
- XA music (37.8/18.9 kHz) is resampled to 44.1 kHz with cubic interpolation (was nearest-neighbour: gritty).
- CD sector reads are strictly periodic too (cdrom.c `cd_event`: next read = previous due time + one sector time).
  Before, each read was timed from the late event that ran it, so the drive ran ~4% slow: XA voice lines played
  faster than they arrived and had a ~7 ms hole every ~170 ms (crackly speech). The SPU also waits 1024 frames
  (23 ms) before playing CD audio that starts after the ring ran empty (`CD_PRIME` in spu.c), so arrival jitter at
  the start of a line doesn't gap. `RUGRATS_XALOG=1` lines show the CD controller volumes too.
- vblank and present are strictly periodic events (`sched_periodic` in hw.c). Before, each fired relative
  to the late safe point that ran it, so the game clock drifted by up to ~2% depending on the frame rate.
- Auto frame rate (`auto_rate` in host_win32.c): if the PC can't present `fps` frames per second, it
  presents fewer (window title shows "PC limit") instead of slowing the whole game and starving the
  audio. It steps back up when there is headroom, straight to what the measured frame time allows (never past
  the display rate on the way up). If a rate above the display's refresh fails while frames still arrive at the
  display rate (fullscreen presents can be capped at the refresh even with VSync off), it stays at the display
  rate and stops trying higher ones until the frame rate option or the display changes.
  When the target is the display's refresh rate (fps = Hz, or capped to it as above), only even shares of the
  refresh are used (120 / 60 / 40 on 120 Hz), so frames always line up with the display: it stays at the refresh
  rate through short misses, drops one share only when frames average over 1.5 refreshes (below ~80 fps at 120 Hz) for 2 s, and
  goes back up when the CPU time and the GPU time (timestamp queries, `hw_gpu_ms` in gpu_d3d11.c) fit 80% of the
  faster frame. A drop soon after going up doubles the wait before the next try (3 s up to 30 s).
  `RUGRATS_LADDER=0` = the old free steps. Debug: `RUGRATS_DTLOG=1` (frame / busy time histograms + GPU ms every
  600 frames), `RUGRATS_WINPOS=x,y` (open the window on another monitor), `RUGRATS_LAGTEST=from:to:pct[:ms],...`
  (that share of presented frames spends extra ms, 9 by default, before the present).

## Presentation / VSync

`runtime/present_d3d11.c`: D3D11 flip-model swap chain; the 320x240 frame is uploaded as a texture and
scaled on the GPU (`smooth=0` sharp / `1` bilinear). GDI is the fallback (no D3D11, or `RUGRATS_GDI=1`).

Anti-aliasing (`[video] aa`, F1 Video row "Anti-aliasing", `RUGRATS_AA=0/1/2`; D3D11 only):
- `0` Off: the plain scaling above.
- `1` Downsample (default): when the picture is bigger than the window (e.g. 9x internal resolution in a 1440p
  window), every source pixel inside a screen pixel is averaged (area filter, in linear light). That is
  supersampling: smooth edges and no broken thin lines. Axes that are scaled up keep Sharp / Smooth. When nothing
  is shrunk, it draws exactly like Off.
- `2` Downsample + FXAA: an FXAA pass (our own code) at the picture's resolution first, then the same scaling.
  Smooths edges at any internal resolution; slightly softer textures and 2D. The F1 menu and the on-screen
  messages are never filtered.

Anisotropic filtering (`[video] aniso` = 1 off / 2 / 4 / 8 / 16 (default), F1 Video row, `RUGRATS_ANISO=<n>`; GPU
renderer only): PS1 textures have no mipmaps and are point-sampled, so far and slanted floors and walls shimmer
when one screen pixel spans several texels. Where that happens, the triangle's pixel averages up to n texels spread
along the squeezed direction (each one the normal PS1 lookup: texture window, CLUT, uv box); elsewhere nothing
changes, so close-up texels stay sharp. The centre texel still decides see-through and semi-transparency, so
cut-outs, depth checks and blending are as with Off. Sprites and texture-pack replacements are not filtered.

MSAA (`[video] msaa` = 1 off (default) / 2 / 4 / 8, F1 Video row, `RUGRATS_MSAA=<n>`; GPU renderer only): the
hi-res VRAM target, its depth buffer and the object-depth buffer become multi-sample, so polygon edges are smoothed
at any internal resolution. Shading still runs once per pixel, except the shaders that read the object-depth
buffer (sort fixes), which run per sample. The shown area is resolved (averaged, in linear light) before
presenting; readback (F1 menu, test shots, save states) resolves the whole target; VRAM copies go sample by sample.
Changing it rebuilds the target from native VRAM (like changing the internal resolution). Video memory: the three
targets x samples, e.g. 4x MSAA at 9x is about 2 GB, at 4x about 400 MB. If the card can't do the asked count it
tries fewer, then Off (logged). Alpha-tested cut-outs (texel 0) keep hard inner edges, as usual with MSAA; Downsample
(Anti-aliasing) and MSAA work together.

- `vsync=1`: no tearing. If the monitor's refresh is a whole multiple of `fps` (59/60 Hz counts as 60),
  `Present(n)` paces the game, e.g. 144 fps on a 144 Hz screen, 60 fps on 60/120 Hz. Otherwise frames are
  timed by the clock and the compositor shows the latest one.
- `vsync=0`: clock pacing, tearing allowed: lowest lag, and works with G-Sync / FreeSync.
- Frame latency is 1 (`SetMaximumFrameLatency`). Refresh is re-read on display change / window move.

## Internal resolution

`internal=1..9` in `[video]` (default 2; F1 menu changes it live). 4x ≈ 1080p, 6x = 1440p, 9x = 2160 lines (4K). `runtime/gpu.c`:
- Native VRAM is still drawn exactly like the console, so texture reads, VRAM copies and CPU readback
  stay correct.
- A shadow VRAM at N x is drawn again for every polygon, sprite, line and fill at full detail and is what
  you see. Uploads and copies are carried over to it.
- Shadow drawing is queued to worker threads (hardware threads − 2, max 16), each owning interleaved 8-row bands.
  The queue is drained before uploads/copies, before the frame is shown, and before a native draw that
  would overwrite a texture area that queued work still reads.
- The 15-bit shadow goes straight to a D3D11 B5G5R5A1 texture (no CPU colour conversion).
- **Perspective textures** (`perspective` in `[video]`, F1 Video page, `RUGRATS_PERSP`): 1 On (default), 0 Off
  (original). The PS1 maps textures affinely, so a big polygon whose corners are at very different depths bends its
  texture (golf wall bricks curving up the right edge while walking along it; 16:9 shows more of such near pieces).
  On: every textured, opaque 3D polygon whose corners all have a depth from the depth check (room / object points,
  split midpoints; so it needs "Hide objects behind walls") interpolates uv * w and w (w = 1/z) and divides per pixel,
  in both renderers (CPU `tri_t`, D3D11 `vs_main` / `shade`: weight 0 = 1 = the old affine result exactly). Corner
  depths more than 64x apart fall back to affine. Native VRAM stays affine (the game's own picture). Semi-transparent
  polygons and 2D stay affine. Wall pieces with a corner clamped at the GTE screen limit (+-1024) or split from such a
  corner (`np_fromclamp`) need more: that corner carries its vertex's texture coordinate but is drawn where the vertex
  does not project (it can even be behind the camera), so its depth as the weight bent the texture (the near golf wall
  cap). Every tag also carries its point's camera-space position (GTE x y z, `SXP` in gte.c; split points the 3D
  midpoint of their ends, where their averaged texture coordinate belongs). `clamp_uv` takes three corners' points as
  the wall plane and its texture layout and gives each DRAWN corner the uv and 1/z of the plane point its pixel shows
  (correction in uf / vf, u / v keep the texel clamp box): exact perspective over the piece, straight cap and brick
  lines into one vanishing point. Unclamped corners keep their own uv and depth. No points / ray misses the plane:
  affine. `RUGRATS_CLAMPUV=0` = affine (PS1 look, sawtooth cap), `RUGRATS_CLAMPAFFINE=0` = old perspective (bent).
  True geometry too (`clamp_draw`, hi-res only, `RUGRATS_CLAMPPOS=0` = off): any polygon with such a corner whose
  corners all carry their 3D point is drawn native-only as the game sent it, and in hi-res each of its triangles is
  clipped in 3D (z >= 1 and a guard band 4 px outside the drawing area) and projected exactly, with uv, colour and 1/z
  per clipped corner. So the wall's outline at the lens is where the wall is, not where the pins put it. Triangles the
  PS1 drops for size (> 1023 x 511 at the pinned corners) stay dropped (a tree trunk at the lens would otherwise appear
  as a slab), except room geometry with depth (DM_WRITE / WNT): the game culls a near wall's split pieces by their
  pinned positions, so only the whole wall's true shape covers it without holes.
  Too big without pins too (`RUGRATS_BIGTRI=0` = PS1 drop): a triangle whose corners all really project (tagged 3D
  point in front of the lens, none pinned) but which spans more than 1023 x 511 native pixels goes the same clamp_draw
  way. With the stick camera just in front of a thick door frame (Angelica's corridor door, 2026-11-11) the frame's
  front and inner faces span ~620 lines (top corner far above the screen); the PS1 drops them and the next room showed
  through. The PS1 drop stays for pinned triangles, for triangles with a corner nearer than `RUGRATS_BIGTRIZ`
  (default 1000 camera units) and for triangles over the view centre (where the player stands): a mini golf tree
  trunk at the lens (z 550-780) otherwise covered Angelica as a slab. The door frame's nearest corners were 1335+.
- **Dithering** (`dither` in `[video]`, F1 Enhancements page, `RUGRATS_DITHER`): 1 Original (default), 0 Off (no 4x4
  dither pattern; colours still truncated to 15-bit, so gradients band), 2 Off + full colour (GPU renderer: shaded and
  lit colours are kept at full precision in the hi-res target, no dither and no banding; CPU renderer = same as Off).
  Textures with dithering painted into them are not changed. Test shots read back at 15-bit, so they don't show the
  full-colour part; capture the real window to check it.
- **Hide objects behind walls** (`depthfix` in `[video]`, default 1; F1 row). The game sorts room polygons by their
  farthest corner, so a world object behind a big near room face (a shelf top seen from above) can paint over it.
  Hi-res renderers keep a depth buffer: room geometry writes it, world objects are hidden where they are more than
  96 camera units (SZ scale, a room block is 1024) plus a slope bias behind it (`RUGRATS_DEPTHGAP=96`; room writes
  are pushed back by their own depth change over `RUGRATS_DEPTHSLOPE=2` native pixels, so floors seen at a grazing
  angle keep shadows / feet; D3D11: SlopeScaledDepthBias on room writes, testers use `ps_test`, which writes
  SV_Depth with the gap); everything else keeps the game's draw order. The game's own 2D splits of
  near room polygons (`800120fc` / `80011854`, edge midpoints) get the average depth of the two ends, so split pieces
  write depth too. The point table keeps room and object depths apart (a clash keeps the farther room / nearer object
  depth), probes 4 slots so live points are not evicted, and starts a new generation per display flip.
  Room pieces are tested the same way against nearer room depth (the room is sorted by farthest corner, so a far
  piece can be drawn after a near wall): CPU in draw order; D3D11 first draws each batch's room pieces and fills
  depth-only (room nearest wins, fills 0), then tests room pieces like objects in the normal pass (plain depth
  testing, any D3D11 GPU). `RUGRATS_ROOMTEST=0` turns that part off.
  The check runs only in frames that show the 3D world (room geometry, `gpu_world_frame`): the title screen draws no
  fill, so its depth was never cleared and the babies peeking in at the bottom corners stayed hidden.
  `RUGRATS_DEPTHWORLD=0` = old (every frame).
  **Lenses on the title** (part of "Fix model sort glitches", hi-res only, frames without the 3D world;
  `RUGRATS_LENSFIX=0` = off): Chuckie peeking in at the top right has a face triangle behind his lower glasses lens that
  the game sorts after the lens (white, semi-transparent), so it painted a skin wedge over the lens corner (the PS1
  draws the same list; at 4x it is a sharp triangle). Semi-transparent object triangles with a model id are remembered
  for the frame; a later opaque triangle of the same model that overlaps one and lies behind it there gets the lens
  blended again over just that overlap. GP0 log: `lensfix:` lines. In world frames the glass fix already does this.
  Room water surfaces (the sewer's brown water, drawer `80030ce0`) are tested like objects: the water grid runs on
  under the raised floor beside it and the game sorts it after that floor, so square bits of water showed through
  the ledge and popped in and out as the camera turned. `RUGRATS_WATERDEPTH=0` = old (water never tested).
  **Walls right at the camera** (`RUGRATS_NEARPLANE=0` = old): with the stick camera close behind Tommy, a wall
  beside the lens has a corner too near / behind the camera; the GTE clamps it (e.g. to -1024,-1024), it has no depth,
  and the game's 2D split pieces of that wall (`800120fc` / `80011854`) inherit none either. The wall then wrote no
  depth and things behind it that the game sorts later painted over it (the closet door by the house boxes). The PS1
  GPU never draws the parent quad (an edge > 1023 px wide / 511 tall), only the split pieces. Fix (gpu.c
  `exec_poly`, `np_collect` / `np_plane` / `np_iz`): the GTE stores each room vertex's camera-space position in the
  point table (`subpx_put3`; clamped pixels keep the newest 4 different ones, since several walls' corners land on
  -1024,-1024), and `subpx_mid` remembers each split midpoint's two ends. A room piece with corners lacking depth
  walks back from every corner to real wall vertices, needs 4+ points on one plane (within 4 units + 0.1%; three
  always look flat), tries each candidate of a clamped pixel, and falls back to leaving clamped pixels out / following
  midpoint links over a pixel's own point (another vertex can land on a split midpoint's pixel). Then 1/z of that
  plane at each drawn corner (`gte_proj_params`: OFX/OFY/H/widescreen) is exact over the piece; corners with a known
  depth must agree within 3% (unless they are split midpoints). Such pieces draw write-notest (hide objects behind
  them, never hidden themselves). GP0 log: `nearplane: N wall points ...` / `nearplane mode M: ...` (points + offsets).
  Positions too, experimental and OFF by default (`RUGRATS_NEARPOS=1` = on, `np_point3`; on the mini golf hedge it
  opened holes and bent the wall cap in single frames while the camera turned): a split corner that comes from a clamped corner sits at the
  2D midpoint of the clamp, not of the real corner; its 3D point (midpoints in 3D, the clamped pixel's candidate on
  the plane) is projected again and the difference goes into the hi-res sub-pixel offset (native VRAM unchanged).
  Skipped when a corner is behind the lens (z <= 1) or would move more than 2048 px. GP0 log: `nearpos: ...`.
  Two room points at different depths on one screen pixel (near wall corner over far scenery): the table also keeps
  the nearest room depth; a quad with one such corner takes the candidate that fits the plane of its other three
  corners, any other polygon with such a corner writes depth but is never hidden by the room test (it once tilted
  near walls away and let old picture show through while the camera turned). `RUGRATS_ZPICK=0` = old rule. Before
  that, a corner with its own exact depth (GTE tag) simply takes the candidate within 3% of it (`RUGRATS_ZTAGPICK=0`
  = off): a corridor wall that kept the far candidate stored depth far behind itself and glass drawn later showed
  through it (2026-11-12).
  Test helpers: `RUGRATS_SHOTFROM=<frame>` (no test shots before it; every-frame shots are ~4 MB each),
  `RUGRATS_GP0LOG=<n>` also matches the game's vblank count printed by `RUGRATS_SHOTLOG=1`.
  `RUGRATS_SHOTRAM=1` also writes guest RAM (2 MB) as `shots/rNNNNN.bin` with each test shot (for RAM diffs).
  `RUGRATS_ENTLOG=1` logs the Triangle target finder (`800631d8`) and hub level-entrance handler (`8004f78c`, db = obj+108).
  `RUGRATS_DEPTHFIX=0/1` overrides it; `RUGRATS_DEPTHVIS=1` (CPU renderer) tints polygons without depth red and
  objects drawn over no depth magenta.
- **HUD stays on top** (part of "Hide objects behind walls"). With the stick camera tilted up the eye sits near the
  floor, and the game's sort put floor pieces right in front of the lens after the HUD (the cookie counter box was
  cut by the floor; native PS1 order). Every point the room / object renderers project is marked as a world point,
  even without a usable depth (too near, clamped; `subpx_world`, also for split midpoints). A polygon with at most
  one world corner, opaque, a textured screen-aligned rectangle with the neutral tint 808080 (or raw texture;
  world sprites are tinted by distance and skewed: the pie gallery's pies are 2cebebeb..2c727272 and are not HUD,
  the clerks draw over them), drawn after 64 polygons with world corners since the drawing area was set, drawn at
  exactly the same corners and texture for the last 30 frames (`HUD_FRAMES`; frames = display flips, the game sets
  the drawing area ~6 times per frame) and whose centre is not a projected point (`hud_like`: world sprites such as
  falling petals or a spinning cookie pickup are flat quads around a projected point and move; a petal held still
  for 2 frames once drew over Tommy's head), and with fewer than 2 corners on projected points (the Reptar bars are
  untinted camera-facing squares whose corners the game projects one by one; they showed through walls) is an overlay (`DM_HUD`: CPU depth 1e30, D3D11 depth 1.0). Later
  polygons with 2+ world corners but no usable depth (`DM_HTEST`, D3D11 `ps_h` = SV_Depth 0.999 against it) and
  unsure room pieces (WNT) skip overlay pixels; room and object tests already do. Backdrops drawn before the 3D
  scene are not overlays. GP0 log: `depth hud` / `depth hud-test`, and `hud? no: ...` says why a 2D polygon was
  not taken.
- **Fix doubled-face flicker** (`dupfix` in `[video]`, default 1; F1 Video row; `RUGRATS_DUPFIX=0/1`). Some models draw a
  face twice in one frame (a toy store stand's side face: once as a triangle, again as half of a quad), and the
  game's sort can put other polygons between the two copies (the stand's books). When the camera angle tips the sort,
  the second copy painted over the books for single frames. The hi-res picture now leaves out an exact repeat
  (same screen corners, texture coordinates, colours, CLUT, texture page and window; a triangle matches half of a
  quad) of an opaque triangle already drawn since the game last set the drawing area (GP0 E3 = a new frame).
  Native VRAM stays exact. GP0 log: `repeat: hi-res skips ...` lines. (Tried first and dropped: depth-sorting
  objects against each other; the stand has a light-blue lid the game always paints over, which true depth showed.)
- **Fix model sort glitches** (`objsort` in `[video]`, default 1; F1 Video row; `RUGRATS_OBJSORT=0/1`; needs
  "Hide objects behind walls"). The game sorts each face of a model on its own, and faces use different rules
  (average, nearest or farthest corner), so a far face can land after a nearer one: in the toy store a shelf's purple
  back panel (average) cut a wedge into the slanted wood top of the box in front of it (farthest corner) while the
  camera turned. The GTE output matches PCSX-Redux exactly; the order is the game's own. Fix: an opaque object
  triangle that faces the camera (clockwise on screen) also stores its depth in a separate object depth buffer
  (CPU `hr_oz`, D3D11 R32_FLOAT target `oz`), and later opaque pixels of the SAME model clearly behind it are hidden.
  Model id: the GTE hashes the rotation + translation in use when it projects an object point (`subpx_cur_oid`,
  1..4095, kept per pixel in the sub-pixel table, up to four models per pixel since boxes touch the shelf and each
  other and an animated model like Tommy's head can add more; extra ones are dropped; a polygon takes the one model all its corners share, none if two or none); the stored depth
  carries it in its 13 lowest mantissa bits. Different models (a clerk running past a stand, Tommy's limbs, shadows)
  keep the game's order: hiding across models showed faces the game paints over (pie gallery). **Props in front of
  props** (2026-11-20, `RUGRATS_PROPSORT=0` = off): two props that stand still (see below, `OID_STATIC`) may hide each
  other: a still prop's pixel is stored with the character bit, so a later face of another still prop clearly behind
  it (object gap; faces seen from above also the floor gap) is hidden, as behind a character. In the laundry room
  ("Let There Be Light") the top-loader's front, ~70 units behind the front-loader's front, was drawn after it and
  cut a notch out of its left border. Moving props and characters keep the game's order. **Shadows under the
  player** (`RUGRATS_SHADOWFIX=0` = old, `2` = floor gap too): a semi-transparent object face seen from above (the
  player's shadow quad, GP0 2E) counts as a still prop face, so it is hidden where it lies more than the object gap
  behind a stored player pixel. The game sorts the shadow among the slippers' faces; while the stick camera turned it
  painted over the feet in some frames and lay under them in others (2026-11-11). GP0 log: `shadowfix:` lines.
  **Glass** (`RUGRATS_GLASSFIX=0` = old): every semi-transparent object face (toy store glass cases) is held back in
  the hi-res picture and drawn later (native VRAM keeps the game's order): it only moves past room pieces and objsort
  faces, whose depth can hide it; the next piece without usable depth (near the lens, no model id, 2D, lines, sprites),
  a fill / VRAM transfer, a new drawing area, a flip or the end of the drawing list draws it first (a polygon without
  usable depth only releases the held glass it overlaps on screen, plus earlier held glass that really overlaps one of
  those, so overlapping sheets keep their order, unless the earlier sheet lies wholly in front of the released one;
  a polygon wholly behind a sheet's plane does not release it, `RUGRATS_GLASSBEHIND=0` = off; write-notest room pieces
  release the glass they lie in front of where they overlap, `RUGRATS_GLASSWNT=0` = off; `RUGRATS_GLASSFIX=2` =
  release all, the first rule; `=3` debug: never release early, a reference for checks). A case's glass wall and the room floor piece it stands on sort as a tie; while the stick camera moved, the
  floor sometimes came after the glass and painted over it, so glass sheets went missing and popped back in
  (2026-11-12). Drawn last, the glass is still tested against the room depth (walls in front hide it) and is left out
  where ANY model stored a nearer depth (polygon bit `OID_GLASS`, D3D11 flag bit 31). GP0 log: `glass:` lines. One exception,
  **the player stays in front of fixed props behind them**: the scene renderer draws each room's props (list
  room+0x48) and its moving characters, player included (list room+0x4c, draw calls return to `80033a7c`); a big
  prop face (a tall curtain whose top is near the camera) can sort after the player standing in front of it and cut
  a piece out of him. The player's model ids carry bit 12 (`OID_ACTOR`); props whose object words +0x10..0x3f never
  change carry `OID_STATIC` (hooks.c `dcls_run` / `prop_still`), and such a prop's face clearly behind a stored
  player pixel is hidden; a face seen from above (1/z grows down the screen, at least `RUGRATS_FLOORK` = 2 x
  faster than sideways; the sign alone flipped on upright faces seen from the side) only when it is also more than
  `RUGRATS_FLOORGAP` (default 96 camera units; 0 = never, the old rule) behind: the game draws shadows and floors
  (the toy store soap) over a character's feet on purpose, but an armchair's armrest top painted a spike across
  Tommy's face. Polygon bit `OID_FLOOR` (D3D11 flag bit 30). The player is never hidden by this rule;
  other characters and props that move keep the game's order. `RUGRATS_ACTORFRONT=0` off / `1` default / `2` every
  character, prop and face (first try: lost shadows, Tommy stood on top of the soap). **Room over object**
  (`RUGRATS_ROOMOBJ=0` = off): room pieces and object faces share one sorted list, so at some camera angles a floor
  piece is drawn after an object face standing on it and paints over it (an armchair's skirt lost its lower triangle
  in single frames while the stick camera turned). A room pixel is now left out where a stored object pixel is
  nearer than it (room depth with its slope bias); it still stores its room depth. Plain depth, whatever the draw
  order. This also shows faces the game's order used to paint over although they are in front: the toy store
  stands' light-blue top (drawn early in the frame, then covered by the floor and the stand's other faces). The
  user decided that this top is correct and must show (2026-11-10). `RUGRATS_ROOMOBJSPAN=1` brings back the
  stricter rule that hides it: the object pixel must also not be nearer than the whole room polygon (nearest
  corner + room gap), since only polygons whose depth ranges overlap can tie in the sort. Where a room pixel
  paints, the object depth gets a room mark (model id 0, blend MAX with the room depth; with the span rule also
  MIN with the polygon's near limit) instead of a clear; `RUGRATS_ROOMOBJGAP` = extra gap in camera units
  (default 0). Unsure depths stay out of this rule (2026-11-10, a handrail by the house stairs flickered through
  the wall while the stick camera turned): room pieces with a corner that has two room depths (write-notest) store
  the far one but use their near candidates for this rule (`RUGRATS_ROOMOBJWNT=2` default, `1` = far as before,
  `0` = they paint and clear the object depth); the game's 2D split midpoints also get the near average of their
  ends, so pieces split from such a corner count as unsure too (`RUGRATS_MIDNEAR=0` = old: far only); an object face uses its own model's depth on each corner, not the nearest
  point of any model on that pixel (`RUGRATS_OIDZ=0` = old; a handrail corner under Tommy's head took the head's
  depth). A polygon whose corners all share several model ids takes the
  newest (`RUGRATS_OIDNEW=0` = none, the old rule): above 60 fps one table generation holds 2-3 game frames and a
  model's id changes with the camera, so a face that stayed on the same pixels had two ids and lost the fix for
  that frame. Back-facing
  triangles (double-sided faces seen from behind, like that stand lid seen from below) store nothing and draw as
  before. Object vs object gap: `RUGRATS_OBJGAP` (camera units, default 24; the stored value is pushed back by room
  gap - object gap). D3D11: a TW triangle is its own run (colour draw `ps_to` with the depth + oz check, then an oz
  draw `ps_ow`, blend MAX); room runs use `ps_rt` / `ps_rh` / `ps_r` (skip those object pixels; near limit in vertex
  flags bits 16-28) and an oz mark draw (`ps_om` / `ps_omh`, blend MAX; with the span rule a second one, MIN, pass 3), fill runs get an oz
  clear draw (`ps_oc`; room runs too with `RUGRATS_ROOMOBJ=0`). GP0 log: `depth test+store`.
- **Sub-pixel 3D** (`subpixel` in `[video]`: 0 Off, 1 Near only = default, 2 On; F1 row "Sub-pixel 3D").
  Near only skips vertices deeper than SZ 16000 (`RUGRATS_SUBPXZ=<SZ>` changes the limit): separate meshes that
  meet in the distance can be up to a pixel apart, which the PS1's whole-pixel snap hides but exact positions show as
  a seam (e.g. water showing under the far wall in mini golf). On = every vertex; its seams are closed by the crack
  fill: opaque scenery faces (not world objects, every corner a sub-pixel GTE point) grow outward by 3/8 native px,
  ramped in from depth 4000 to 8000. `RUGRATS_SUBPXGROW=<d 1/256 px>,<z0>,<z1>` (`0` = off). Since 2026-11-11 the
  face's OUTLINE grows (`grow_ring`, `RUGRATS_GROWPOLY=0` = old corner moves): every outer edge moves out by d along
  its whole length; a corner whose two moved edges would meet more than 2d away is cut flat (bevel) instead of a
  spike. A quad grows as one outline, cut along its diagonal so both halves share the cut points (`clip_side`); each
  half takes colour, depth and texture from its own triangle's planes (`tri_attr`). Drawn hi-res only as a fan;
  native VRAM keeps the game's polygon. The old rule moved a thin strip's sharp tip by d in x and d in y, which
  tilted the strip's long edge instead of moving it: blue cracks between far ground strips stayed open. Hi-res-only
  pieces (grown / reprojected) carry `zs` > 0 (their source triangle's depth slope): the CPU renderer skips its
  "zero area at whole native pixels" rejection for them (thin pieces were dropped there: 1-pixel slits) and uses
  `zs` for its slope bias. Debug: GP0 log `grow: tri n k corners`, `RUGRATS_TRIDBG=<hi-res x>,<y>` (CPU renderer:
  every triangle touching that pixel, its edge values and texel). The GTE also computes each
  projected vertex from the unrounded camera-space x/y/z with an exact divide and records the fraction the PS1
  drops (1/256 px), keyed by the packed screen X/Y word. When a polygon packet's vertex word matches a projection
  from this or the last frame, the shadow draws it at that sub-pixel position. Removes the wobble/jitter at 2x-4x;
  native VRAM stays exact. 78-99% of 3D vertices match (misses are 2D/HUD or words the game edits).
  **Split midpoints** (`RUGRATS_MIDSUB=0` = old): the game splits near polygons in 2D (`800120fc` / `80011854`) and
  rounds each edge midpoint to a whole pixel, so a split edge sits up to half a pixel off the same edge of the
  unsplit neighbour (a T-junction). At hi-res that opened thin cracks where the scene behind showed through (a seam
  down the pillar wall by the house boxes, a dashed line on the teal wall by the stairs). `subpx_mid` now gives the
  midpoint the exact average of its two ends as drawn (their sub-pixel positions where they have one). Side effect:
  textured split walls line up better (toy store brick mortar no longer steps at split edges). Needs Sub-pixel 3D on.

**GPU renderer** (`renderer=1` in `[video]`, default; used at every internal resolution including 1x; F1 row "Renderer"; `RUGRATS_RENDERER=0/1`;
`runtime/gpu_d3d11.c`): the hi-res VRAM is a D3D11 render target (RGBA8, 5-bit values / 31, mask bit in alpha)
instead of the threaded software shadow. Every polygon, sprite, line, fill and upload is drawn into it by a pixel
shader that does the PS1 texel / CLUT / texture window / shading / dither logic, reading textures from a copy of the
native VRAM (R16_UINT, dirty rows re-uploaded at each flush). Draws are batched; a native write into an area that
batched draws still read flushes first. Semi-transparency modes 0/1/3 use dual-source blending (opaque texels stay
opaque); mode 2 (subtract) draws twice (opaque texels, then reverse subtract). VRAM copies are GPU copies.
Not emulated on this path: the mask check bit (logged once if the game uses it). Falls back to the CPU renderer
without D3D11 or when the target can't be created. The F1 menu and test shots read the target back (slow, fine there).
Measured 2026-11-06: cost no longer depends on the internal resolution (4x and 9x the same, ~2k vertices per
frame, GPU time negligible); CPU-side frame time ~20-30% lower than the CPU renderer. Output matches the CPU
renderer except edge/texel rounding (0.1-3.7% of pixels, mostly polygon edges). `RUGRATS_GPUSYNC=1` waits for the GPU
each present and logs flushes / vertices per present.

**Native copy only where needed** (2026-11-07, GPU renderer only): the exact 1x VRAM on the CPU is no longer drawn
for every primitive. It is only needed when the game reads pixels it drew (as a texture / CLUT, a VRAM copy source or a
VRAM->CPU read); Rugrats only samples a small patch at VRAM 0,0 in a few levels. Primitives are rastered on the CPU
only inside "hot" rectangles (seeded with 0,0-15,31, plus any area caught being read at run time, max 16, then it
falls back to the full copy). Areas drawn only on the GPU are tracked in 16x16 tiles; a read of one first copies the
pixels back from the GPU target. Save states and renderer / resolution changes copy everything back first. The
emulated GPU drawing time is charged exactly as before, so game timing is identical (checked: screenshots identical
to the full copy in boot/menus/hub and the 5 levels that read the patch). `RUGRATS_NONATIVE=0` = old full copy,
`=1` = none (debug, reads see stale pixels), `RUGRATS_NATIVELOG=1` logs reads of GPU-only pixels.

Measured 2026-11-06 on a Ryzen 7 5800X3D (14 workers), house hub while walking, unpaced with `RUGRATS_NOPRESENT=1`
(CPU side, worst of the run): 4x ~8.5 ms per frame (~115 fps), 6x ~16 ms (~60 fps), 9x ~33 ms (~30 fps). Quiet
moments are much faster (4x ~1.9 ms). Above 4x the raster workers are the limit; the auto frame rate lowers fps. In a window the real limit is the display
(DWM frees a buffer once per refresh). Older numbers (4x ~11 ms, "about 90 fps") are obsolete.

**Frame overlap** (`overlap=1` in `[video]`, default off; F1 row "Frame overlap"): at 2x-4x each present shows the
frame from the previous present, so the raster workers finish it while the game runs on (only a partial wait up
to that frame's queue mark). If anything queued or written since then touches that frame's display rect (a
drawing area, fill, upload or copy overlapping it, e.g. single-buffered screens), it drains and shows the current
frame as before. Costs one frame of latency; worth it only above the display rate or on slower CPUs.

## Status

Working: boot, loading, controller, menus, cutscenes, gameplay, CD/XA streaming, memory cards,
widescreen (GTE), high frame rate, D3D11 vsync'd presentation, F1 options menu.
2D in widescreen: stretched by default; Display page "Stretch 2D" Off keeps it at 4:3 (see below), movies included.
Not done yet: pictures uploaded straight into VRAM outside movies (the white boot loading screen, .BS stills) stay
stretched with Stretch 2D Off. (MDEC is done: the THQ / Nickelodeon / n-Space logo movies and the .BS still screens play.)

Stretch 2D (Display page, ini `[video] stretch2d`, default 1 = On, `RUGRATS_STRETCH2D=0/1`; widescreen + GPU renderer
only): the PS1 picture is shown stretched to 16:9 and the GTE squeezes the 3D world by 3/4 to make up for it, so 2D
(HUD, menu text, hint screens) is 4/3 too wide. With Off, the D3D11 vertices of the game's HUD layers are pulled toward
the screen centre by the same 3/4, so 2D keeps its 4:3 shape while the world stays 16:9. The game draws each frame
in 6 layers, each started by its own DRAWENV packet at fixed RAM addresses (hw.c `layer_env`): 4 = backdrop (sky),
1 = world, 2 = HUD / text / full 2D screens, 3 = menu button icons. Layers 2 and 3 are squeezed; on frames without
room geometry (title, menus, loading screens: fewer than 64 GTE room points, `gte_world_n`) every layer is squeezed and
the sides are painted black (plain 4:3). The menu button icons (3D models, `8003f934`) are then projected without the
GTE squeeze instead of being moved outward. Native VRAM is never changed. Movies (logo movies, title attract demos,
credits; MDEC pictures uploaded straight into VRAM) are shown 4:3 with black bars while the movie player `8008b448`
runs (host_win32.c `frame_43`; `RUGRATS_FRAMELOG=1` logs each aspect change). `RUGRATS_LAYERLOG=<first>,<last test frame>`
logs every list packet with its layer and each frame's room point count.

## Debug

- `RUGRATS_TEST=<frames>,<every>` — unpaced run, writes `shots/fNNNNN.bmp` and VRAM dumps, auto-presses START.
- `RUGRATS_TRACE=1` — per-30-frame loop address + CD commands in `rugrats.log`. `=2` CD registers, `=3` pad/IRQ,
  `=4` frame rate / game speed / time-step stats every 2 s.
- `RUGRATS_FPS`, `RUGRATS_OC`, `RUGRATS_STEP` override the ini; `RUGRATS_SCRIPT=<start-until>,<walk-after>` and
  `RUGRATS_TESTBTN=<hex>` steer the test input; `RUGRATS_PADLOG=1` logs unusual pad exchanges.
- `RUGRATS_MENU=<frame>` (test mode): opens the options menu at that frame, steps through it, writes
  `shots/menuN.bmp`. Tests never write the ini unless `RUGRATS_MENU_SAVE=1`.
- `RUGRATS_PACED=1` (test mode): keep real-time pacing and vsync instead of running unpaced.
- `RUGRATS_SWITCH=<t60>:<internal>:<renderer>,...` (test mode): change internal resolution / renderer live, like the
  F1 rows (e.g. `7200:4:1,7300:1:0` = 4x GPU, then 1x CPU).
- `RUGRATS_GDI=1`: skip D3D11, present with GDI.
- `RUGRATS_DSLOG=1` logs Sony controller scan, connect and button changes; `RUGRATS_NODS=1` turns the
  native Sony reader off (`runtime/pad_dualsense.c`, Windows HID, rescans every 2 s).
- `RUGRATS_AUDIOLOG=1` every 2 s: audio pushes, device underruns/drops, queue, CD stream, clipping, presents;
  `RUGRATS_WAV=1` writes the SPU output to `audio.wav`. Audio gaps (device underruns / dropped buffers, each one an
  audible pop) are always logged as `audio gap: ...` (at most one line per 2 s).
- Sound debug: `RUGRATS_OUTWAV=<file>` raw stereo exactly as sent to the sound card (after rate control, before the
  volume); `RUGRATS_SPUSTEM=<file>` per-voice output (24 x int16 per 44.1 kHz frame); `RUGRATS_SPUPOP=<n>` logs
  per-voice sample jumps bigger than n; `RUGRATS_KOFFLOG=1` key-offs and key-on retriggers with level + ADSR;
  `RUGRATS_ADSRLOG=1` every ADSR register write.
- `RUGRATS_OVERLAP=0/1` overrides `overlap`; `RUGRATS_NOPRESENT=1` skips the final D3D Present (measure the CPU
  frame limit; nothing is shown). `RUGRATS_PROF` now also prints `late NN%` (frames shown via overlap).
- `RUGRATS_WIDECULL=0/1` overrides `widecull` (portal culling widened to 16:9, default on);
  `RUGRATS_CUTPROPS=0` turns off the cutscene-prop list (hooks.c `cutscene_prop`: while a streamed cutscene runs,
  the listed props keep the original 4:3 object cull, e.g. the prop beside the fridge that the fridge door swings
  through in Let There Be Light); `RUGRATS_OBJCULLLOG=1` logs objects whose cull differs between 16:9 and 4:3
  (object pointer in s0, world position at +0x18); `RUGRATS_OBJHIDE=<hex obj>` culls one object (debug);
  `RUGRATS_OBJCULLALL=1` logs every object sphere test, `RUGRATS_CHUNKLOG=1` every room chunk box test (debug);
- `RUGRATS_NEARSIDE=0` turns off the widescreen near-side rule (default on): the game hides any object whose centre
  is closer than the projection distance; in 16:9 big props beside the camera (kitchen counter) then vanished at the
  screen edge, so objects whose centre is outside the 4:3 view go to the normal sphere test instead. Their faces with
  a corner at the lens are skipped (`RUGRATS_NEARFACE=<SZ>`, default 48, 0 = keep), and the whole object is hidden
  while most of its faces are at the lens (camera inside it). `RUGRATS_NEARLOG=1` logs it all (debug);
- `RUGRATS_PAIRCULL=0` turns off the back-to-back panel fix (default on): signs built as two flat double-sided panels
  facing opposite ways (Reptar 2010 "L&K") are drawn one-sided, so each side shows only its own panel instead of a
  mix of both (half mirrored). `RUGRATS_PAIRLOG=1` logs the panels found (debug);
- `RUGRATS_PORTALPAD=<px>` (default 64, 0 = off): the room/portal test 80033278 gets this many px of slack around its
  parent rect when that rect is the whole screen (portals seen directly; nested levels unchanged), so geometry in
  front of a doorway (door jambs) no longer pops in at the screen edge. `RUGRATS_EYEOUT=0` turns off the
  eye-outside-room rule (default on: when a scripted camera, e.g. the space station elevator, sits outside the
  camera room's box, that room's portals pass with the whole screen rect, so the rooms next to it are not cut down
  to their portal's outline). `RUGRATS_OVERWALL=0` turns off the eye-over-the-wall rule (default on: while the
  right-stick camera's eye is above a portal's top edge, e.g. tilted high over the hedge maze, that portal keeps its
  parent rect, and a failing one still passes when it is within `RUGRATS_OVERWALLDIST` units of the eye in x/z
  (default 4096) or its room's box is on screen, at most 24 per frame; the game's own camera never triggers it).
  Only in the levels with a gap colour (the hedge maze; `RUGRATS_OVERWALL=2` = every level, debug): in the house the
  eye above the stairway portal let the whole upstairs pass and the picture broke into long lines.
  Also in Reptar 2010 (looking down past a corner building, the street around the corner showed black).
  Reptar 2010 also draws room and prop back faces in every frame (the facades have no backs or roofs; seen from
  the high stick camera the buildings read as solid boxes) and its black clear colour (missing roofs, past the
  level edge) is building blue 28,28,58; the low-wall rule (`RUGRATS_LOWWALL`) runs there too (rooms whose box is on
  screen pass even when their doorway does not, with the rect of that box only, not the whole screen, which broke the
  park; checked pixel-identical to forcing every room on)
  Draw packet ring (all levels): the game builds a frame's packets and ordering table in one ring with no overflow
  check (its own: 280 KB at 0x80193E50; next-free pointer 0x800DE3B0, +4 start, +8 end). A frame past the ring
  corrupts its own list ("DMA2 list loop" in the log, then a crash; Reptar 2010 with the extras reached 272 KB).
  Guest RAM is 8 MB here (the PS1's 2 MB + an extension the game never touches; exports hold the first 2 MB only,
  save states since 2026-11-18 also the ring, field `cpu.ram_ext`, because right after a load the game re-sends the
  previous frame's lists and they link into the ring; for older states without it those first-frame sends are
  skipped, `dma2_stale` in hw.c; before this fix every load drew one frame from stale ring memory, "DMA2 list loop",
  sometimes a screen full of lines) and at every frame start the ring is moved to 0x80200000, `RUGRATS_BIGRING` KB long (default
  2048; 0 = the game's own ring). Safety limits, scaled to the ring: back faces only while the frame is under
  `RUGRATS_PKTBUDGET` KB (1280; own ring 128), the extra-room rules pause 2 s after a frame over `RUGRATS_PKTROOMS`
  KB (1536; 200), past `RUGRATS_PKTHARD` KB (1792; 250) further room/object faces are dropped (0 = off).
  `RUGRATS_PKTLOG=1` logs each frame's use, each list send and the ring move. Saves made with this build point the
  ring into the extension, so older builds must not load them. (`RUGRATS_BACKFACEALL=0` / `RUGRATS_TREEBACK=0` / `RUGRATS_GAPCOLOR=0` = off).
  In those frames, in levels with a gap colour (the hedge maze), room faces seen from behind are drawn too (the far
  sides of the hedges; `RUGRATS_BACKFACE=0` = off, `2` = every level; such a face only shows where nothing near it is
  already drawn: its depth is pushed back by four depth gaps, so it can't paint over the front of the same wall, ledge
  or prop, e.g. black bars along the Reptar 2010 balconies, a grey bar at a storefront; `RUGRATS_BACKZ=0` = off,
  `=<n>` = n gaps) and the background clear takes the level's gap
  colour (hedge tops the game never
  modelled; Grandpa's Teeth = dark hedge green; `RUGRATS_GAPCOLOR=<rrggbb>` = that colour everywhere, `0` = off; the maze's
  night-purple out-of-bounds colour is canopy green in every frame).
  `RUGRATS_LOWWALL=0` turns off the low-wall rule (default on, same levels: passing portals keep the whole screen
  rect and rooms whose box, or the box of a room up to 6 doorways past them, is on screen pass, up to 12 rooms a frame, so rooms seen over a hedge are not cut to their
  doorway's outline). In the same levels back faces are drawn with the game's camera too: `RUGRATS_BACKFACEALL=0`
  = room back faces only with the stick camera over a wall (as before), `RUGRATS_TREEBACK=0` = no prop (tree) back faces.
  Those extra rooms are mostly behind nearer walls, so in the same levels the D3D11 renderer asks the GPU which
  room geometry chunks and objects are hidden (occlusion queries of their bounding boxes against the room depth,
  answers a frame late; unknown = visible, 30 frames visible after the last visible answer) and skips the hidden
  ones: `RUGRATS_OCCL=0` = off, `RUGRATS_OCCLCHUNK=0` / `RUGRATS_OCCLOBJ=0` = only that kind off,
  `RUGRATS_OCCLSYNC=1` = wait for each answer (repeatable tests), `RUGRATS_OCCLLOG=1`.
  **Extended View Distance** (F1 Enhancements, ini `[video] view_ext`, `RUGRATS_VIEWEXT`): 3 Full (no
  limit), 2 Medium (default since 2026-11-18: Full costs ~2x the CPU in long Reptar 2010 streets for little picture), 1 Short, 0 Off. The low-wall and over-wall rules only bring in or widen a room whose box lies
  within 12000 (Medium) / 6000 (Short) units of the camera eye; Off = no extra rooms (the game's own culling).
  The game's own room passes are never changed. Fewer extra rooms = fewer polygons; the far side then can be
  missing again (sky / gap colour over far hedges, far Reptar 2010 streets). `RUGRATS_VIEWDIST=<units>` overrides
  the distance (0 = no limit), `RUGRATS_VIEWDISTLOG=1` logs rooms left out.
  `RUGRATS_PORTALLOG=1` logs each test (with the frame number);
  `RUGRATS_WSX=<num>,<den>` changes the widescreen squeeze (debug: `1,2` makes leftover 4:3 culling obvious).
  `RUGRATS_WSIR=0` = hardware x clamp in widescreen (by default, points more than 32767 units to the side are
  projected from their real x in 16:9, so far scenery in the extra side strips is not squashed).
- `RUGRATS_SUBPX=0/1/2` overrides `subpixel`; `RUGRATS_SUBPXAMB=0` turns off the same-pixel check (two different
  points stored on one screen position in a frame get no sub-pixel position); `RUGRATS_SUBPXLOG=1` logs the match rate and a fraction histogram.
  Such a shared pixel keeps each kind's own position too: a wall polygon takes the wall point's (the near or far one,
  whichever depth it uses), an object polygon the object point's, and wall pieces scale the crack fill by their own
  depth. Before, a wall corner that a character's or a far wall's corner landed on lost its position in single
  frames and the whole piece jumped up to a pixel (castle wall bricks twitching while the camera or Angelica turns).
  `RUGRATS_SUBPXCLS=0` = old behaviour. Object points also keep one position per model (head, body, arms are
  separate models).
  Exact positions: most vertices don't need the pixel match at all. The game stores each projected point (`swc2`, or
  `mfc2` + `sw`) into a vertex buffer or the scratchpad and copies it into its packets with `lw` + `sw`. The
  recompiler tags those stores (`gte_swc2_sxy`, `gte_mfc2_sxy`, `tag_ld[]` + `subpx_tag_cp`): the stored word carries
  its point's fraction, copies of the same value keep it, and the GPU DMA reports which RAM word each packet word came
  from. A packet vertex whose word is still the tagged value uses that point's own position (about 90% of corners in
  mini golf). The rest (split midpoints, other writes) use the pixel table. This stops the jitter where two points of
  one model shared a pixel (Angelica's hair and face). `RUGRATS_SUBPXTAG=0` = pixel table only.
  The RAM tag table (`subpx_at`, 128K slots) probes up to 4 slots per address and never overwrites a live tag of
  another address while a free or stale slot is there; direct-mapped, ~25k live tags per 300 frames were evicted and a
  near wall vertex lost its tag in single frames (its splits fell back to 2D midpoints: the wall flickered while
  standing still). `RUGRATS_TAGPROBE=1` = direct-mapped; `RUGRATS_SUBPXLOG=1` also logs the eviction count.
  Only the lw / sw instructions in `recomp/tools/tagsites.txt` carry the tag (on every one it cost ~8% frame time;
  listed: ~3%). To refresh the list: build with `RECOMP_TAGALL=1`, play / run saves, and collect the
  `TAGSITE <lw> <sw>` lines from `rugrats.log` (lw 0 = register from mfc2).
  Split points: the room renderer splits near polygons in 2D (hooks.c 800120fc / 80011854, `subpx_split`). With
  perspective textures each split point is placed where the edge's 3D midpoint projects, `A + (B - A) * wa / (wa + wb)`
  (w = 4096/z; the near half of an edge covers more screen), with depth `2 wa wb / (wa + wb)`, because the game gives
  it the average texture coordinate of its ends. The 2D midpoint made split pieces disagree with the unsplit wall next
  to them: bricks and cap jumped out of line when the camera turned far enough for the game to split the near wall.
  `RUGRATS_PERSPMID=0` = 2D midpoints. The hook tags the split registers (exact ends from the caller's tags), and
  0x80010e14-0x80012820 (`TAG_RANGE` in recomp.py) tracks every lw / sw and register move, so the copies keep them.
  Each tag also carries its point's exact depth (GTE `SXZ`, split points their own), so nested splits never read
  depth from the shared pixel table (two game frames per refresh share pixels there), and perspective weights use it
  (`RUGRATS_PWTAG=0` = pixel table). Tag offsets are 32-bit: near the camera nested split points move hundreds of
  pixels, and a 62 px limit dropped them in single frames (the near wall jumped as the game re-split it). Every split
  point of the frame is also kept by its packed screen value (`mid_h`), so a copy the tracking misses still finds it.
  Texture coordinates: the game rounds split points' texture coordinates down to whole texels ((a+b)>>1, 80011a28);
  the hook on 80011a28 keeps the exact fraction per split point (`subpx_split_uv`, `RUGRATS_SPLITUV=0` = off) and
  exec_poly adds it under perspective textures. Pairing (`RUGRATS_SPLITQ=0` = old: last position split only): near
  walls are split in two levels (4x4 pieces) and the game runs all five position splits (parent, then its four
  children) before the five texture splits, so "last only" gave the parent's fractions to the last child's points
  and dropped the rest (panel edges kinked at the screen edge while walking, space station, 2026-11-13). Both splits
  of one quad run with r2 = its primitive in the packet buffer (the parent and its first child share one), so a
  texture split takes the oldest waiting position split with the same r2 (else the one just before). Fractions are
  keyed by position AND the game's rounded uv word: neighbouring quads split their shared edge at the same point
  with other texture coordinates (mirrored tiles). The table has 4-way buckets (two frames of split points; single
  slots lost entries). Checked with a planarity test (uv vs 3D points per quad): no quad worse, hundreds better.
  Debug: `RUGRATS_POLYID=1` (CPU renderer) paints each polygon one flat colour, the GP0 log prints `#<n> <colour>`
  and, for perspective polygons, `persp <w per corner, t = from the tag> uv ...`;
  a `RECOMP_WATCH=1` build logs stores to `RUGRATS_WATCH=<hex address>` during GP0-logged vblanks.
- `RUGRATS_RES=<1..9>` internal resolution, `RUGRATS_THREADS=<n>` shadow raster threads,
  `RUGRATS_PROF=1` logs per-frame timing (main thread, readout, present, waits on workers) every 300 frames.
  `RUGRATS_SLOWLOG=<ms>` logs each frame over that long as game / convert / show ms; `RUGRATS_DMATIME=1` the time
  spent sending draw lists; `RUGRATS_LATENCY=<1..3>` frames DXGI may queue ahead of the display (default 1);
  `RUGRATS_SAMPLE=<s>` samples the main thread after that delay into `samples.txt` (map with an unstripped link).
- `RUGRATS_FLIPLOG=1` logs every display flip (vblank, cycles) and every present (real ms, flips so far, vblank,
  cycles): shows how long each game/movie frame stays on screen. `recomp/tools/emuref/movieflips.lua` logs the same
  in PCSX-Redux (vsyncs where the picture changes).
- `RUGRATS_MDECLOG=1` logs MDEC commands and why each movie ended; `RUGRATS_PEEK=<hexaddr>,<words>` logs up
  to 16 guest RAM words whenever they change (checked per vblank). `RUGRATS_PEEKF=<hexaddr>,<words>` logs them at
  the start of every game frame (with the frame number and ticks), for high-fps motion checks.
- `RUGRATS_MOVERSMOOTH=0` turns off platform smoothing (default on: elevators and moving platforms, which the game
  moves in fixed 1/30 s steps, glide between those steps at high fps). `RUGRATS_TIMERLOG=1` logs every 1/30 s timer
  callback the game fires (frame, object, callback).
- `RUGRATS_LIGHTFIXES=0/1` sets the "Let There Be Light" Fixes option (Enhancements); `RUGRATS_FANSTILL=0` /
  `RUGRATS_NIGHTFURN=0` / `RUGRATS_HEDGELEAVES=0` / `RUGRATS_KITCHENARCH=0` turn off only its fan / furniture / hedge leaves /
  kitchen arch part (`RUGRATS_ARCHLOG=1` logs the arch load and first draws). `RUGRATS_FANLOG=1` logs every spinning-prop update
  (level, object, model, speeds, angles); `RUGRATS_NIGHTCLUT`, `RUGRATS_NIGHTDBG`, `RUGRATS_NIGHTLOG` are the furniture
  part's debug switches, `RUGRATS_NIGHTPOWER=0` keeps it dark after the power comes back in the ending
  cutscene (see "Let There Be Light" Fixes).
- `RUGRATS_AISMOOTH=0` turns off companion smoothing (default on: characters the game updates in a 1/30 s timer, like
  Chuckie following Tommy, get their running animation, position and heading drawn in between those updates at high
  fps; only the picture changes, the game's own state stays exactly as it was). `RUGRATS_AISMOOTHLOG=1` logs each
  smoothed draw.
- `RUGRATS_EYEBLINK=0` lets the characters' eye blink run once per game frame (default: at the original 30 Hz game
  rate, so blinks keep their length and frequency at high fps).
- `RUGRATS_NCLIPFIX=0` culls faces by whole-pixel screen area like the hardware (default, with sub-pixel 3D on: a face
  seen nearly edge-on uses its exact area, so thin faces such as the floor lamp's base don't blink in and out at high
  internal resolution). `RUGRATS_MTC2TAG=0` stops screen positions moved into the GTE with `mtc2` from keeping their
  sub-pixel position (needed by that fix).
- `RUGRATS_MOVIE=<n>` plays movie n (0 THQ, 1 Nick, 2 n-Space, 3-5 attract demos, 6 credits) in place of the
  first boot movie. `RUGRATS_POKE=<vblank>,<hexaddr>,<hexval>[;...]` writes guest RAM once (debug).
- `RUGRATS_LEVEL=<db>` boots straight into database (level) 1..31: the hub (db 1) runs for 30 vblanks, then
  `0x800C4918` (the level loop's exit variable, gp+0xa98) is set to <db>, exactly what the game's door message
  (`800771d4`) does, so the hub tears itself down and the main loop loads <db> (hooks on `80018c38` / `8006ee14`,
  `hooks_level_vblank`). The log shows `level: run db N` each time a database's level loop starts.
- `RUGRATS_GP0LOG=<vblank>` (or `<first>-<last>`) logs every GPU drawing command during that vblank (or range) (with drawing area, offset, texpage), plus
  each DMA2 list start (`dma2 list from <addr> (ra ...)`, one per DrawOTag) and packet address (`pkt <addr>`).
- `RUGRATS_RCAMEXACT=0/1` (default 1): the right-stick camera's rotation is rebuilt from the exact (unrounded)
  eye-to-target direction after the game's LookAt (`800211e8`), instead of from whole-unit eye and target. With
  whole units the direction zigzagged ~0.01 degrees frame to frame while walking or while the eye settled, shifting
  the picture a pixel back and forth (thin door frame edges flickered). `RUGRATS_RCAMLOG=1` also logs `rcame` (eye,
  distance, frame time) and `rcamm` (game's matrix vs ours).
- The game's MemAlloc failure (break at `80021948`) now logs the game's own message, the caller and the file id.
- `RUGRATS_BENCH=<settle>,<measure>` (60 Hz frames): unpaced run that waits for the target level's gameplay
  loop, settles, records the real time of each presented frame for <measure> frames under a fixed walk/turn
  script, saves `shots/bench_dbNN.bmp`, appends a row to `bench.csv` (or `RUGRATS_BENCH_OUT`) and exits
  (code 0 ok, 3 = gameplay never started within `RUGRATS_BENCH_TIMEOUT`, default 30000).
- `RUGRATS_GPUSYNC=1` waits for the GPU after each present (GPU renderer) so frame times include GPU work. Every
  1000 presents it also logs draw runs per present by depth mode and the run-merge triangle tests.
- `RUGRATS_RUNMERGE=0` (GPU renderer): old draw batching (a draw joins only the run right before it; object-sort
  triangles one run each). Default 1 merges draws that cannot touch the same pixels, which removes most render
  target switches (identical picture). `RUGRATS_RUNLOG=<n>` logs the run list of the first flush with at least n runs.

## Per-level performance test

`python -I recomp/tools/bench.py run` boots the build into every database (0-31), measures each, and writes
`bench/<date-time>/` at the project root (outside `build/`, so results stay comparable across builds):
`results.csv`, `report.md`, `shots/` (one screenshot per level and config, to check the level really loaded),
`logs/`. Options: `--configs gpu:4,cpu:4,gpu:9` (renderer:internal), `--levels 0-31` or `2,5,7-9`, `--fps`,
`--settle`, `--measure`, `--exe` (e.g. the test build), `--present` (real presentation instead of skipping the
final Present), `--baseline <old results.csv>` (adds +/-% per level). Compare later:
`python -I recomp/tools/bench.py report <new results.csv> <old results.csv>`.

How it stays usable in future builds: it only uses env vars and two hooks on fixed guest addresses (the disc is
fixed, so addresses don't move); nothing depends on the host code layout. If a future change breaks a run, the
row says `timeout`, `crash(code)` or `hang` instead of the numbers, and the log/screenshot show where it stopped.
Each run takes ~30-40 s (boot, level intro, 5 s settle, 30 s measure, all unpaced); 32 levels x 3 configs ~1 h.
Run nothing else heavy meanwhile. `contact_<config>.png` = one thumbnail per level, the quickest check.

Level names in the report come from the game's own table. The default `--levels playable` runs 28 databases;
these 4 are skipped (`--levels all` runs them anyway), because the released game can't play them from the hub:
- 0 Menus: no level loop (always `timeout`).
- 8 Ice Cream Mountain: LEVEL08.BIN stores its tables with 32-bit coordinates; every other level and the
  engine's loader (`8002a208`) use 16-bit. Parsed the release way, its room table pointer lands past the end of
  RAM (`0x80896c48`), so the level loop runs but draws nothing (black). Leftover data from an older format.
- 19 Be Bad: its level file (file id 453, table `0x800B2C40`, 8 bytes per id: sector, size) has no entry: not on
  the disc. The loader reads stack junk as the size and MemAlloc fails. A cut level.
- 22 Multiplayer Golf: from the hub it asks for file id 328, also not on the disc; the real game starts it from the
  main menu after player setup. The 9 holes (23-31) are benchmarked as single levels.
Toy Palace (6) has a short black stretch (~4 of 37 sampled frames) when the scripted walk triggers something; it
happens at native 1x on the CPU path and with or without the widescreen cull hook, so it is not a renderer bug
(probably an in-game fade). Baseline: `bench/2026-11-06-baseline/` (see its report.md). Run-to-run noise:
mostly within +-2% per level, a few up to ~6-9%; treat per-level changes under ~5% as noise.

## Memory card (the game's own Save Game / Load Game)

Standard 128 KB card image `saves\card1.mcd` (runtime `memcard.h`, BIOS file API in `bios.c`). Save from the
pause menu ("Save Game?"), load from the title menu ("Load Game") or the pause menu; 15 games per save file.
- **Fixed 2026-11-06:** saving said "Creating save game file failed" and Load Game showed no games. The game's PsyQ
  `firstfile` (`8009ac98`) walks the BIOS kernel device table in low RAM (pointer at `0x150`, size at `0x154`,
  0x50 bytes per device) to patch the "bu" device before calling the BIOS; the HLE BIOS left it empty, so the
  game never found its own file. `bios_init` now builds a table (tty, cdrom, bu) at `0x80007000`. Cards written
  before the fix hold a save file with only its 512-byte header (no games): just save again, it is reused.
- **Test:** `python -I recomp/tools/memcard_test.py` (4 runs in `build/mctest` with a fresh card: save new file,
  load, save a 2nd slot, overwrite; checks the BIOS trace, the game's own output and the card image; screenshots
  in `shots_N`). Passed 2026-11-06.
- Debug: `RUGRATS_MCLOG=1` (every card/event BIOS call with arguments, file names, results),
  `RUGRATS_GAMELOG=1` (the game's own debug printf to stdout), `RUGRATS_INPUT=<t60>:<hex buttons>[:<frames>],...`
  (scripted pad input for tests, replaces the default script; bits: start 8, up 10, right 20, down 40, left 80,
  triangle 1000, circle 2000, cross 4000, square 8000).

## Quick save / quick load (save states)

F5 saves, F7 loads, F6 picks the slot (1-4); the F1 menu has "Save slot", "Quick save" and "Quick load" rows for
pads. A box in the top-left corner says what happened. Files: `saves\rugrats_slotN.state` (~4 MB, written to a
.tmp file first, then renamed, so a crash never leaves a half-written slot). Memory card saves are separate and
unaffected. `runtime/savestate.c`.

- **Where you can save:** in gameplay only (any level, the house, golf). The request runs at the start of the next
  game frame; in menus, cutscenes, movies or the pause screen it answers "Can't save here". Loading works
  anywhere, also right after starting the game (title screen); during boot movies it waits until they end.
- **How it works:** the guest runs as native C calls, so the host call stack can't be saved. A save is only taken
  at the start of the frame pacing call (`8001ceb4`) while the guest call chain is exactly boot `800977e8` ->
  main `800185f8` -> main loop `80017c0c` -> level loop `80018c38` -> `8001d3e8` -> `8001d00c`; this is checked
  by walking the guest stack with each function's frame layout. The state is RAM, scratchpad, CPU + GTE registers
  and every device (scheduler, IRQ, DMA, timers, GPU + VRAM, CD-ROM, SPU + sound RAM, SIO, MDEC, BIOS HLE tables,
  hook timing). A load longjmps back to `state_boot` (dropping the host stack), restores everything, rebuilds the
  hi-res picture from native VRAM, and re-enters the chain: dispatch `8001ceb4`, then each time a function returns,
  continue at the guest return address in `ra`. Those return addresses are recompiler entry points
  (`RESUME_POINTS` in `tools/recomp.py`).
- **Why it keeps working in future builds:** the file holds only guest state and guest addresses (fixed by the
  disc), no host pointers or struct dumps. Each field is stored under a name (`cdrom.c.mode`, `spu.vo3.level`,
  ...) and found by name on load: new fields keep their value (logged as "no field ... kept"), removed ones are
  skipped, size changes copy what fits. A state is refused if it was made for a different game EXE (CRC32) or by a
  newer incompatible format (`STATE_VERSION`). Rules for future changes: keep `RESUME_POINTS` and the chain table
  in savestate.c; never reuse a field name for something else; add new device state with `ST()` in that device's
  `*_state()` function; bump `STATE_VERSION` only for header changes that old builds can't read.
- **Verified 2026-11-06:** save in the house, load in a fresh process at the title screen: the frame 300 frames
  later is pixel-identical to the original run (scripted input replayed). Same after rebuilding with an extra
  state field (simulated future build). Level 2 save -> walk far -> load jumps back. Load with the CPU renderer
  (GPU-made state) works. Saving on the title screen is refused. F5/F7 + message box checked in a real window.
- Not saved (harmless): the XA resampler's 3-sample history and reverb accumulators (the game uses no reverb),
  the hi-res picture (rebuilt), host timing/audio buffers.
- Test hooks: `RUGRATS_STATE=save<slot>@<t60>,load<slot>@<t60>,...` (scripted saves/loads by test time),
  `RUGRATS_STATE_SHOT=<n>` (writes `shots/state_save.bmp` / `state_load.bmp` n frames after a save / load; in tests
  a load also restores the scripted-input clock so both show the same moment).

## Options menu

F1 (or clicking a stick on an Xbox pad) opens it. Game code is paused while it is open. Keyboard: arrows,
Enter, Esc. Mouse: hover, click (left/right half of the value = down/up), right-click or wheel to change.
Pad: d-pad/left stick, A, B. All settings are written to `rugrats.ini` on close. To add a setting, add a row to
`menu_items[]` in `runtime/host_win32.c` (label, value list, ini key, apply function, page).

Pages (tabs): Display, Video (sub-heading Fixes), Audio/Input (sub-headings Audio, Input; Control Bindings...),
Game, Enhancements, Debug. A row's `section` field draws a sub-heading above it. The launcher's Settings tab mirrors the same
pages (not Debug: its rows are live game values, not settings), sub-headings and a Control Bindings dialog (`SETTINGS` / `SECTIONS` / `BINDINGS` in launcher.py). The actions (Quick save, Quick load, Resume, Quit game; page `T_ACTIONS`)
are pinned at the bottom of every page. Switch pages with Q / E, Tab / Shift+Tab, PgUp / PgDn, LB / RB (L1 / R1 on
Sony pads), a click on a tab, or the mouse wheel over the tab bar. The last page stays selected between openings.

Debug page (2026-11-23): "Puzzle pieces" (0..12), "Reptar bars" (0..99) and "Freecam" (below). Pieces / bars the player has, read from game RAM each time
the menu opens and written on every change; not in the ini. Before any level has run (title screen) both show "In game
only" and ignore changes. Progress RAM (hooks.c `hooks_dbg_*`): pieces earned bitmask `0x800C56BC` (s16, bit = piece
0..11), placed `0x800C56BE`; bars taken per level `0x800C56C0 + 4*db` (bit = that level's bar), spent
`0x800C5740 + 4*db`; bars held = taken - spent; 15 held + under 12 pieces = the next pickup spends 15 and earns a piece.
Fewer bars = spend held ones (spare first); more = unspend spent ones, then mark bars taken in db 8 (leftover level)
and db 19 (unused), so no real bar ever disappears from a level. Fewer pieces = unplaced pieces go first (highest
first), then placed ones; more = the lowest unearned pieces, unplaced. The memory card save keeps whatever is set.
Finished saves: bit 0x200 of `0x800C56A8` ("game beaten", set by the final level's ending `8007b748`; also sends the hub
TV straight to the final level) makes the piece pick `80091d50` accept an already-earned piece, harmless on the PS1
(only reachable with all 12). With pieces lowered here, the next earn re-picked an owned piece (no piece to place).
Hook `rt_hook_80091d50`: while fewer than 12 are earned, that bit is hidden for the pick only, so a missing piece is
always chosen (logs `puzzle: earned pieces ...`).
Freecam (Debug page, Off / On, not saved): detaches the camera; the player stands still because the game gets no pad
buttons while it is engaged (`host_pad_buttons`; only while the gameplay camera ran in the last 0.5 s, so menus,
cutscenes and the title keep their input). Up / Down fly along the view, Left / Right strafe, right stick or J L I K
look, R1 / L1 up / down, Cross held x4 speed, Square held x1/4 (pad and key bindings apply: `host_pad_raw`). The left
stick is analog (half-way = half speed, `host_lstick`); d-pad / keys give full speed. Speed and turning ease in and out
(0.12 s / 0.06 s), and forward + strafe together never go faster than full speed. How:
hooks.c "Debug Freecam" takes the gameplay LookAt call (`800211e8` from `8001e928`, same filter as the right-stick
camera), writes our eye to cam+0x14 and builds the matrix from the exact direction; `8002953c` puts the game's eye back
each frame, so turning it off returns to the normal camera at once. Starts from the current view, and again after a
level change or cutscene. No collision: the game's eye clamp is skipped (after the camera update it asks `8002b660`
which room holds the eye and, for none, pulls cam+0x14 back into the last room's box; while the freecam runs that call
answers with the last room instead, hook `rt_hook_8002b660`), so it flies through walls, floors and ceilings and is
drawn exactly where it is. Rooms render from the room the eye is in, or the last one it was in; inside walls you see
the void.
`RUGRATS_FREECAM=1` (on from the start, tests), `RUGRATS_FCLOG=1` (eye per game frame), `RUGRATS_LSTICK=<from>:<to>:<x>:<y>,...`
(test left stick, t60 clock, as `RUGRATS_RSTICK`), `RUGRATS_FCPOS=x,y,z,yaw,pitch` (start pose, degrees).
Test helper: `RUGRATS_MENU_KEYS=<keys>` with `RUGRATS_MENU=<frame>` (+ `RUGRATS_MENU_TAB=<page>`): one key per menu
step, U D L R = arrows, 0 = shot (`shots/menuN.bmp`), then the menu closes. Logs `debug: pieces ...` / `debug: bars ...`.

Music / Sound effects / Dialogue volume (Audio page, ini `[audio] music`, `sfx`, `dialogue`, 0..100, default 100;
`RUGRATS_VOLS=<music>,<sfx>,<dialogue>`): applied per SPU voice in the mixer. libsnd's sequencer only uses voices below
its reserved count (`SsSetReservedVoice(14)` at sound init, byte `0x800EAD8C`) = music; the game keys its effects and
ambient loops on voices it picks itself (15..23) = sound effects. CD/XA audio = dialogue, except while the movie
player `8008b448` runs (movie soundtracks mix music and speech: master volume only).

Chatter Frequency (Enhancements page, ini `[audio] chatter`: 0 Original, 1 Less often, 2 Rarely, 3 Off;
`RUGRATS_CHATTER`): the random voice lines during play come from one timer per level (`800818e0` -> e.g. `80080a20`,
timer `gp+0xe40` in 1/300 s of real time, reset to 6.0..19.6 s, some levels 0..6.8 s). Less often / Rarely stretch
each new gap x2 / x4, Off keeps the timer from running out. Cutscene and event lines are played from other code.

Restored Lines (Enhancements page, ini `[audio] voice_restore`, default 1 = On; `RUGRATS_RESTORE`): voice lines that
are on the disc but that the game never requests. Each level's `DATA/DBxx/SPEECHTB.DAT` maps line ids to clips in
`DATA/SOUND/XA/DBxx.ISF` (every clip on the disc has an entry). 160 ids (177 entries, ~7.6 min) are requested by no
call site, no id list in the EXE and no level trigger or hint object (`restore_ids[]` in hooks.c). Hook on the voice
player `8008da4c`: only while the chatter function runs, half of its picks become a restored line that the current
level's table holds and that has not played yet this visit (played bit 0 of `gp+0x778[index]`). Once all of them
have played they repeat: the hook clears the pick's played bit (the game refuses a line whose flags are exactly 1),
never picks the same line twice in a row, and swaps less often (nc / (nc + 8) of picks for nc lines). Then the
game's player runs as usual (skips while a line plays, Dialogue volume). Where the playable baby changes inside a
level, `restore_speaker[]` (sorted by ear) keeps each line to the baby being played: `0x800C5680` = player object
`+0x15c`, 1 Tommy, 2 Chuckie, 3 Phil, 4 Lil (6 Angelica, 9 Reptar). Toy Palace (db 6): Tommy half / Chuckie half,
6/1135 left out (a copy of a line the game plays). Touchdown Tommy (db 21): its own chatter mixes all babies
whoever is playing, so its restored lines do too; only 360 is left out (misplaced). `RS_ANY` = a line for any
baby in a level that otherwise sorts by speaker. The level comes
from `0x800C5678` as 800818e0 reads it. `RUGRATS_VOXLOG=1` also logs each change of the playing character. Levels without a
chatter function have none of their own. Restored Lines adds a timed one (`hub_chatter` in hooks.c) to the hub
(db 1, its 6 Tommy lines) and Egg Hunt (db 18: 686 687 688): after a gap of game time it plays one of that
level's restored lines (unheard first, then repeats; no other line). Gap: hub 60..120 s; Egg Hunt the game's own
level chatter gap, 6.0..19.6 s (x2 / x4 with Chatter Frequency Less / Rare). Off with Restored Lines Off or Chatter
Frequency Off; the hub's also with Unused Hub Lines Off (Enhancements page, ini `[audio] voice_hub`, default 1 = On;
`RUGRATS_HUBLINES`); a line already playing delays it 5 s. Still unheard: Cookie Race (687 688, no chatter). 1135 is one
of Mr. Friend's own clips (1135..1146): it only stands in for one of his chase lines (1136..1146, which The
Mysterious Mr. Friend's chatter 80080e08 asks for only while he is after the player), never as ambient chatter
(`RS_CHASE`). The Return of Mr. Friend runs with the hub's voice bank, so none of his lines exist there. Incident on
Aisle 7: during the pie gallery (level word `0x800C56A8` & 8) its chatter 80080330 asks for one of the deli clerks'
8 lines (list `0x800B92DC`) every 6.7 s; the clerks' 4 restored clips (1073 1080 1083 1084) only stand in for one of
those, and Tommy's restored lines (103 215 220 221 224 230 231 233) never do (`deli_line`). Circus Angelicus: Phil's
762 plays when the trike lands on the target (the game has no line there): the hit result (`8007db6c`, event 57) is
held until the line has played (after a ride line still playing; 5 s at most), since the success cinematic takes the
CD drive at once; the miss result (`8007db90`) is not taken meanwhile. 33 97 141 (copies of No More Cookies' lines)
and 762 are not used as Circus chatter. No More Cookies has chatter (8008046c), so its 20 lines come
through the normal swap. Golf holes use
Ice Cream Mountain's bank (db 8); its 27 restored lines are not mixed into golf chatter, they play at their own
moment (Golf Reactions below). Debug: `RUGRATS_VOXLOG=1` logs every voice request and each swap.

Golf Reactions (Enhancements page, ini `[audio] voice_golf`, default 1 = On; `RUGRATS_GOLFVOX`). When the ball drops
(`8007a9d8`, holes db 23..31) the game asks for one reaction line and in the same frame starts the next hole (its
scorecard music takes the CD drive) or the "You were over par" screen (pauses the CD), so in the original these lines
are never heard; only hole 9's announcer at or under par plays. The hook picks the line (golf character = player
`+0x15c`: 1 Tommy, 2 Chuckie, 3 Angelica, 4 Lil, 5 Phil; strokes vs par): hole in one Tommy 70, Angelica 586, Phil
812; birdie Tommy 73 (restored), Chuckie 338, Angelica 589, Phil 811, Lil 939; other results at or under par the
game's own list for that baby (none for Tommy); bogey Chuckie 336, Angelica 587, Phil 809; double bogey Tommy 72,
Chuckie 337, Angelica 588, Phil 810, Lil 938; worse: the game's fail list for that baby plus the restored announcer
lines 1211 1212 1218 (and Angelica's 524 593). It starts that line (after one still playing, if any) and runs the
hole finish only when it has ended plus 0.2 s (10 s safety), with the original's own voice requests muted and the
pad ignored meanwhile. No line for the result: the game goes on at once. Hole 9 at or under par is left alone.
Ball rolling (`8005fcb8`, stroke counted at launch): 1 swing in 3, 0.3 s later, the baby's rolling line (Tommy 270
82, Chuckie 347, Angelica 598, Phil 820 868, Lil 949 995; unheard first, never the same twice in a row) if nothing
else is playing. Off: original.

Level Select (Enhancements page, "Level Select..." row): a list of the 28 playable levels (game database numbers;
not 0 menus, 8 Ice Cream Mountain, 19 Be Bad, 22 Multiplayer Golf, see "Per-level performance test"). Going there
(Enter / A / Cross / click) closes the menu and, at the next vblank, sets the level loop's exit variable `0x800C4918`
to the level (`hooks_level_request`, same path as `RUGRATS_LEVEL` and the game's doors): the current level ends without
saving and the chosen one loads with its normal title card / intro. Only during gameplay (`hooks_gameplay()`: level
loop running gameplay frames); on the title, in cutscenes or the pause screen the panel says so. Keys: Up/Down,
Left/Right or PgUp/PgDn (page), Home/End, Esc / B / Circle back; mouse click / wheel / right-click back. Test:
`RUGRATS_MENU=<frame> RUGRATS_MENU_LVL=<db>` (opens it, selects the level, writes `shots/menu0.bmp`, presses Enter).

Fixed minigame cameras: when the game's camera is in mode 4 (`cam+0x4a`, a fixed staged camera; Incident on Aisle 7
pie gallery, Circus Angelicus diving board) the right-stick camera stays off and forgets its turn, Override Camera
included. In those scenes the player's facing / aim follows the camera, so a kept stick turn faced Tommy away from
the targets (`rcam_fixed()` in hooks.c).

Camera cuts with a direction held: on a camera cut the game keeps steering by the old camera (copied to `0x800E31C8`,
flag `player+0x188` bit 0x800, set by `80062a08` from the camera-zone code `80027304`) with left/right mirrored, as
if the view had turned around, until every direction is released. With the right-stick camera in charge the cut never
reaches the screen, so Left/Right were reversed while held (also right after loading a save state taken in that
state). The movement hook (`rt_hook_8005be24`) drops that flag while the right-stick camera owns the view.
`RUGRATS_RCAMLATCH=0` = old behaviour.

Override Camera (Enhancements page, ini `[input] rcam_override`: 0 Off = default, 1 On; `RUGRATS_RCAMOVR`): the
right-stick camera is off in the levels listed in `rcam_excluded[]` (hooks.c; level = database number at
`0x800C5678`; now only 21 = Touchdown Tommy), and the game's own camera runs there. On = the right-stick camera works in
those levels too. Both camera hooks (`8002953c`, `800211e8`) ask `rcam_on()`. To exclude another level, add its number
to the list.

Wall Bonk (Enhancements page, ini `[game] wall_bonk`: 1 On = original, 0 Off; `RUGRATS_WALLBONK`): player state byte
`obj+396` = requested state, `+397` = current, `+0x658` = per-frame handler; `8005b2d8` switches states through the
jump table at `0x80013FAC`. Walking (state 2, body `8005e1f4`) into a wall with the stick held requests state 9 = bonk
(anim 6, sound 8 for Tommy, handler `8005d030`, ~0.4 s without control). Off: a wrapper hook on `8005e1f4` changes
that request to 0 (idle), so the player just stops; holding into the wall stays idle, no repeat.

Walk After Shooting (Enhancements page, ini `[game] shot_walk`: 1 Fixed = default, 0 Original; `RUGRATS_SHOTWALK`,
debug `RUGRATS_ANIMSPEEDLOG=1`): the animation speed `obj+0xe4` (4096 = 1x, set by `80057214`) drives both animation
layers. Firing while walking (Angelica's laser) sets anim 0x27 at 0x2000; when the shot animation (0x32) ends the walk
anims resume at that speed, so the walk plays at 2x until the player stops (same in PCSX-Redux). Fixed: a hook on
`80057214` notes the shot's speed write, and the walk body hook (`8005e1f4`) puts 0x1000 (0x800 for model 8) back on the
first walking frame where neither layer (`obj+0xa8`, `obj+0xb4`) plays 0x27/0x32.

"Let There Be Light" Fixes (Enhancements page, ini `[game] light_fixes`: 1 Fixed = default, 0 Original; an older ini's
`fan_still` key is used when `light_fixes` is missing; `RUGRATS_LIGHTFIXES`). Four parts, all only in db 12:

Kitchen arch part (2026-11-24, debug `RUGRATS_KITCHENARCH=0` = this part off): the hub has an arch piece over the
kitchen -> hall doorway (a world object, hub model bank id 0x17a, in room 27); db 12 uses the same house but never
places it, so the top of that doorway is open and the clear colour shows through (same on a PS1). Fixed: the model is
read once from the user's disc (DB01MOD.BIN through the game's file table), copied to 0x807FF500 in the port's RAM
extension with a small object at 0x807FF400, and drawn through the game's own object draw right after db 12 draws
room 27 (hook on the room draw `80033818`). Its three one-colour palettes get db 12's night versions, the ones db 12's
own room polygons use in place of the hub's (55dd -> 4cdd, 559d -> 4c9d, 561d -> 4d1d). Not drawn once the power is back
on in the ending cutscene.

Hedge leaves part (debug `RUGRATS_HEDGELEAVES=0` = this part off): walking into a backyard hedge spawns leaves (the
walk's puff `80050b5c` looks up model 0x3f0 with `80044b74`). db 12's model bank has no 0x3f0, and the lookup's
not-found path returns the bank's first model, small red / green / blue axis markers (game data: PCSX-Redux shows
the same fragments). Fixed: while the hub runs, its leaf model (a textured 4-vertex quad) and its 16 colours are copied
(kept in save states); in db 12 the leaf lookup gets that copy at 0x807FF000 (the last 4 KB of the port's 8 MB RAM),
pointed at db 12's identical leaf palette (0x65dc) with the night tone on its face colours. db 12's VRAM holds the same
leaf texels. If the hub was not seen yet this run (a state loaded straight into db 12 from an older build) the leaves
are dropped instead of drawn as markers.
Separately (all levels, not part of the option): the port spawned one leaf per drawn frame while walking, ~8x the
PS1's rate at 120 fps; now one per 2 PS1 game frames (`RUGRATS_LEAFRATE=0` = old).

Ceiling fan part (debug `RUGRATS_FANSTILL=0` = this part off, `RUGRATS_FANLOG=1`): Let There Be Light (db 12) is set during a power cut, but the original keeps the kitchen ceiling
fan turning. The fan is a generic spinning prop: its 1/30 s timer callback `8004e2ac` adds the s16 speeds
`obj+0x2c/+0x2e/+0x30` x vblanks to the angles `obj+0x24/+0x26/+0x28`. Fixed: a hook zeroes those speeds for model 94
(`obj+0x96`) in db 12 only, so the blades keep their spawn angle; other spinning props and other levels are untouched.
Switching back to Original in the level gives the fan its speed again. Idea by flamespeedy.

Furniture part (debug `RUGRATS_NIGHTFURN=0` = this part off,
`RUGRATS_NIGHTCLUT=<hex CLUT>[:<hex source CLUT>],...` replaces the list, `RUGRATS_NIGHTDBG=1` paints the listed
palettes red, `RUGRATS_NIGHTLOG=1`): Let There Be Light (db 12) is dark because its palettes (CLUTs) are dark; room and
prop polygons are drawn at the neutral colour 0x808080. A few kitchen furniture textures kept daytime palettes on the
disc, so they glow next to the room; the PS1 shows the same (VRAM equals the disc bytes, nothing is darkened at run
time). Fixed, while db 12 runs: the centre drawer front and the orange cabinet doors get the palettes of their own
night versions elsewhere in the kitchen (the other drawers, the counter doors: same texture layout, so they look exactly
like those), copied in VRAM; the stove front and the fridge doors / side edges are scaled (red and green x 0.30, blue
x 0.45); so is the light teal door panel (6edd, a room texture). Untextured props are darkened in their model data in
RAM, found through the object draw hooks: the bathroom trash can (model 81, colour table at model header +0x18), the
potted plants (models 949 / 950 / 951, flat-face colours of pot, soil and flat leaves; stems are already dark), the
door knob (model 527), the coffee tables (model 132), the garage cardboard boxes (models 961 / 962), the stair
handrail posts (model 1271), Tommy's bed sheets (model 1092), the purple toy on his toy chest (model 383, flat faces)
and the Reptar doll on his nightstand (model 1130, plus its texture CLUTs 551d / 6d5d / 6d9d / 6ddd; the nightstand
itself is CLUT 6d1d), the small green lamp on the dresser (model 388) and the underside of the kitchen ceiling fan
(model 94: orange light cover and grey), the golf clubs and bag (model 964) with the trash can in front (model 263),
in the laundry the washer and dryer (models 958 / 959), the sink (963) and the bench (960), the kitchen hood (239), the
living room floor lamp stand (138), the inside of the fireplace (953), the ball (954) and the frame of a side table
(134; its drawer fronts are a dark texture already). A model with more than
128 colour words takes several rows (the golf clubs: two). Original puts
the saved colours back. Save states keep the model rows by model id (`hooks.nm3_*`, fixed 32 x 128 layout); states
from the two earlier test layouts are converted. Uploads over a listed CLUT re-arm it, other levels are never touched,
save states keep the state (older states are handled). (The range hood "not covered" note of the first version is out of
date: it is model 239 above.)
Power back on: the level's last cutscene (Triangle on Spike: the fridge opens, then the power comes back) cuts to a lit
copy of the kitchen, camera room 44 (first byte of the room queue `0x800E00C0`; the dark kitchen is room 19, gameplay
never uses 44). While the camera room is 44 every darkened palette and model colour is put back to its vanilla value,
so the lit room looks exactly like Original (checked pixel for pixel to the end of the level). The check also runs from
the object draw hooks, because the timer pass that normally drives this part does not run in cutscenes. Debug:
`RUGRATS_NIGHTPOWER=0` keeps the furniture dark there. The ceiling fan part also stands down in room 44 (fan counts as
powered), but the fan is never drawn or ticked after the cut, so nothing changes on screen.

Final Level Behavior (Enhancements page, ini `[game] final_level`: 1 Enhanced = default, 0 Default = original;
`RUGRATS_FINALLEVEL`). Enhanced = four parts, all on or all off: (1) stay in the level after the last piece, (2) a
"Final level unlocked!" screen, (3) the hub TV as the final-level entrance, (4) progress kept after the credits.
Default = the original game.

(1) Back to level: taking the Reptar bar that earns the 12th puzzle piece runs the "You earned a puzzle piece"
screen, the puzzle (`80092c68`) and the "You did it!" screen inside that frame of the level. Then the game sends
`800781f8(player, 7)` = "leave for database 7" (the final level), from the end of the puzzle (ra `80092d7c`) and from the
bar pickup (ra `8005180c`). The message's handler `800771d4` writes the database to `0x800C4918`, which ends the level
loop. A hook on `800781f8` drops those two db-7 messages only, so play goes on where the bar was taken.
Levels that end some other way (Mirrorland's last balloon etc.) earn their piece in the level-complete handler
`80076d6c` (or the main loop's level exit `80018208`), which also runs the puzzle through `80094978` ("all 12 just
earned?", which also shows "You did it!") and on 1 sends db 7 (ra `80077030` / `80018364`) in place of the level's
normal exit. A hook on `80094978` makes it return 0 to those two callers (ra `80076ea8` / `80018318`), so the level
ends as it does for pieces 1-11 (Mirrorland: "Returning to Pickles' Home", hub). `8004d414` is not touched. Note: with this on, the final level is not reached
through the puzzle (use the TV, Level Select, or set the option to Default).
Puzzle facts: piece positions (x, y halfwords, 8 bytes each) at `0x800E7260`, homes at `0x800BB38C`, start spots at
`0x800BB32C`; d-pad moves the held piece 7 (x) / 3 (y) per game frame; X places it if
((home x - x) * 320 / 512)^2 + (home y - y)^2 < 101, else it goes back to its start spot.

(3) TV to the final level: once all
12 puzzle pieces are in place (`80094b8c()` == 12), the TV in the house hub works like the game's own level entrances:
walk up to it for a red "Final / Reptar 2010" label with sparkles, press Triangle to play Reptar 2010 (db 7). The TV keeps
its own hint. Its label only shows while no other entrance label does (the Egg Hunt basket is next to it). How: hooks.c
`tv_frame()` (once per game frame from the timer-pass hook) gives the TV object the game's entrance handler `8004f78c`,
db 7 at obj+108 and the label callback `80049eec`, and starts / stops its label timer by distance (2500, same room); the
label hook points the level-7 difficulty case at the red "Hard" case and shows "Final" in that string's slot, for the
TV's calls only. Debug: `RUGRATS_TVFORCE=1` (unlocked without the pieces), `RUGRATS_ENTLOG=1`.

(4) Keep progress after the credits: the final level's ending sets the credits byte `0x800C3E80` and leaves the level
loop with next db -1. For db -1 the main loop wipes the progress block inline (`80018264..800182ac`: flags incl. 0x200
"game beaten", puzzle pieces earned / placed, Reptar bars taken / spent), plays the credits, and goes through the title
front end `80030158(-1)` (which, without a memory-card load, also runs the new-game reset `80094cfc` -> `80091a44`).
Enhanced copies the block (+0x34..+0x147 and the flag word +0x30 except 0x400) at the level exit (hook `8006ee14`, only when the credits
byte is set), puts it back right after the wipe (hook `8008a7ec` from ra `800182b8`) and skips the title's new-game reset
once. Quitting to the title and a later New Game still reset as before. Flag 0x10 opens the hub entrances to db 3, 4
and 6 (`8004f78c`, else hint 0x2c8 "you'll need more puzzle pieces"); the level-complete handler `80076d6c` sets it
when db 15 is finished. Saves from the first version of this part (it kept only 0x200) lost it: with Enhanced, a beaten
game (0x200) with all 12 pieces earned gets 0x10 back at each level start and on Triangle at an entrance.

(2) Final level unlocked: the "You did it!" screen is followed by a second
screen, "Final level unlocked!" (yellow) / "Go see what's on TV!" (pink); X returns to the level. How: hook on the
puzzle message screen `80093048` (fade in, wait for X, fade out), only from `80094978` (ra `800949d0`). After the
first run it points the line objects (`0x800E7300`, 3 x 0x2c bytes; drop shadows at `0x800E7390`; +0x14 string,
+8 x, +0xa y) at its own strings, hides line 3, re-centres with `8002e144(obj, 1)` and runs the screen again. The
strings borrow 64 bytes at `0x80015EBC` (the "You did it!" / memory-card texts), restored afterwards.
Line text: `0x800BB4E8 + 4 * id` (228 "You did it!", 230/231 the two pink lines; set up by `80094178`).

Turn step (always on; `RUGRATS_TURNFIX=0` = original): in the player movement `8005be24` the turn step is
rate x ticks >> 6 (tick read at `8005c7ec`). If the wanted heading is reached within gp+0x44c after that step,
the turn target `obj+0x2c` snaps to it; otherwise `obj+0x2c += step`, built on its own last value, so it keeps
spinning and the facing `obj+0x24` (which eases toward it) wobbles. At the PS1's 10 ticks a full-stick turn always
snaps in its first frame; above 30 fps it did not (turning Spike around toward the camera: ~0.75 s of shaking in
place). That load goes through `rt_load_8005c7ec` (recomp.py `LOAD_HOOKS`, new: a listed load instruction's value
passes through a native function) and sees max(ticks, 10).

Collision probe look-ahead (always on; `RUGRATS_PROBEFIX=0` = original, `RUGRATS_PROBELOG=1`): the movement
collision probe `80064a34(obj, mode)` looks ahead by bbox extent + speed `obj+0x184` x ticks (gp+156), and in mode 2
its step-up check `800645d8` also scales by ticks. The PS1 never steps less than 10 ticks (30 fps); above 30 fps the
look-ahead shrinks. Found on a low ledge in the sewer: after the ledge hop (state 6, handler `8005f310`, anim 5)
Spike stands ~150 units short of the ledge face; the 10-tick probe point lands past that thin face (walks on), a
shorter one inside it (hit), so the walk body's low-speed branch (`80058da4`, `80062cf4`) restarted the hop from the
old start point `obj+0x104/0x10c` at the bottom: endless hop while the stick was held. The hook gives the player's
probe max(ticks, 10) for the duration of the call (real value restored after).

Step-down hop (2026-11-21, always on; `RUGRATS_HOPFIX=0` = original, `RUGRATS_HOPLOG=1`): walking down stairs (hub),
the movement step `800645d8` sets state 4 (hop, handler `8005f1b4`) when its floor probe sees the drop, ~150 units
before the edge. At 30 fps that frame also moves the player 170 units, past the edge. At 144 fps only 34: the hop's
collision (`8005ee78`, `80066014` result 2) finds ground at once, so a full landing (state 10) played on the upper step
and the real hop a few frames later: two hop + land animations per step (28 instead of 12 on the hub stairs; 60 fps:
some steps). The hook lets the walk -> hop change through only once the consecutive frames it fired add up to 10 real
ticks (tick ring `0x800CE0B8[gp+154]`, since gp+156 reads 10 inside the probe, see above); until then the object keeps
its state and walks on. 30 fps: passes on the first frame (unchanged).

Spinning props seen from another room (2026-11-21, always on; `RUGRATS_SPINFAR=0` = original, `RUGRATS_FANLOG=1` also
logs these as "spinfar"): the room system deactivates the objects of rooms the player has left (`80049e5c`: removes the
object's timer, clears obj+0 bit 0), and the spinner callback `8004e2ac` runs on that timer, so the hub kitchen ceiling
fan stopped while still in view through the doorway (walking away from the kitchen). Now, whenever an inactive object
with the spinner callback is drawn, its angles get what the callback would add (speeds x vblanks since obj+0x64) and
obj+0x64 is updated, so nothing jumps when the room comes back. The Let There Be Light still-fan rule still applies.

Damage cooldown (always on; `RUGRATS_DMGCOOL=<ms>`, default 400, 0 = original; `RUGRATS_DMGLOG=1` logs every
damage event): `80077864(msg)` is the damage event (msg+4 = object, msg+0x10 = other object, damaged when set,
msg+0x14 = amount, negative heals). It subtracts from health `obj+0x1a4` (s16, 0..255, 0 = `8007fd0c` death) and runs
the hurt reaction `8008c124`. The game has no invincibility time, so repeated contact with a hazard (sewer pipe in
7 Voyages of Cynthia: 15 per hit, Spike starts at 195) could stack hits. The hook drops a damage event for the player
(`0x800E2B48`) that comes less than 0.4 s of game time after the last accepted one (no damage, no knockback);
heals and other objects are untouched. Not saved in save states (a load clears the cooldown).

Note Fade-out (Enhancements page, ini `[audio] note_fade`: 1 On = default, 0 Off = original; `RUGRATS_NOTEFADE`):
the game's music engine releases notes with ADSR2 = 0000 (linear release rate 0 = -16384 per sample), so every note
end drops from full level to 0 in 2 samples, a click (PCSX-Redux does the same, so it is in the game). On caps one
release step at 256 (full scale to 0 in ~2.9 ms) for every voice; slower releases are untouched. Off is
bit-identical to the original behaviour.

Reptar Bar Sound (Enhancements page, ini `[audio] bar_roar`: 1 Chime + roar = original, 0 Chime only, 2 Off;
`RUGRATS_BARROAR`): picking up a Reptar bar plays sound id 0x1c through `8006f26c`, called only from `800517a8`
(bar collected, then the HUD counter update): program 15, one sample (at SPU 0x61b40 in one level, 0x512c0 in the toy store: each level loads its bank elsewhere) that holds both the chime
(rising tones, first ~0.42 s) and the roar (~1.2 s of noise). Chime only lets it start and has the SPU fade that
voice out from 0.42 s to 0.43 s after key-on (user-picked; `RUGRATS_BARCUT=<ms>,<ms>`; `spu_cut_next(voice, ...)`: the voice is taken from the `SsUtKeyOnV` call that sound makes in the same vblank, caller `800517b0`); Off
skips the call. `8007013c` from `8005ea28` (program 0, notes 0x1b / 0x19, SPU 0x2c950) plays around the same time
but is a different sound (it is the footstep). Debug: `RUGRATS_SFXLOG=1` logs every call of the effect wrappers (`8006f26c`, `8006f2fc`, `8006f3a4`,
`8006f438`, `8006f644`, `80070064`, `8007013c`) and of libsnd `SsUtKeyOnV` (`800a99a8`) with args and caller.
Cookie pickup (note for a possible "replace the roar with the cookie chime" option): pickup handler `800514b0` jumps
on item byte +0xa9 via table `0x8001389c`; types 4 and 14 go to `8005199c`, which plays sound 0x1d through `8006f26c`
(ra `800519a4`): program 15 tone 1, SPU 0x53ae0, a ~1.07 s rising chime. Swapping means changing a0 0x1c -> 0x1d in
`rt_hook_8006f26c` for the bar call (ra `800517b0`). Clip: `build/cookie_sound.wav`.

Pie gallery aim (Incident on Aisle 7, no option): left/right while aiming adds the same fixed +-25 nudge per game
frame in `8005be24` (level 3 with flag 0x8 at 0x800C56A8). The PS1 runs that scene at ~17 game fps (PCSX-Redux),
so the reticle turned ~440 units/s there but up to ~3600 at 144 fps here. The `8005be24` wrapper rescales it to
25 per 17 ticks (never more than the game's own step). Measured: a 40/60 s hold turns 294 units at 30/60/144/240 fps.

Aim lock turn speed (always on; `RUGRATS_LOCKAIM=0` = original, `RUGRATS_LOCKAIMLOG=1` logs each step): while the
player's aim is locked (`player+0x188` bit 0, e.g. Tommy aiming on the shuffleboard, database 2), `8005be24` turns
the target heading `+0x2c` by 1/8 of what is left (1/16 in fine mode) every game frame, capped by rate x tick >> 6.
Both are per game frame, so the aim settled 2x faster at 60 fps, 4x at 120 and 5x at 150 than at 30. The
`8005be24` wrapper rescales the step by the real time step against the PS1's own game frame (an eased step by
k(1 - ((k-1)/k)^(ticks/ref)), a capped step by ticks/ref). ref = 14 ticks on the shuffleboard (PCSX-Redux runs that
scene at ~20 game fps: frames of 15 and 10 ticks), 10 elsewhere. Measured holding Left: 30, 60, 120 and 150 fps
follow the PS1 curve (centre to the left edge in ~1.4 s; within ~5 units of 4096 after the first 0.3 s).

Goose feathers on the shuffleboard (always on; `RUGRATS_FEATHERS=0` = 30/s, `RUGRATS_PARTLOG=1` logs every particle
spawn): the goose's emitter (`8005389c` -> `80052b60` -> particle spawn `8008864c`) spawns one feather each time its
1/30 s timer fires, and the timer fires at most once per game frame. The PS1 runs that scene at ~20.7 game fps, so it
showed 20.7 feathers/s; the port showed 30/s at every frame rate. The `8008864c` hook (database 2, that call only)
lets a feather through only once 14.5 ticks (1/20.7 s) of game time have built up. Measured: 20.7 feathers/s at
30, 60, 120 and 150 fps.

Reptar's fire breath (Reptar 2010, always on; `RUGRATS_FIRERATE=0` = old): the breath (call at `8005ce64`) spawns one
fire particle per game frame through `8008864c`; each gets an animated sprite (`80086114` type 3 -> `8008a22c`,
sprite 0x1017) from a pool of 32 slots. The PS1 runs that scene at ~9.8 game fps (PCSX-Redux replay of a user save:
8 spawns per breath, one per game frame); the port spawned one per drawn frame (64 per breath at 120 fps). Past 32
the sprite allocation fails, and those particles drew with empty texture fields: texture page 0 / palette 0 / uv 0 =
the top-left screen pixels, additive, 100+ times = a white blob over the fire, and the game's 150-packet cap left
gaps where buildings and trees vanished. The same game-time gate as the feathers now lets one fire particle through
per 300/9.8 ticks (first one of a breath at once). Measured: 7-9 particles per breath at 30, 60, 120 and 144 fps, at
most ~50 fire packets (Redux: 42-45), no empty ones.

Golf aim speed (Game page, ini `[game] golf_aim`, default 100): in the golf holes the game turns the aim a fixed
step every game frame, so at 120 fps it turned 4x as fast as on a PS1 (30 game fps). The fix scales that step by the
real time step; the value is a percent of the PS1 speed (25 / 50 / 75 / 100 / 150 / 200), 0 = "Uncorrected" (old
frame-rate-bound behaviour). Debug: `RUGRATS_GOLFAIM=<percent>`, `RUGRATS_GOLFLOG=1`.

Skip intro (Game page, ini `[game] skip_intro`, default 0): boots straight to the title screen. The boot function
`800185f8` shows the legal screen (`80030010(0xea, 5)`, a still for 5 s) and plays the THQ / Nickelodeon / n-Space
logos through their movie wrappers `8008b230` / `8008b27c` / `8008b0b4`; hooks skip exactly those calls (checked by
return address). Attract demos and the credits still play. Takes effect at the next start. Debug: `RUGRATS_SKIPINTRO=0/1`.

## Discord Rich Presence

Your Discord profile shows **Rugrats: Search for Reptar**, the level you are in (e.g. "Toy Palace", "Golf: hole 3",
"In the menus") and the time spent in it. `runtime/discord.c` talks to the Discord desktop app over its local pipe
(`\\.\pipe\discord-ipc-N`) from a background thread: no SDK, no DLL, no network, and the game never waits on it. If
Discord isn't running it retries every 15 s. The level is the game's current database (`0x800C5678`).

It works out of the box: the game has the project's own Discord application ID (`1570819608427110562`, named
`Rugrats: Search for Reptar`, which Discord shows as "Playing ..."). Just have the Discord desktop app running.

To use your own application instead (e.g. for your own art), create one at https://discord.com/developers/applications
and put its **Application ID** in `rugrats.ini` (ini only; the launcher has no field for it):
```ini
[discord]
client_id=123456789012345678
enabled=1
; optional: art uploaded under Rich Presence -> Art Assets, by its key
image=logo
```
Empty / absent `client_id` = the built-in ID.

F1 -> Game -> **Discord status** turns it on/off (Off clears the status at once; `enabled=0` in the ini).
Debug: `RUGRATS_DISCORD_ID=<id>` overrides the ini; the log shows `discord: ...` lines (connected, status, errors such
as "Invalid Client ID").

## Mods

Cheats, RAM patches (GameShark codes too), replaced game files (whole files or BPS patches) and texture packs
(hi-res PNGs, GPU renderer; F1 Game > Dump textures writes the originals with their pack names) as mods: a folder
with a `mod.ini` in `mods\` next to `Rugrats.exe`. Modder's kit: `recomp/tools/modkit.py` (list / extract / TIM<->PNG /
BPS / pack).
Add and order them in the launcher's **Mods** tab, switch them in game with F1 > Game > **Mods...** (takes effect at
once; mods with files or textures at the next start). Enabled list: `rugrats.ini [mods] enabled=a,b` (later wins). Applied once per vblank; with no mod on nothing
runs. Save states store the mod list and say so when loaded under other mods. Format, code types and examples:
[MODDING.md](MODDING.md). Example mods: `recomp/mods/` (copied into the install, off). Debug: `RUGRATS_MODS=<id,id>`,
`RUGRATS_MODDIR=<dir>`, `RUGRATS_MODLOG=1` (replaced-file reads), `RUGRATS_TEXDUMP=1`, `RUGRATS_MENU_MODS=1` (menu test: Mods page shots).

## Backup / restore

- Backup: `backup/` holds a copy of the `.bin`/`.cue` and `SHA1.txt`.
- Restore: delete `recomp/` and `build/`. If the original fails its hash check, copy it back from `backup/`.
