# Modding Rugrats: Search for Reptar (native port)

Mods change the running game. This version (mod API 3) supports:

- **RAM patches** and **GameShark codes** (API 1)
- **game file replacement**: any file the game loads from the disc, as a whole new file or as a **BPS patch**
  (API 2)
- **texture packs**: hi-res (or just different) pictures for the game's textures (API 3)

Build-time code mods are planned for a later API version.

A mod never contains the game. Share only what you made yourself. To change a game file, share a **BPS patch**
(it holds only your changes) rather than the edited file (see "Sharing edits of game files" below).

## Installing and turning mods on

- **Launcher**: open the **Mods** tab. *Add .zip...* or *Add folder...* installs a mod. Double-click a mod (or
  press *On / off*) to turn it on. *Up* and *Down* change the load order.
- **In game**: press F1 and go to **Game > Mods...**. RAM and GameShark mods switch right away. Mods with files
  or textures switch at the next start of the game (the list says "on after a restart").
- **By hand**: put the mod folder in `mods\` next to `Rugrats.exe`. List the enabled mods in `rugrats.ini`:

  ```ini
  [mods]
  enabled=infinite_health,my_other_mod
  ```

Mods are applied in that order, so a later mod wins when two of them write the same memory or replace the same
file. The launcher warns about that.

The window title shows how many mods are on. Save states remember which mods were on. Loading a state made with
other mods works, and the "Loaded" message says so.

## Mod folder

```
mods/
  my_mod/          the folder name is the mod's id: letters, digits, _ - . (up to 63)
    mod.ini
    files/...      files the mod uses (any layout, any names)
    readme.txt     (optional, anything you like)
```

A `.zip` for the launcher has `mod.ini` either at its root (the id is then the zip's name) or inside one top
folder (the id is that folder's name). Mods are data only: zips with programs or scripts in them (`.exe`, `.dll`,
`.bat`, `.ps1`, ...) are refused. `modkit.py pack` makes such a zip (see below).

## mod.ini

```ini
; comments start with ; or #
[mod]
name=My Mod
version=1.0
author=Your name
description=One line shown in the launcher and the F1 list.
api=2

[ram]
; <when> <address> <type> <value>
always   800E2CEC u16 255
level:5  800C4CC0 u32 0x1000

[gameshark]
D20E2CEC 0040   ; if health < 64 ...
800E2CEC 00FF   ; ... set it to 255

[files]
; <file on the disc> = <file in this mod folder>
DATA/HINTS/HINT01.TXT = hint01.txt

[patch]
; <file on the disc> = <BPS patch in this mod folder>
DATA/MENU/MENU.TEX = files/menu.bps

[textures]
; a folder of PNG pictures named after the textures they replace
folder = textures
```

- `api` is the mod API version the mod needs: `1` for RAM and GameShark only, `2` when it has `[files]` or
  `[patch]`, `3` when it has `[textures]`. A build with a lower version refuses the mod and says why.
- Unknown sections (for example from a newer API) make this build refuse the mod.
- **Anything that can't be used refuses the whole mod**: a bad line, a missing file, a disc path that doesn't
  exist, a patch made for a different file. A mod is never half-applied. The error shows in the launcher, in F1
  and in `rugrats.log`.

RAM and GameShark codes run **once per vblank (60 times a second)**, like on a cheat cartridge, at any frame rate.
With no mod on, nothing runs and no file is changed: the game is identical to having no mod support at all.

### [ram]

One write per line: `<when> <address> <type> <value>`.

| Field | Values |
|---|---|
| when | `always`, or `level:<n>` (only while level database `n` runs, 1..31) |
| address | hex, with or without `0x`. Main RAM: `80000000`-`801FFFFF` (also `00`/`A0` views) |
| type | `u8`, `u16`, `u32` (`s8`/`s16`/`s32` are the same). The address must be aligned to the size |
| value | decimal, `0x` hex, or negative |

Level numbers are the ones shown in F1 > Enhancements > Level Select (1 = the house, 2 = Grandpa's Teeth, ...).

### [gameshark]

One code per line, written `XXXXXXXX YYYY`. A comment after `;` is allowed.

| Code | Meaning |
|---|---|
| `80aaaaaa vvvv` | write 16 bits |
| `30aaaaaa 00vv` | write 8 bits |
| `10aaaaaa vvvv` / `11aaaaaa vvvv` | add / subtract 16 bits |
| `20aaaaaa 00vv` / `21aaaaaa 00vv` | add / subtract 8 bits |
| `D0`/`D1`/`D2`/`D3` `aaaaaa vvvv` | run the next code only if the 16-bit value is = / != / < / > `vvvv` |
| `E0`/`E1`/`E2`/`E3` `aaaaaa 00vv` | same, 8 bits |
| `5000nnss iiii` + `80aaaaaa vvvv` (or `30`) | write `nn` times, address + `ss` and value + `iiii` each time |

When a condition is false, the conditions straight after it are skipped too, and then one more code. So
`D0... D0... 80...` writes only if both conditions hold. Other code types (`C0`, `C1`, `C2`, `D4`-`D6` and so on)
are not supported yet, and a mod that uses them is refused.

### What RAM patches can and can't do

- They change **data**: health, counters, timers, flags, positions, tables the game reads.
- They **can't change the game's code**. This port runs the game's code as native x86, compiled from the disc, so
  writing to addresses inside the game's code (`80010000`-`800C4FFF` holds code *and* data) changes nothing when
  that spot is code. Code mods will come as build-time mods in a later API version.
- Codes from the PlayStation version of the game (SLUS-00650) work as long as they only touch data.

### [files] and [patch]: replacing game files

One file per line: `<file on the disc> = <file in the mod folder>`. Disc paths are written like `modkit.py list`
prints them (`DATA/DB02/LEVEL02.BIN`; case, `\` or `/`, a leading `/` and `;1` don't matter). Paths in the mod
folder must stay inside it (no `..`, no drive letters).

- `[files]` gives the whole new file.
- `[patch]` gives a **BPS patch**, applied to the file from the player's own disc when the game starts. The patch
  stores the file's checksum, so a patch made for another version of the file is refused instead of breaking the
  game.

How it works: the game finds every file through its own table of disc positions and sizes (in its program, at
`800B2C40`), plus a table of the logo/attract/credits movies (`800BAE4C`). A replacement that fits into the
original file's sectors is read from there; a bigger one is put after the end of the disc. Both tables are
updated to match, so files may grow or shrink freely.

There are two kinds of files (`modkit.py list` shows which is which):

| Kind | Which files | Replacement |
|---|---|---|
| `data` | almost everything: levels, models, textures (`.TIM`, `.TEX`, `.RAW`), sounds (`.VB`/`.VH`), music (`.SEP`), fonts, text | plain bytes, or a BPS patch, up to 64 MB |
| `stream` | movies (`DATA/OPEN/*.STR`), cut-scene streams (`DATA/CINEMAT/CINEPAK/*`, `DATA/CINEMAT/OPEN.BIN`), XA audio (`DATA/SOUND/XA/*`) | **raw CD sectors** (2352 or 2336 bytes each, as `modkit.py extract` writes them), up to 512 MB. No BPS |

A replaced movie in the movie table also gets its frame count from the new file (the highest frame number in its
video sectors). Making new STR movies needs an STR encoder (for example from the PlayStation homebrew tools).

Files the game's table doesn't list are never loaded; `modkit.py list` marks them, and the game's log says so when
a mod replaces one.

Replaced files need a restart: when a mod with files is switched in F1, it takes effect at the next start.

### [textures]: texture packs

`folder = <folder in the mod>` names a folder of `.png` pictures. Each one replaces one texture of the game,
found by its file name. Texture packs work with the **GPU renderer** (F1 Video > Renderer, the default); the CPU
renderer draws the original textures.

**Making a pack:**

1. Turn on **F1 > Game > Dump textures** (or `dump=1` under `[textures]` in `rugrats.ini`).
2. Play through the parts you want to change. Every texture the game draws is saved once to `textures_dump\`
   next to `Rugrats.exe`, already named the way a pack needs it.
3. Copy the pictures you want to change into your mod's texture folder and edit them. Make them bigger for more
   detail: any size up to 4096 x 4096 works. A 64 x 64 texture drawn at 256 x 256 is 4x.
4. Turn dumping off again. It costs a little time whenever something new is drawn.

**Names.** A texture is a block of the PS1's texture memory: usually one 64 x 64 tile (bigger when the game draws
a bigger piece at once). The name is a hash of the block's contents, so it is the same on every PC and every run.

| File name | Used for |
|---|---|
| `<16 hex digits>.png` | that texture with any palette (and every 15-bit texture) |
| `<16 hex digits>-<16 hex digits>.png` | that texture only with that palette (wins over the one above) |

Many tiles are drawn with several palettes (the same shape in different colours). The dump saves one picture per
palette. Keep only the ones you change; a picture without the palette part covers all palettes of that tile.

**Pictures:**

- Colours are taken as they are. Shading and lighting are applied on top, as for the original texture.
- Alpha below 50% is see-through, like the PS1's colour 0. Edges are smoothed (bilinear filtering).
- Semi-transparency (glass, shadows, glow) still follows the original texture's texels.
- An unedited dump scaled up without smoothing looks the same as the original.

Files with other names in the folder are ignored, and the launcher counts them as wrong names. When two enabled
packs have a picture with the same name, the later mod in the list wins.

Limits: the textures in use share an atlas of up to 8 slices of 4096 x 4096 pixels on the graphics card (512 MB).
A picture is loaded the first time it is drawn, which can make that frame a little late. Textures the game draws
into itself (render-to-texture) are never replaced.

## The modder's kit: `recomp/tools/modkit.py`

It works on **your** disc image: the one in the installed game's `rugrats.ini`, or `--disc <your .bin>`.

```bat
python -I recomp\tools\modkit.py list [text]                     :: files the game loads (table id, LBA, size, kind)
python -I recomp\tools\modkit.py extract DATA/MENU/MENU.TEX menu.tex
python -I recomp\tools\modkit.py tim2png STRIP.TIM strip.png     :: TIM picture -> PNG (first palette)
python -I recomp\tools\modkit.py png2tim STRIP.TIM strip.png new.tim   :: edited PNG back into the TIM
python -I recomp\tools\modkit.py diff DATA/MENU/TIMS/STRIP.TIM new.tim strip.bps   :: BPS from the disc file
python -I recomp\tools\modkit.py bps original edited out.bps     :: BPS between any two files
python -I recomp\tools\modkit.py apply patch.bps original out    :: apply a BPS (to check one)
python -I recomp\tools\modkit.py pack my_mod my_mod.zip          :: check a mod folder and zip it
```

TIM pictures: 4- and 8-bit TIMs become palette PNGs. Keep them in palette (indexed) mode with the same number of
colours when you edit them; changing a palette colour changes the TIM's first palette. 16-bit TIMs become RGBA
PNGs. A pixel that is fully transparent in the PNG becomes the PlayStation's see-through colour (0). The picture
must keep its size. An unedited PNG turns back into exactly the same TIM.

### Sharing edits of game files

Use `[patch]` with a BPS made by `modkit.py diff`. The patch only holds the bytes you changed and needs the
player's own disc to work. Don't put files extracted from the disc (or edited copies of them) into a mod you
share. `[files]` is meant for files that are entirely your own work: your own text, pictures you drew, sounds you
recorded.

## Useful addresses

All of them were found while building this port; more are in the comments of `recomp/runtime/hooks.c`.

| Address | What |
|---|---|
| `800E2CEC` (u16) | player health, 0..255 (player object `800E2B48` + `0x1A4`) |
| `800C5678` (u32) | level database running now |
| `800C4CC0` (s32) | flashlight battery (Let There Be Light) |
| `800B2C40` | the game's file table: 714 x {LBA, size} (filled in by mods with files) |
| `800BAE4C` | movie table: 7 x {frame count, LBA}: THQ, Nickelodeon, n-Space, 3 attract demos, credits |

## Checking a mod

```bat
python -I recomp\launcher.py --validate-mod path\to\my_mod
python -I recomp\launcher.py --list-mods
python -I recomp\launcher.py --add-mod my_mod.zip [--replace]
```

`--validate-mod` and the launcher's Mods tab make the same checks as the game, including the disc ones (a disc path
that exists, raw sectors for streams, the patch's source checksum) when the game is installed. `rugrats.log` lists
every mod found, whether it is on, where each replaced file is served from, and why a mod was refused.

Debug environment variables: `RUGRATS_MODS=<id,id>` overrides the enabled list for one run (it isn't saved).
`RUGRATS_MODDIR=<folder>` reads mods from another folder. `RUGRATS_MODLOG=1` logs when the game reads a replaced
file (its first and last sector). `RUGRATS_TEXDUMP=1` turns on texture dumping for one run. `rugrats.log` lists the
pictures each pack loads (`textures: loaded ...`) and each dumped one.

## Example mods

`recomp/mods/` holds three examples. The installer copies them into the game's `mods\` folder, turned off.

- `infinite_health`: one `[ram]` write
- `example_codes`: a GameShark condition and write
- `custom_hint`: `[files]`, replaces the first hint screen's text with our own
