# Rugrats: Search for Reptar — native PC port (static recompilation)

An unofficial native Windows version of the 1998 PlayStation game *Rugrats: Search for Reptar* (USA). It is not an
emulator: a build tool translates the game's code from **your own disc image** into C and compiles it into a
Windows program, with a small runtime standing in for the PS1 hardware.

What it adds:

- **Display:** 16:9 widescreen (the 3D world is really wider, not stretched; HUD and menus can stay 4:3 with
  *Stretch 2D* Off), high frame rates (60–240 fps at correct game speed, smooth character animation in between).
- **Video:** internal resolution up to 4K, a GPU (Direct3D 11) renderer, anti-aliasing (downsampling, FXAA, MSAA),
  anisotropic filtering, sub-pixel 3D (no PS1 wobble), perspective-correct textures, no dithering / full colour, and
  fixes for the original's draw-order glitches.
- **Play:** quick save/load (F5/F7, 4 slots), native DualSense / DualShock 4 / Xbox pad support, a right-stick
  camera, optional gameplay tweaks (Enhancements page), and an options menu in game (F1).
- **Mods:** RAM-patch mods, disc file replacements and texture packs, with a Mods page in the launcher and in F1
  (see [recomp/MODDING.md](recomp/MODDING.md)).

## This repository contains no game data

No game code, graphics, sound, video or disc files are included or ever will be. You need your own copy of the game.
Everything the build produces from your disc (the extracted files, the generated C code and `Rugrats.exe`) stays on
your computer. **Do not share the built `Rugrats.exe` or any files from `build/`:** they contain the game.

## What you need

- Windows 10 or 11 (x64)
- [Python 3](https://www.python.org/) and clang for Windows, e.g. [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
  (on `PATH`, or pass `--cc <path>`). **The release zip brings both** (folders `python/` and `toolchain/`, licences in
  `licenses/`), so with it there is nothing to install: unzip, double-click `Launcher.bat`.
- A disc image of **Rugrats: Search for Reptar (USA)**, SLUS-00650, dumped from your own disc as `.bin` + `.cue`.
  The build checks it: SHA1 of the `.bin` must be `b547ef42310a5b04ccfd62d12aa9c41af1c5d6f5`. Other versions and
  modified images are refused.

## Install and play (launcher)

Double-click **`Launcher.bat`**, or run:

```
python -I recomp/launcher.py
```

The launcher asks for your disc image (`.bin` or `.cue`), checks its SHA1, builds the game from it and puts
`Rugrats.exe` + `rugrats.ini` in an install folder of your choice. After that it is the launcher: **Play** starts
the game, and the **Settings** tab edits every option without starting it. Reinstalling (to update) keeps your
settings and saves. **Desktop shortcut** puts a shortcut to the game on your desktop. **Uninstall** removes the game
and asks whether to keep your settings and saves; your disc image is never touched. Paths with characters outside
your Windows language's character set don't work for the install folder (for the disc image, tick "Copy the disc
image into the install folder").

Without the window: `python -I recomp/launcher.py --install <disc> [--dir DIR] [--cc clang]`, `--shortcut`,
`--uninstall [--purge]`.

## Build by hand

```
python -I recomp/build.py "path/to/Rugrats - Search for Reptar (USA).bin"
```

This writes `build/Rugrats.exe` and `build/rugrats.ini` (the disc path is stored there; the disc is only read,
never changed). Run `build/Rugrats.exe`. Press **F1** for options, **F5/F7** for quick save/load, **F8** to change
the frame rate, **F9** for widescreen, **F11** for fullscreen.

Full details (controls, every option, how the recompiler and runtime work, debugging switches):
[recomp/README.md](recomp/README.md).

## Licence

The code and documentation in this repository are under the [MIT licence](LICENSE). That covers only our own work:
the game itself (code, data, characters, trademarks) belongs to its owners and is not part of this project.

## Credits

- PS1 hardware behaviour was written from the [psx-spx](https://psx-spx.consoledev.net/) documentation (Martin
  "nocash" Korth and contributors).
- [PCSX-Redux](https://github.com/grumpycoders/pcsx-redux) was used as a reference to compare behaviour while
  debugging; none of its code is included.

## Disclaimer

Rugrats, Reptar and related titles and characters are trademarks of their respective owners (Nickelodeon).
*Rugrats: Search for Reptar* was developed by n-Space and published by THQ. This project is not affiliated with, endorsed by or connected to any of
them. It contains only original code; you must own the game to use it.
