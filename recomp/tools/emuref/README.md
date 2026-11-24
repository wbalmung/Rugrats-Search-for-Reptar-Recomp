# emuref: replay a quick save in a reference emulator (PCSX-Redux)

Used on 2026-11-06 to find the GTE screen-X overflow bug (pink wall slabs). It plays the exact same guest moment in
PCSX-Redux and in the native port, then compares pictures, RAM per game frame and every RTPS/RTPT.

The emulator is a debugging reference only. It is not part of the port and is not shipped.

## Setup (once)

- PCSX-Redux Windows build (free, GPL, has its own open-source BIOS, so no Sony BIOS is needed). Download the zip from
  distrib.app: manifest `https://distrib.app/storage/manifests/pcsx-redux/dev-win-x64/manifest.json`, then
  `manifest-<id>.json` gives the `path` and the sha1. Build 678 (2026-10-23) was used. Unzip it into a scratch folder.
- Redux writes its memory cards to `%APPDATA%\pcsx-redux\` (outside the project).

## Steps

Work folder `W` (any scratch folder, forward slashes for Lua), with `W/export`, `W/inject`, `W/emu`.

1. Export our state (debug env, see savestate.c `state_export`):
   `RUGRATS_EXPORT=<W\export> RUGRATS_EXPORT_N=12 RUGRATS_STATE=load<slot>@600 RUGRATS_TEST=... Rugrats.exe`
   -> `ram.bin, scratch.bin, vram.bin, regs.txt` (+ `ram_fK.bin` per game frame after the load).
   For a fair comparison use `RUGRATS_WSX=1,1 RUGRATS_SUBPX=0 RUGRATS_RES=1 RUGRATS_RENDERER=0 RUGRATS_FPS=30
   RUGRATS_TICKS=10` (the PS1 runs this game at 30 fps = 10 ticks per frame) and `RUGRATS_GTELOG=4000`.
2. `python -I mkloader.py <W>` -> `W/inject/blob.bin` (MIPS loader + our VRAM + GPU settings).
3. Copy `gtesites.txt` to `W`. Run (from the Redux folder):
   `INJECT_DIR=<W> INJECT_SHOTS=40 INJECT_GTELOG=4000 pcsx-redux.exe -run -debugger -iso <cue> -dofile inject.lua
   -interpreter -stdout -lua_stdout -no-ui`
   The script presses Cross/Start like our test mode until the game reaches its frame pacing call (8001ceb4), runs
   the loader (uploads VRAM through GP0), copies our RAM above 0x10000 + scratchpad + CPU/GTE registers, jumps to
   8001ceb4, then writes `W/emu/sNNN_*.raw` (one screenshot per vblank), `W/emu/ram_fK.bin` (RAM at each frame start)
   and `W/gte_emu.txt`.
4. Compare: `raw2png.py W/emu`, `sheet.py` (contact sheets), `ramdiff3.py W/emu <ours export dir>` (RAM by the game's
   frame index), and diff `gte_emu.txt` with our `gtelog` lines (Lua prints negative words as 16 hex digits: strip
   the leading `ffffffff`).

Options for step 3: `INJECT_PAD=<from>:<to>:<buttons>,...` holds buttons (names joined by `+`, e.g. `up+cross`) from
run vsync `<from>` to `<to>-1` after the injection; `INJECT_PEEK=<hex addr>` logs 3 words there every run vsync
(`peek run N: a b c` in inject.log; player position = `800E2B60`). Used 2026-11-06 to compare the trike jump distance.

`INJECT_BPLOG=<hex>,...` logs each execution of those addresses after the injection (fixed 2026-11-08: the logger
returned nil during boot, which deleted its breakpoints before the run started; fixed again 2026-11-10: the breakpoint
objects were not kept in a global, so the GC removed them and nothing was logged).

**Sound (2026-11-10):** the export also writes `spu_ram.bin` (512 KB sound RAM) + `spu_regs.bin` (raw SPU registers).
`mkloader.py` then makes the loader key off all voices, DMA the sound RAM (copied by `inject.lua` to 0x80070000) into
the SPU, ack the DMA4 flag and write our SPU registers (not key on/off, ENDX, FIFO, status, current volume). Without
this the emulator keeps its title-screen sound RAM and level sounds are silent. The loader's DONE address moves with
its size: `mkloader.py` writes it to `inject/done.txt`, which `inject.lua` reads. Voices that were playing at the
export are not restarted (music resumes on its next notes).

**Recording audio:** add `-wavdump <W>/redux.wav` to the Redux command line, and set `SDL_AUDIO_DRIVER=dummy` so nothing
plays on the speakers. The WAV is 44.1 kHz stereo from Redux power-on; run vsync N after the injection is at about
(injection vsync + N) / 59.94 s.

`bootcut.lua` (no injection): boots with the port's default test presses and logs the streamed cutscene counter
(0x800C4C34) and header length (0x800C43E0) whenever they change; `BC_DIR`, `BC_VSYNCS`, `BC_SHOTEVERY=<n>` (raw
screenshots). Run: `BC_DIR=<W> pcsx-redux.exe -run -iso <cue> -dofile bootcut.lua -interpreter -stdout -lua_stdout
-no-ui`. Used 2026-11-08 for the opening cinematic / house intro lengths. The 2026-11-08 Redux copy (build 678, sha1
b3c95f83a4e4d5ca7cc0c84949852f56fe14f124) was unzipped in that session's scratchpad `redux/`.

## Gotchas learned

- The emulator's game must be past its own boot/init before the injection, or kernel event handles don't match:
  inject at a gameplay frame, not the title.
- Redux breakpoint invokers: returning `false` deletes the breakpoint; return `true` to keep it (it does not pause
  unless you call `pauseEmulator`). Keep breakpoint objects in globals, or the GC removes them. Only one breakpoint
  per address seems to fire.
- A Lua runtime error at load shows a hidden "Error" message box and the process hangs: check with
  `Get-Process pcsx-redux.main | Select MainWindowTitle`. Don't put `\n` through a Python heredoc into Lua strings.
- The first 1-2 frames after the injection are the frame our port had already built (its OT), not emulator work.
- 6% pixel difference between the two renderers is normal (dither/shading), as long as it does not grow.

`demolog.lua` (no injection): boots with no input and logs each movie's start (index), first decoded frame and end
(frames shown, whether the file ran out) by vsync; `DL_FORCE=<4|5>` swaps the first attract demo (always the call with
index 3) for another one. Used 2026-11-13 to check the attract demo lengths. `INJECT_OFX=<px>` (inject.lua) forces the
GTE screen x offset every run vsync, to look past the 4:3 screen edge (the game uses OFX 0 + the GPU draw offset).

**2026-11-23:** states made with the big packet ring (0x80200000+, outside the PS1's 2 MB) freeze Redux after the
injection (the OT links into memory it doesn't have). Load the state with `RUGRATS_BIGRING=0` (moves the ring back to
the level's own ring, start gp+0xbc / size gp+0xc0 as `80021e40` sets it up: Reptar 2010 80193e50..801d8410, Let There
Be Light 801d4684..801ee0ac, hub 801d8ff8..801f7840; a fixed address broke non-Reptar states until 2026-11-23; two or
three "DMA2 list loop" lines on that frame are expected), save a new slot a few
seconds later, and export that slot (again with `RUGRATS_BIGRING=0`). `INJECT_DUMPFROM=<k> INJECT_DUMPN=<n>` dump RAM
at game frames k .. k+n-1 after the injection (default 1, 12; `INJECT_DUMPN=0` = none). Redux's CD drive is idle after
the injection (our CD state is not injected), so the music stream and the CD status byte 0x800C2F84 differ from ours.

**Timing gotcha (2026-11-23):** after a test state load the script clock (t60, `RUGRATS_INPUT` times) jumps to the
state's clock, but the frame counter used by `RUGRATS_SHOTFROM` / `RUGRATS_TEST` shot names keeps counting from boot
(one per present: a load at t60 1300 lands at frame 650 at 30 fps, 1300 at 60, 2600 at 120). Shots meant for "right
after the load" must use frames from there, not the state's t60. Redux run vsync N then matches port frame
(load frame + N * fps / 60).
