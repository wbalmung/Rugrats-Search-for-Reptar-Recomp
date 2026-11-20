"""Installer, launcher and settings editor for the Rugrats: Search for Reptar native port.

Run it with no arguments for the window:      python -I recomp/launcher.py
Install without the window:                   python -I recomp/launcher.py --install <disc.bin|.cue> [--dir DIR] [--cc clang] [--copy-disc]
Desktop shortcut / uninstall:                 python -I recomp/launcher.py --shortcut | --uninstall [--purge]
Check the settings table against the game:    python -I recomp/launcher.py --check
Mods:                                         --list-mods | --add-mod <zip|folder> [--replace] | --validate-mod <folder>
Double-click start:                           Launcher.bat in the repo root

Install = check the SHA1 of YOUR disc image, build Rugrats.exe from it (recomp/build.py) and put it in the
install folder with a rugrats.ini. Nothing from the game ships with this program.
"""
import argparse
import ctypes
import hashlib
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tools"))
from psxdisc import EXPECTED_SHA1  # noqa: E402

APP = "Rugrats: Search for Reptar"
DISC_BYTES = 608424768
TOOLCHAIN_URL = "https://github.com/mstorsjo/llvm-mingw/releases"
NO_WINDOW = 0x08000000  # CREATE_NO_WINDOW


# ---- launcher's own state (where the game was installed) -------------------------------------------------------
def cfg_path():
    p = os.environ.get("RUGRATS_LAUNCHER_CFG")
    if p:
        return p
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.path.join(base, "RugratsRecomp", "launcher.json")


def cfg_load():
    try:
        with open(cfg_path(), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def cfg_save(c):
    os.makedirs(os.path.dirname(cfg_path()), exist_ok=True)
    with open(cfg_path(), "w", encoding="utf-8") as f:
        json.dump(c, f, indent=1)


def default_install_dir():
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.path.join(base, "RugratsRecomp", "game")


# ---- rugrats.ini: same Windows calls the game uses, so comments and unknown keys are kept -----------------------
_k32 = ctypes.windll.kernel32 if os.name == "nt" else None


def ini_int(path, sec, key, default):
    return _k32.GetPrivateProfileIntW(sec, key, int(default), path)


def ini_str(path, sec, key, default=""):
    buf = ctypes.create_unicode_buffer(2048)
    _k32.GetPrivateProfileStringW(sec, key, default, buf, 2048, path)
    return buf.value


def ini_has(path, sec, key):
    return ini_str(path, sec, key, "\x01") != "\x01"


def ini_set(path, sec, key, value):
    if not _k32.WritePrivateProfileStringW(sec, key, str(value), path):
        raise OSError(f"could not write {path}")


INI_TEMPLATE = """[game]
; Your Rugrats: Search for Reptar (USA) disc image. Only read, never written.
disc=

[video]
; Everything here can be changed in the launcher's Settings tab, or in game with F1.
; 1 = 16:9 widescreen, 0 = original 4:3
widescreen=1
; widescreen: 1 = menus/HUD/text stretched to 16:9, 0 = kept at 4:3 (GPU renderer)
stretch2d=1
; window size = multiple of 320x240
scale=3
fullscreen=0
; gameplay frame rate. 60/75/100/150 are perfectly even.
fps=60
vsync=1
; internal resolution 1..9 (4 is about 1080p, 9 is 4K)
internal=2
; virtual PS1 CPU multiplier
overclock=4
; 1 = game time follows real time (needed above 60 fps)
realtime_step=1

[audio]
volume=100

[input]

[discord]
enabled=1
"""

# ---- settings table: mirrors menu_items[] in runtime/host_win32.c (run --check after changing either) -----------
BOOL = [0, 1]
OFF_ON = ["Off", "On"]
VOL = list(range(0, 101, 10))


def _pct(v):
    return f"{v}%"


def _res(v):
    tag = {9: "4K", 6: "1440p", 4: "~1080p"}.get(v)
    return f"{v}x  ({tag})" if tag else f"{v}x  ({320 * v} x {240 * v})"


def _golf(v):
    return "Uncorrected" if v <= 0 else "100% (PS1)" if v == 100 else f"{v}%"


# (tab, label, section, key, default, values, names or formatter, hint)
SETTINGS = [
    ("Display", "Window size", "video", "scale", 3, [1, 2, 3, 4, 5, 6], lambda v: f"{v}x  ({240 * v} lines)",
     "Window size in windowed mode (multiple of the PS1's 320x240)."),
    ("Display", "Fullscreen", "video", "fullscreen", 0, BOOL, OFF_ON, "Borderless fullscreen. In game: F11 or Alt+Enter."),
    ("Display", "Widescreen", "video", "widescreen", 1, BOOL, OFF_ON, "16:9 view of the 3D world. Stretch 2D picks how menus, HUD and text are shown."),
    ("Display", "Stretch 2D", "video", "stretch2d", 1, BOOL, OFF_ON,
     "On: menus, HUD and text fill the 16:9 screen (stretched). Off: they keep their original 4:3 shape, centred (GPU renderer)."),
    ("Display", "Frame rate", "video", "fps", 60, [60, 75, 100, 120, 144, 150, 240], lambda v: f"{v} fps",
     "Gameplay frame rate. 60, 75, 100 and 150 give perfectly even motion."),
    ("Display", "VSync", "video", "vsync", 1, BOOL, OFF_ON,
     "On: no tearing; locks to the display when it runs at the frame rate. Off: lowest lag, G-Sync/FreeSync."),
    ("Video", "Internal resolution", "video", "internal", 2, list(range(1, 10)), _res,
     "Sharper 3D and 2D. 4x is about 1080p, 6x 1440p, 9x 4K. Cheap on the GPU renderer; CPU renderer: above 4x needs a strong CPU."),
    ("Video", "Renderer", "video", "renderer", 1, BOOL, ["CPU", "GPU (D3D11)"],
     "GPU: the graphics card draws the picture (fast at any resolution). CPU: drawn on the processor (old path)."),
    ("Video", "FPS counter", "video", "fps_counter", 0, BOOL, OFF_ON, "Shows the frames per second, top-right. In game: F3."),
    ("Video", "Filter", "video", "smooth", 0, BOOL, ["Sharp", "Smooth"],
     "Sharp keeps the original pixels. Smooth blurs them when scaled up."),
    ("Video", "Anti-aliasing", "video", "aa", 1, [0, 1, 2], ["Off", "Downsample", "Downsample + FXAA"],
     "Downsample: internal resolution above the window is averaged down (smooth edges; try 9x). FXAA also smooths edges (slight blur)."),
    ("Video", "Anisotropic filtering", "video", "aniso", 16, [1, 2, 4, 8, 16], ["Off", "2x", "4x", "8x", "16x"],
     "Far and slanted floors and walls stop shimmering: squeezed textures are averaged. Close-up texels stay sharp. GPU renderer."),
    ("Video", "MSAA", "video", "msaa", 1, [1, 2, 4, 8], ["Off", "2x", "4x", "8x"],
     "Smooth polygon edges by multisampling (GPU renderer). Uses a lot of video memory at high internal resolution (4x at 9x: ~2 GB)."),
    ("Video", "Dithering", "video", "dither", 1, [1, 0, 2], ["Original", "Off", "Off + full colour"],
     "Removes the PS1's checkered dither pattern. Full colour also removes colour banding (GPU renderer)."),
    ("Video", "Sub-pixel 3D", "video", "subpixel", 1, [0, 1, 2], ["Off", "Near only", "On"],
     "Smooth 3D (no wobble). Near only: far scenery stays on whole pixels, so its seams stay shut. Off: original."),
    ("Video", "Frame overlap", "video", "overlap", 0, BOOL, OFF_ON,
     "On: at 2x-4x each frame is shown one frame later so the render threads keep working. More fps, +1 frame lag."),
    ("Video", "Perspective textures", "video", "perspective", 1, BOOL, OFF_ON,
     "On: textures on walls and floors stay straight. Needs Hide objects behind walls. Off: original."),
    ("Video", "Hide objects behind walls", "video", "depthfix", 1, BOOL, OFF_ON,
     "Objects behind nearer walls or shelves stay hidden (the PS1's draw order can show them through). Off: original."),
    ("Video", "Fix doubled-face flicker", "video", "dupfix", 1, BOOL, OFF_ON,
     "Some models draw a face twice; the second copy no longer flickers. Off: original."),
    ("Video", "Fix model sort glitches", "video", "objsort", 1, BOOL, OFF_ON,
     "Solid parts of a model hide its parts behind them. Needs Hide objects behind walls. Off: original."),
    ("Audio/Input", "Volume", "audio", "volume", 100, VOL, _pct, "Master volume."),
    ("Audio/Input", "Music", "audio", "music", 100, VOL, _pct, "Background music."),
    ("Audio/Input", "Sound effects", "audio", "sfx", 100, VOL, _pct, "Effects and ambient sounds."),
    ("Audio/Input", "Dialogue", "audio", "dialogue", 100, VOL, _pct,
     "Spoken lines in play and in-game cutscenes. Movies follow only the master volume."),
    ("Audio/Input", "Right-stick camera", "input", "rstick_camera", 1, [0, 1, 2], ["Off", "On", "On, auto-center"],
     "Right stick (or J/L/I/K) turns the camera around the player. Auto-center: drifts back behind when released."),
    ("Audio/Input", "Camera up/down", "input", "rstick_invert", 0, BOOL, ["Normal", "Inverted"],
     "Normal: stick up looks up. Inverted: stick up looks down."),
    ("Game", "Real-time game step", "video", "realtime_step", 1, BOOL, OFF_ON,
     "Game time follows real time. Needed for correct speed above 60 fps."),
    ("Game", "CPU overclock", "video", "overclock", 4, [1, 2, 3, 4, 6, 8], lambda v: f"{v}x", "Virtual PS1 CPU speed."),
    ("Game", "Golf aim speed", "game", "golf_aim", 100, [0, 25, 50, 75, 100, 150, 200], _golf,
     "How fast left/right turns the aim in the golf holes, same at any frame rate. Uncorrected: faster at higher fps."),
    ("Game", "Skip intro", "game", "skip_intro", 0, BOOL, OFF_ON,
     "Boot straight to the title: skips the legal screen and logo movies."),
    ("Game", "Discord status", "discord", "enabled", 1, BOOL, OFF_ON,
     "Shows the game and the current level in your Discord profile (Discord desktop app must be running)."),
    ("Game", "Save slot", "game", "slot", 1, [1, 2, 3, 4], None, "Slot for quick save / quick load. In game: F6 next slot."),
    ("Game", "Dump textures", "textures", "dump", 0, BOOL, OFF_ON,
     "For texture pack makers: every texture the game draws is saved once as a PNG named for packs, in "
     "textures_dump next to the game (GPU renderer). See MODDING.md."),
    ("Enhancements", "Chatter Frequency", "audio", "chatter", 0, [0, 1, 2, 3], ["Original", "Less often", "Rarely", "Off"],
     "How often the babies say random lines while you play. Cutscenes are not affected."),
    ("Enhancements", "Restored Lines", "audio", "voice_restore", 1, BOOL, OFF_ON,
     "On: lines the game recorded but never plays come back: in random chatter (unheard first, then repeating), the deli clerks in the pie gallery, Phil's trike target landing."),
    ("Enhancements", "Unused Hub Lines", "audio", "voice_hub", 1, BOOL, OFF_ON,
     "On: in the house between levels, Tommy now and then says one of his unused lines (needs Restored Lines On)."),
    ("Enhancements", "Golf Reactions", "audio", "voice_golf", 1, BOOL, OFF_ON,
     "On: mini golf babies react to hole in one, birdie, bogey and over par, and sometimes while the ball rolls. Off: original."),
    ("Enhancements", "Reptar Bar Sound", "audio", "bar_roar", 1, [1, 0, 2], ["Chime + roar", "Chime only", "Off"],
     "The sound when you pick up a Reptar bar: the original chime and roar, the chime alone, or nothing."),
    ("Enhancements", "Note Fade-out", "audio", "note_fade", 1, BOOL, OFF_ON,
     "On: music notes the game cuts off instantly fade out over a few milliseconds instead (removes the clicks)."),
    ("Enhancements", "Extended View Distance", "video", "view_ext", 2, [0, 1, 2, 3], ["Off", "Short", "Medium", "Full"],
     "Hedge maze and Reptar 2010: how far rooms seen over walls are drawn. Lower = fewer polygons, faster; far parts can then be missing. Off: original."),
    ("Enhancements", "Override Camera", "input", "rcam_override", 0, BOOL, OFF_ON,
     "On: the right-stick camera also works in levels where it is normally off (Touchdown Tommy)."),
    ("Enhancements", "Wall Bonk", "game", "wall_bonk", 1, BOOL, OFF_ON,
     "On: walking into a wall bonks (sound, short loss of control). Off: you just stop at the wall."),
    ("Enhancements", "Walk After Shooting", "game", "shot_walk", 1, BOOL, ["Original", "Fixed"],
     "Fixed: after firing while walking, the walk animation goes back to normal speed. Original: it stays double speed."),
    ("Enhancements", "\"Let There Be Light\" Fixes", "game", "light_fixes", 1, BOOL, ["Original", "Fixed"],
     "Fixed: the power is out in Let There Be Light, so the kitchen ceiling fan stands still and various objects that kept "
     "their daytime colours get night colours. Original: as on the disc."),
    ("Enhancements", "Final Level Behavior", "game", "final_level", 1, BOOL, ["Default", "Enhanced"],
     "Enhanced: after the last puzzle piece you stay in the level; the TV in the house starts Reptar 2010. Default: straight on."),
]
TABS = ["Display", "Video", "Audio/Input", "Game", "Enhancements"]
# sub-heading drawn above a row, like the in-game menu's `section` field: label -> heading
SECTIONS = {"Frame overlap": "Fixes", "Volume": "Audio", "Right-stick camera": "Input"}

# ---- control bindings: mirrors bind_def[] / pb_* in runtime/host_win32.c ([bindings] in rugrats.ini) ------------
# (label, ini name, default Windows VK code, default controller button)
BINDINGS = [("Up", "up", 0x26, "up"), ("Down", "down", 0x28, "down"), ("Left", "left", 0x25, "left"),
            ("Right", "right", 0x27, "right"), ("Cross", "cross", ord("Z"), "cross"),
            ("Circle", "circle", ord("X"), "circle"), ("Square", "square", ord("A"), "square"),
            ("Triangle", "triangle", ord("S"), "triangle"), ("L1", "l1", ord("Q"), "l1"), ("R1", "r1", ord("E"), "r1"),
            ("L2", "l2", ord("1"), "l2"), ("R2", "r2", ord("3"), "r2"), ("Start", "start", 0x0D, "start"),
            ("Select", "select", 0x08, "select")]
PAD_BUTTONS = [("cross", "A / Cross"), ("circle", "B / Circle"), ("square", "X / Square"), ("triangle", "Y / Triangle"),
               ("l1", "LB / L1"), ("r1", "RB / R1"), ("l2", "LT / L2"), ("r2", "RT / R2"), ("select", "Back / Share"),
               ("start", "Start / Options"), ("up", "D-pad Up"), ("down", "D-pad Down"), ("left", "D-pad Left"),
               ("right", "D-pad Right")]
_KEY_NAMES = {0x26: "Up arrow", 0x28: "Down arrow", 0x25: "Left arrow", 0x27: "Right arrow", 0x0D: "Enter",
              0x08: "Backspace", 0x20: "Space", 0x10: "Shift", 0x11: "Ctrl", 0x12: "Alt", 0x2D: "Insert",
              0x2E: "Delete", 0x24: "Home", 0x23: "End", 0x21: "Page Up", 0x22: "Page Down", 0x14: "Caps Lock",
              0x13: "Pause"}


def key_reserved(vk):
    """Same keys the game refuses: Esc, Tab, F1-F12, J/L/I/K (camera)."""
    return vk in (0x1B, 0x09) or 0x70 <= vk <= 0x7B or vk in (ord("J"), ord("L"), ord("I"), ord("K")) or not 0 < vk < 255


def key_name(vk):
    if vk in _KEY_NAMES:
        return _KEY_NAMES[vk]
    if ord("A") <= vk <= ord("Z") or ord("0") <= vk <= ord("9"):
        return chr(vk)
    if 0x60 <= vk <= 0x69:
        return f"Num {vk - 0x60}"
    try:
        u = ctypes.windll.user32
        sc = u.MapVirtualKeyW(vk, 0)
        buf = ctypes.create_unicode_buffer(64)
        if sc and u.GetKeyNameTextW(sc << 16, buf, 64):
            return buf.value
    except (AttributeError, OSError):
        pass
    return f"Key {vk}"


def binding_swap(lst, i, v):
    """Give row i the value v; a row that had v gets row i's old value."""
    for o in range(len(lst)):
        if o != i and lst[o] == v:
            lst[o] = lst[i]
    lst[i] = v


def bindings_load(ini):
    """(keys, pads) per BINDINGS row, with the game's rules (bad values ignored, duplicates swapped away)."""
    keys = [b[2] for b in BINDINGS]
    pads = [b[3] for b in BINDINGS]
    for i, b in enumerate(BINDINGS):
        vk = ini_int(ini, "bindings", "key_" + b[1], b[2]) if ini else b[2]
        if not key_reserved(vk):
            binding_swap(keys, i, vk)
        pb = (ini_str(ini, "bindings", "pad_" + b[1]) if ini else "").lower()
        if pb in [p[0] for p in PAD_BUTTONS]:
            binding_swap(pads, i, pb)
    return keys, pads


# Free-text ini values that the in-game menu has no row for: (tab, label, section, key, allowed regex, hint).
# Empty: the Discord application ID is built into the game (discord.c); [discord] client_id stays an ini-only override.
TEXT_SETTINGS = []


def value_text(row, v):
    names = row[6]
    if callable(names):
        return names(v)
    if names and v in row[5]:
        return names[row[5].index(v)]
    return str(v)


def check_table():
    """Compare SETTINGS with the ini keys of menu_items[] in host_win32.c."""
    src = open(os.path.join(HERE, "runtime", "host_win32.c"), encoding="utf-8").read()
    body = src[src.index("static MenuItem menu_items[]"):src.index("#define MENU_N")]
    game = set(re.findall(r'"(video|audio|input|game|discord|textures)",\s*"(\w+)"', body))
    mine = {(r[2], r[3]) for r in SETTINGS}
    for s, k in sorted(game - mine):
        print(f"missing in launcher: [{s}] {k}")
    for s, k in sorted(mine - game):
        print(f"not in the game's menu: [{s}] {k}")
    print("settings table matches" if game == mine else "settings table DIFFERS")
    return game == mine


# ---- install ----------------------------------------------------------------------------------------------------
class InstallError(Exception):
    pass


def resolve_disc(path):
    """A .cue points at its .bin; anything else is taken as the image itself."""
    path = os.path.abspath(path)
    if path.lower().endswith(".cue"):
        try:
            text = open(path, encoding="latin-1").read()
        except OSError as e:
            raise InstallError(f"Can't read {path}: {e}")
        m = re.search(r'FILE\s+"([^"]+)"', text, re.I)
        if not m:
            raise InstallError("That .cue file names no image file. Pick the .bin instead.")
        path = os.path.join(os.path.dirname(path), m.group(1))
    if not os.path.isfile(path):
        raise InstallError(f"File not found: {path}")
    return path


def verify_disc(path, progress=None):
    """Raises InstallError unless the file is the supported disc image."""
    size = os.path.getsize(path)
    h = hashlib.sha1()
    done = 0
    try:
        with open(path, "rb") as f:
            while True:
                b = f.read(1 << 22)
                if not b:
                    break
                h.update(b)
                done += len(b)
                if progress:
                    progress(done / max(size, 1))
    except OSError as e:
        raise InstallError(f"Can't read the disc image: {e}")
    got = h.hexdigest()
    if got != EXPECTED_SHA1:
        hint = ""
        if size != DISC_BYTES:
            hint = f"\nIts size is {size:,} bytes; the supported image is {DISC_BYTES:,} bytes."
        raise InstallError(
            "This is not the supported disc image.\n\n"
            "Needed: Rugrats: Search for Reptar (USA), SLUS-00650, as a single-track .bin (2352-byte sectors).\n"
            f"SHA1 wanted: {EXPECTED_SHA1}\nSHA1 of your file: {got}{hint}\n\n"
            "Other regions, other revisions, and compressed or converted images (CHD, PBP, ISO, ECM) won't work.")
    return got


def find_cc(hint=""):
    cands = [hint] if hint else []  # then the release bundle's own compiler, then PATH
    cands += [os.path.join(os.path.dirname(HERE), "toolchain", "bin", "clang.exe"), "clang",
              os.path.expanduser("~/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin/clang.exe")]
    for c in cands:
        p = shutil.which(c) or (c if os.path.isfile(c) else None)
        if p:
            return os.path.abspath(p)
    return None


def console_python():
    """python.exe even when the launcher itself runs under pythonw.exe."""
    exe = sys.executable
    if os.path.basename(exe).lower() == "pythonw.exe":
        alt = os.path.join(os.path.dirname(exe), "python.exe")
        if os.path.exists(alt):
            return alt
    return exe


def ansi_path(p):
    """The game reads rugrats.ini with the ANSI Windows calls. Returns a spelling of p that survives that
    (p itself, or its short 8.3 name), or "" when there is none."""
    for cand in (p, short_path(p)):
        try:
            cand.encode("mbcs")
            return cand
        except UnicodeEncodeError:
            pass
    return ""


def short_path(p):
    buf = ctypes.create_unicode_buffer(1024)
    n = _k32.GetShortPathNameW(p, buf, 1024) if _k32 else 0
    return buf.value if 0 < n < 1024 else p


def disc_for_ini(disc):
    p = ansi_path(disc)
    if not p:
        raise InstallError(
            "The disc image's path has characters the game can't read:\n" + disc +
            "\n\nTick 'Copy the disc image into the install folder', or move / rename the image so its path "
            "uses plain letters.")
    return p


def install(disc, dest, cc="", copy_disc=False, log=print, progress=None):
    """Verify the disc, build the game from it, put Rugrats.exe + rugrats.ini in dest. Returns the exe path."""
    disc = resolve_disc(disc)
    dest = os.path.abspath(dest)
    try:
        dest.encode("mbcs")
    except UnicodeEncodeError:
        raise InstallError("The install folder's path has characters the game can't read:\n" + dest +
                           "\n\nPick a folder whose path uses plain letters.")
    if not copy_disc:
        disc_for_ini(disc)  # fail before the long steps, not after
    log(f"Disc image: {disc}")
    log("Checking the disc image (SHA1)...")
    verify_disc(disc, progress)
    log("Disc image is the right version.")
    ccp = find_cc(cc)
    if not ccp:
        raise InstallError("No C compiler (clang) found.\n\nGet llvm-mingw (x86_64, ucrt) from\n" + TOOLCHAIN_URL +
                           "\nunpack it, and point 'Compiler' at its bin\\clang.exe.")
    log(f"Compiler: {ccp}")
    os.makedirs(dest, exist_ok=True)
    tmp = os.path.join(dest, "_build")
    log("Building the game from your disc (takes a moment)...")
    cmd = [console_python(), "-I", os.path.join(HERE, "build.py"), disc, "--out", tmp, "--cc", ccp]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                         creationflags=NO_WINDOW if os.name == "nt" else 0)
    for line in p.stdout:
        if line.strip() and not line.startswith("> "):  # skip the echoed command lines (very long)
            log("  " + line.rstrip())
    if p.wait():
        raise InstallError("The build failed. See the log above.")
    exe = os.path.join(dest, "Rugrats.exe")
    try:
        shutil.copyfile(os.path.join(tmp, "Rugrats.exe"), exe)
    except OSError as e:
        raise InstallError(f"Can't write {exe} (is the game still running?): {e}")
    if copy_disc:
        target = os.path.join(dest, "disc", "rugrats.bin")
        if os.path.normcase(target) != os.path.normcase(disc):
            log("Copying the disc image into the install folder...")
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(disc, target)
            disc = target
    ini = os.path.join(dest, "rugrats.ini")
    if not os.path.exists(ini):
        with open(ini, "w") as f:
            f.write(INI_TEMPLATE)
    ini_set(ini, "game", "disc", disc_for_ini(disc))
    copy_example_mods(dest, log)
    shutil.rmtree(tmp, ignore_errors=True)
    c = cfg_load()
    c["install_dir"] = dest
    if cc:
        c["cc"] = cc
    cfg_save(c)
    log(f"Installed: {exe}")
    return exe


def installed_dir():
    d = cfg_load().get("install_dir", "")
    return d if d and os.path.isfile(os.path.join(d, "Rugrats.exe")) else ""


# ---- desktop shortcut + uninstall -------------------------------------------------------------------------------
LNK_NAME = "Rugrats - Search for Reptar.lnk"


def shortcut_path():
    d = os.environ.get("RUGRATS_LAUNCHER_DESKTOP")  # testing: write the shortcut somewhere else
    if not d:
        buf = ctypes.create_unicode_buffer(520)
        ctypes.windll.shell32.SHGetFolderPathW(None, 0x10, None, 0, buf)  # CSIDL_DESKTOPDIRECTORY
        d = buf.value or os.path.join(os.path.expanduser("~"), "Desktop")
    return os.path.normpath(os.path.join(d, LNK_NAME))


def make_shortcut(d):
    """Desktop shortcut that starts the installed game. Returns the .lnk path."""
    lnk = shortcut_path()
    ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:RR_LNK);$s.TargetPath=$env:RR_EXE;"
          "$s.WorkingDirectory=$env:RR_DIR;$s.Description='Rugrats: Search for Reptar (native PC port)';$s.Save()")
    env = dict(os.environ, RR_LNK=lnk, RR_EXE=os.path.join(d, "Rugrats.exe"), RR_DIR=d)
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", ps], env=env,
                           capture_output=True, text=True, errors="replace",
                           creationflags=NO_WINDOW if os.name == "nt" else 0)
    except OSError as e:
        raise InstallError(f"Can't make the shortcut: {e}")
    if r.returncode or not os.path.isfile(lnk):
        raise InstallError("Can't make the shortcut.\n" + (r.stderr or r.stdout).strip()[:400])
    return lnk


def uninstall(d, purge=False, log=print):
    """Remove what install() and the game put in d. purge=False keeps rugrats.ini and saves/ (a reinstall into the
    same folder picks them up). Only known names are deleted; anything else in the folder is left alone."""
    d = os.path.abspath(d)
    files = ["Rugrats.exe", "rugrats.log", os.path.join("disc", "rugrats.bin")] + (["rugrats.ini"] if purge else [])
    trees = ["_build"] + (["saves"] if purge else [])
    for name in files:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            try:
                os.remove(p)
            except OSError as e:
                raise InstallError(f"Can't delete {p} (is the game still running?): {e}")
            log(f"Deleted {p}")
    for name in trees:
        p = os.path.join(d, name)
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
            log(f"Deleted {p}")
    for p in (os.path.join(d, "disc"), d):  # only goes when empty
        try:
            os.rmdir(p)
        except OSError:
            pass
    lnk = shortcut_path()
    if os.path.isfile(lnk):
        try:
            os.remove(lnk)
            log(f"Deleted {lnk}")
        except OSError:
            pass
    if purge:
        c = cfg_load()
        if os.path.normcase(c.get("install_dir", "")) == os.path.normcase(d):
            c.pop("install_dir")
            cfg_save(c)
    log("Uninstalled." if purge or not os.path.isdir(d) else f"Uninstalled. Settings and saves are kept in {d}")


# ---- mods (spec: recomp/MODDING.md; the game's loader is runtime/mods.c and accepts exactly the same files) -------
MOD_API = 3
TEX_NAME = re.compile(r"[0-9a-fA-F]{16}(-[0-9a-fA-F]{16})?\.png", re.I)
MOD_INI_MAX = 256 * 1024
MOD_FILES_MAX = 100000   # texture packs can hold many pictures
MOD_BYTES_MAX = 4 * 1024 * 1024 * 1024
MOD_ID = re.compile(r"[A-Za-z0-9_.\-]{1,63}")
GS_TYPES = {0x80: (2, "w"), 0x30: (1, "w"), 0x10: (2, "a"), 0x11: (2, "a"), 0x20: (1, "a"), 0x21: (1, "a"),
            0xD0: (2, "c"), 0xD1: (2, "c"), 0xD2: (2, "c"), 0xD3: (2, "c"),
            0xE0: (1, "c"), 0xE1: (1, "c"), 0xE2: (1, "c"), 0xE3: (1, "c")}
# Phase 1 mods are text only. Code arrives with later phases and will be asked about explicitly.
MOD_REFUSE_EXT = {".exe", ".dll", ".bat", ".cmd", ".ps1", ".vbs", ".js", ".jse", ".wsf", ".scr", ".com", ".msi",
                  ".lnk", ".reg", ".hta", ".cpl", ".sys"}


class ModError(Exception):
    pass


def _ram_off(a, align):
    """Guest address -> offset in the 2 MB of main RAM, or None (same rule as ram_off in mods.c)."""
    if (a >> 24) not in (0x00, 0x80, 0xA0):
        return None
    a &= 0xFFFFFF
    return a if a < 0x200000 and a % align == 0 else None


def _num(s):
    return int(s, 0) if not s.startswith("-") else int(s, 0) & 0xFFFFFFFF


def parse_mod(folder):
    """Read mods/<id>/mod.ini. Returns a dict; 'error' is set when the game would refuse the mod."""
    mid = os.path.basename(os.path.normpath(folder))
    m = {"id": mid, "name": "", "version": "", "author": "", "description": "", "api": 0, "ram": 0, "gs": 0,
         "writes": [], "files": [], "textures": [], "texdir": "", "tex_bad": 0, "error": "", "path": folder}
    try:
        if not MOD_ID.fullmatch(mid):
            raise ModError("folder name may use only letters, digits, _ - .")
        p = os.path.join(folder, "mod.ini")
        if not os.path.isfile(p):
            raise ModError("no mod.ini")
        if os.path.getsize(p) > MOD_INI_MAX:
            raise ModError("mod.ini is bigger than 256 KB")
        with open(p, "rb") as f:
            raw = f.read()
        lines = raw.decode("utf-8-sig", errors="replace").replace("\r", "").split("\n")
        sec = ""
        i = 0
        while i < len(lines):
            ln, s = i + 1, lines[i].strip()
            i += 1
            if not s or s[0] in ";#":
                continue
            if s.startswith("["):
                if "]" not in s:
                    raise ModError(f"line {ln}: bad section")
                sec = s[1:s.index("]")].strip().lower()
                if sec not in ("mod", "ram", "gameshark", "files", "patch", "textures"):
                    raise ModError(f"section [{sec}] needs a newer build")
                continue
            if sec == "mod":
                if "=" in s:
                    k, v = (x.strip() for x in s.split("=", 1))
                    k = k.lower()
                    if k == "api":
                        m["api"] = int(v) if v.isdigit() else 0
                    elif k in ("name", "version", "author", "description"):
                        m[k] = v
                continue
            s = s.split(";", 1)[0].strip()
            if not s:
                continue
            if sec == "ram":
                f = s.split()
                if len(f) != 4:
                    raise ModError(f"line {ln}: want <when> <address> <type> <value>")
                when, a, ty, v = f
                if when.lower() != "always" and not (when.lower().startswith("level:") and when[6:].isdigit()
                                                     and int(when[6:]) <= 63):
                    raise ModError(f"line {ln}: <when> must be always or level:<n>")
                size = {"u8": 1, "s8": 1, "u16": 2, "s16": 2, "u32": 4, "s32": 4}.get(ty.lower())
                if not size:
                    raise ModError(f"line {ln}: type must be u8, u16 or u32")
                try:
                    off = _ram_off(int(a, 16), size)
                except ValueError:
                    off = None
                if off is None:
                    raise ModError(f"line {ln}: address {a} is not in main RAM (or not aligned)")
                try:
                    _num(v)
                except ValueError:
                    raise ModError(f"line {ln}: bad value {v}")
                m["ram"] += 1
                m["writes"].append((off, size))
            elif sec == "gameshark":
                g = re.fullmatch(r"([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{4})", s)
                if not g:
                    raise ModError(f"line {ln}: GameShark codes look like 800E2CEC 00FF")
                a, v = int(g.group(1), 16), int(g.group(2), 16)
                t = a >> 24
                m["gs"] += 1
                if t == 0x50:
                    n, step = (a >> 8) & 0xFF, a & 0xFF
                    g2 = re.fullmatch(r"([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{4})",
                                      lines[i].split(";", 1)[0].strip()) if i < len(lines) else None
                    if not g2:
                        raise ModError(f"line {ln}: serial code needs a second line")
                    b = int(g2.group(1), 16)
                    if b >> 24 not in (0x80, 0x30):
                        raise ModError(f"line {ln}: serial code must be followed by an 80 or 30 code")
                    size = 2 if b >> 24 == 0x80 else 1
                    i += 1
                    for k in range(n):
                        off = _ram_off((b & 0xFFFFFF) + k * step, size)
                        if off is None:
                            raise ModError(f"line {ln + 1}: serial code runs outside main RAM")
                        m["writes"].append((off, size))
                    continue
                if t not in GS_TYPES:
                    raise ModError(f"line {ln}: GameShark code type {t:02X} is not supported")
                size, kind = GS_TYPES[t]
                off = _ram_off(a & 0xFFFFFF, size)
                if off is None:
                    raise ModError(f"line {ln}: address {a & 0xFFFFFF:06X} is not in main RAM (or not aligned)")
                if kind != "c":
                    m["writes"].append((off, size))
            elif sec == "textures":
                k, _, v = (x.strip() for x in s.partition("="))
                parts = re.split(r"[\\/]", v) if v else []
                if k.lower() != "folder" or not v or ":" in v or v[0] in "/\\" or ".." in parts or "" in parts:
                    raise ModError(f"line {ln}: want folder = <folder inside the mod>")
                td = os.path.join(folder, *parts)
                if not os.path.isdir(td):
                    raise ModError(f"line {ln}: texture folder {v} not found")
                m["texdir"] = v
                for f in os.listdir(td):
                    if f.lower().endswith(".png"):
                        if TEX_NAME.fullmatch(f):
                            m["textures"].append(f.lower())
                        else:
                            m["tex_bad"] += 1
            elif sec in ("files", "patch"):
                if "=" not in s:
                    raise ModError(f"line {ln}: want <disc path> = <file in the mod>")
                disc, src = (x.strip() for x in s.split("=", 1))
                disc = disc.replace("\\", "/").strip("/").split(";")[0].upper()
                if not disc or not src:
                    raise ModError(f"line {ln}: want <disc path> = <file in the mod>")
                parts = re.split(r"[\\/]", src)
                if ":" in src or src[0] in "/\\" or ".." in parts or "" in parts:
                    raise ModError(f"line {ln}: {src} must be a plain path inside the mod folder")
                if any(f[0] == disc for f in m["files"]):
                    raise ModError(f"line {ln}: {disc} is replaced twice in this mod")
                if len(m["files"]) >= 256:
                    raise ModError("more than 256 files")
                if not os.path.isfile(os.path.join(folder, *parts)):
                    raise ModError(f"line {ln}: {src}: file not found")
                m["files"].append((disc, sec == "patch", os.path.join(folder, *parts), ln))
            else:
                raise ModError(f"line {ln}: outside a section")
        if m["api"] > MOD_API:
            raise ModError(f"needs mod API {m['api']} (this launcher has {MOD_API})")
    except ModError as e:
        m["error"] = str(e)
    except OSError as e:
        m["error"] = f"can't read: {e}"
    return m


def check_mod_files(m, disc):
    """The checks the game makes when it starts (mods.c make_repl), against the user's disc. disc = psxdisc.Disc."""
    import zlib
    if m["error"] or not m["files"] or disc is None:
        return
    try:
        for name, patch, src, ln in m["files"]:
            f = disc.files.get("/" + name)
            if not f:
                raise ModError(f"line {ln}: no file {name} on the disc")
            lba, size, _ = f
            stream = any(disc.raw(lba + k)[15] == 2 and disc.raw(lba + k)[18] & 0x60
                         for k in range(min(8, (size + 2047) // 2048)))
            n = os.path.getsize(src)
            if stream and patch:
                raise ModError(f"line {ln}: {name} is a streamed file (movie/audio): use [files] with raw sectors")
            if stream and not (n and (n % 2352 == 0 or n % 2336 == 0) and n <= 512 << 20):
                raise ModError(f"line {ln}: {name} is a streamed file: give raw sectors (2352 or 2336 bytes each, as "
                               "modkit extract writes them), up to 512 MB")
            if not stream and n > 64 << 20:
                raise ModError(f"line {ln}: {os.path.basename(src)}: file too big")
            if patch:
                with open(src, "rb") as fp:
                    p = fp.read()
                if p[:4] != b"BPS1" or len(p) < 19:
                    raise ModError(f"line {ln}: {os.path.basename(src)}: not a BPS patch")
                orig = disc.read_extent(lba, size)
                if zlib.crc32(orig) != int.from_bytes(p[-12:-8], "little"):
                    raise ModError(f"line {ln}: {os.path.basename(src)}: the patch is for a different version of "
                                   "this file")
    except ModError as e:
        m["error"] = str(e)
        m["on"] = False
    except OSError as e:
        m["error"] = f"can't read: {e}"
        m["on"] = False


def open_disc(d):
    """The installed game's disc image as psxdisc.Disc, or None."""
    try:
        from psxdisc import Disc
        p = ini_str(os.path.join(d, "rugrats.ini"), "game", "disc") if d else ""
        return Disc(p) if p and os.path.isfile(p) else None
    except (OSError, ValueError):
        return None


def mod_summary(m):
    if m["error"]:
        return "Not loaded: " + m["error"]
    kinds = []
    if m["textures"] or m["texdir"]:
        kinds.append(f"{len(m['textures'])} texture" + ("s" if len(m["textures"]) != 1 else ""))
    if m["files"]:
        kinds.append(f"{len(m['files'])} file" + ("s" if len(m["files"]) != 1 else ""))
    if m["ram"]:
        kinds.append(f"{m['ram']} RAM write" + ("s" if m["ram"] != 1 else ""))
    if m["gs"]:
        kinds.append(f"{m['gs']} GameShark code" + ("s" if m["gs"] != 1 else ""))
    out = ", ".join(kinds) or "empty"
    if m.get("tex_bad"):
        out += f" ({m['tex_bad']} .png with a wrong name ignored)"
    return out


def mods_folder(d):
    return os.path.join(d, "mods")


def mods_enabled(ini):
    return [x for x in re.split(r"[,\s]+", ini_str(ini, "mods", "enabled")) if x] if ini else []


def mods_set_enabled(ini, ids):
    ini_set(ini, "mods", "enabled", ",".join(ids))


def list_mods(d):
    """Installed mods in the game's order: enabled ones in load order, then the rest by name. Each gets 'on'."""
    md = mods_folder(d)
    found = {}
    if os.path.isdir(md):
        for n in os.listdir(md):
            if os.path.isdir(os.path.join(md, n)) and not n.startswith("."):
                found[n.lower()] = parse_mod(os.path.join(md, n))
    out = []
    for mid in mods_enabled(os.path.join(d, "rugrats.ini")):
        m = found.pop(mid.lower(), None)
        if m:
            m["on"] = not m["error"]
            out.append(m)
    for k in sorted(found):
        found[k]["on"] = False
        out.append(found[k])
    if any(m["files"] and not m["error"] for m in out):
        disc = open_disc(d)
        for m in out:
            check_mod_files(m, disc)
    return out


def mod_conflicts(mods):
    """Warnings for enabled mods writing the same RAM bytes or replacing the same disc file (later one wins)."""
    owner, files, out, tex_hit = {}, {}, [], {}
    for m in mods:
        if not m["on"]:
            continue
        for name in m["textures"]:
            if name in files and files[name] != m["id"]:
                tex_hit.setdefault(files[name], 0)
                tex_hit[files[name]] += 1
            files[name] = m["id"]
        for other, n in tex_hit.items():
            out.append(f"'{m['id']}' and '{other}' both replace {n} texture{'s' if n != 1 else ''}; '{m['id']}' wins "
                       "(later in the list).")
        tex_hit.clear()
        for disc, *_ in m["files"]:
            if disc in files and files[disc] != m["id"]:
                out.append(f"'{m['id']}' and '{files[disc]}' both replace {disc}; '{m['id']}' wins (later in the list).")
            files[disc] = m["id"]
        hit = set()
        for off, size in m["writes"]:
            for b in range(off, off + size):
                if b in owner and owner[b] != m["id"]:
                    hit.add(owner[b])
                owner[b] = m["id"]
        for h in sorted(hit):
            out.append(f"'{m['id']}' and '{h}' write the same memory; '{m['id']}' wins (later in the list).")
    return out


def _check_names(names):
    if len(names) > MOD_FILES_MAX:
        raise ModError(f"more than {MOD_FILES_MAX} files")
    for n in names:
        p = n.replace("\\", "/")
        parts = [x for x in p.split("/") if x]
        if p.startswith("/") or re.match(r"^[A-Za-z]:", p) or ".." in parts:
            raise ModError(f"unsafe path inside the mod: {n}")
        if os.path.splitext(p)[1].lower() in MOD_REFUSE_EXT:
            raise ModError(f"contains a program or script ({n}); mods of this version are data only")


def add_mod(src, d, replace=False):
    """Install a mod from a .zip or a folder into <d>/mods/<id>. Returns the parsed mod. The zip may hold mod.ini at
    its root (id = zip name) or inside one top folder (id = that folder)."""
    import zipfile
    md = mods_folder(d)
    os.makedirs(md, exist_ok=True)
    tmp = os.path.join(md, ".adding")
    shutil.rmtree(tmp, ignore_errors=True)
    try:
        if os.path.isdir(src):
            names = [os.path.relpath(os.path.join(r, f), src) for r, _, fs in os.walk(src) for f in fs]
            _check_names(names)
            if sum(os.path.getsize(os.path.join(src, n)) for n in names) > MOD_BYTES_MAX:
                raise ModError("mod is bigger than 4 GB")
            if not os.path.isfile(os.path.join(src, "mod.ini")):
                raise ModError("no mod.ini in that folder")
            mid, root = os.path.basename(os.path.normpath(src)), src
            shutil.copytree(src, tmp)
            root = tmp
        else:
            try:
                z = zipfile.ZipFile(src)
            except (zipfile.BadZipFile, OSError) as e:
                raise ModError(f"not a zip file: {e}")
            with z:
                infos = [i for i in z.infolist() if not i.is_dir()]
                _check_names([i.filename for i in infos])
                if sum(i.file_size for i in infos) > MOD_BYTES_MAX:
                    raise ModError("mod is bigger than 4 GB")
                names = [i.filename.replace("\\", "/") for i in infos]
                if "mod.ini" in names:
                    mid, prefix = os.path.splitext(os.path.basename(src))[0], ""
                else:
                    tops = {n.split("/")[0] for n in names}
                    if len(tops) != 1 or tops.pop() + "/mod.ini" not in names:
                        raise ModError("no mod.ini at the top of the zip (or in one top folder)")
                    mid = names[0].split("/")[0]
                    prefix = mid + "/"
                for i, n in zip(infos, names):
                    out = os.path.join(tmp, *n[len(prefix):].split("/"))
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with z.open(i) as fi, open(out, "wb") as fo:
                        shutil.copyfileobj(fi, fo)
            root = tmp
        if not MOD_ID.fullmatch(mid):
            mid = re.sub(r"[^A-Za-z0-9_.\-]", "_", mid)[:63].strip(".") or "mod"
        target = os.path.join(md, mid)
        if os.path.exists(target):
            if not replace:
                raise ModError(f"a mod named '{mid}' is already installed")
            shutil.rmtree(target)
        os.replace(root, target)
        return parse_mod(target)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def remove_mod(d, mid):
    md = os.path.abspath(mods_folder(d))
    target = os.path.abspath(os.path.join(md, mid))
    if not MOD_ID.fullmatch(mid) or os.path.dirname(target) != md or not os.path.isdir(target):
        raise ModError(f"no installed mod '{mid}'")
    shutil.rmtree(target)
    ini = os.path.join(d, "rugrats.ini")
    on = mods_enabled(ini)
    if any(x.lower() == mid.lower() for x in on):
        mods_set_enabled(ini, [x for x in on if x.lower() != mid.lower()])


def copy_example_mods(dest, log=print):
    """recomp/mods/* (our own example mods) into the install's mods folder, off, without touching existing ones."""
    src = os.path.join(HERE, "mods")
    if not os.path.isdir(src):
        return
    for n in sorted(os.listdir(src)):
        t = os.path.join(mods_folder(dest), n)
        if os.path.isfile(os.path.join(src, n, "mod.ini")) and not os.path.exists(t):
            shutil.copytree(os.path.join(src, n), t)
            log(f"Example mod added (off): {n}")


# ---- window -----------------------------------------------------------------------------------------------------
# Dark theme with a purple accent. Colours only; no artwork from the game.
C = {"bg": "#15111d", "field": "#231c31", "hover": "#30264a", "border": "#3d3158", "text": "#ece8f5",
     "muted": "#a59bbd", "accent": "#8b5cf6", "accent_hi": "#a57ff9", "accent_lo": "#7140e0", "off": "#4b4263",
     "soft": "#c4b0ff", "head_a": (0x3b, 0x1a, 0x7e), "head_b": (0xa8, 0x55, 0xf7)}
FONT = "Segoe UI"


def apply_theme(root, ttk):
    st = ttk.Style(root)
    st.theme_use("clam")
    bg, field, border, text, muted, accent = C["bg"], C["field"], C["border"], C["text"], C["muted"], C["accent"]
    root.configure(bg=bg)
    st.configure(".", background=bg, foreground=text, font=(FONT, 10), bordercolor=border, lightcolor=bg, darkcolor=bg,
                 troughcolor=field, focuscolor=bg, fieldbackground=field, insertcolor=text,
                 selectbackground=accent, selectforeground="#ffffff")
    st.configure("Muted.TLabel", foreground=muted)
    st.configure("Ok.TLabel", foreground=C["soft"])
    st.configure("Section.TLabel", foreground=C["accent_hi"], font=(FONT, 10, "bold"))
    st.configure("TButton", background=field, bordercolor=border, lightcolor=field, darkcolor=field, padding=(12, 6),
                 relief="flat")
    plain = [("disabled", bg), ("pressed", border), ("active", C["hover"])]
    st.map("TButton", background=plain, lightcolor=plain, darkcolor=plain,
           bordercolor=[("disabled", border), ("active", accent)], foreground=[("disabled", C["off"])])
    acc = [("disabled", C["off"]), ("pressed", C["accent_lo"]), ("active", C["accent_hi"])]
    st.configure("Accent.TButton", background=accent, foreground="#ffffff", bordercolor=accent, lightcolor=accent,
                 darkcolor=accent, font=(FONT, 13, "bold"), padding=(36, 10))
    st.map("Accent.TButton", background=acc, lightcolor=acc, darkcolor=acc, bordercolor=acc,
           foreground=[("disabled", muted)])
    go = [("disabled", bg), ("pressed", C["accent_lo"]), ("active", accent)]
    tint = "#33235c"
    st.configure("Go.TButton", background=tint, foreground="#e4d9ff", bordercolor=tint, lightcolor=tint, darkcolor=tint,
                 font=(FONT, 10, "bold"), padding=(20, 7))
    st.map("Go.TButton", background=go, lightcolor=go, darkcolor=go,
           foreground=[("disabled", C["off"]), ("active", "#ffffff")], bordercolor=[("disabled", C["off"])])
    st.configure("TEntry", bordercolor=border, lightcolor=field, darkcolor=field, padding=5)
    st.map("TEntry", bordercolor=[("focus", accent)])
    st.configure("TCombobox", background=field, arrowcolor=muted, bordercolor=border, lightcolor=field,
                 darkcolor=field, padding=4, arrowsize=14)
    st.map("TCombobox", fieldbackground=[("readonly", field)], foreground=[("readonly", text)],
           selectbackground=[("readonly", field)], selectforeground=[("readonly", text)],
           background=[("active", C["hover"])], bordercolor=[("focus", accent), ("active", accent)],
           arrowcolor=[("active", "#ffffff")])
    for k, v in (("background", field), ("foreground", text), ("selectBackground", accent),
                 ("selectForeground", "#ffffff"), ("font", (FONT, 10))):
        root.option_add("*TCombobox*Listbox." + k, v)
    st.configure("TCheckbutton", indicatorbackground=field, indicatorforeground="#ffffff", upperbordercolor=border,
                 lowerbordercolor=border, indicatormargin=(2, 2, 8, 2))
    st.map("TCheckbutton", background=[("active", bg)], indicatorbackground=[("selected", accent)],
           upperbordercolor=[("selected", accent)], lowerbordercolor=[("selected", accent)])
    st.configure("TLabelframe", bordercolor=border, lightcolor=bg, darkcolor=bg, borderwidth=1, relief="solid")
    st.configure("TLabelframe.Label", foreground=C["soft"], font=(FONT, 10, "bold"))
    st.configure("TNotebook", borderwidth=0, tabmargins=(0, 0, 0, 0), bordercolor=bg)
    st.configure("TNotebook.Tab", background=bg, foreground=muted, padding=(18, 8), borderwidth=0, bordercolor=bg,
                 lightcolor=bg, darkcolor=bg, font=(FONT, 10, "bold"))
    st.map("TNotebook.Tab", background=[("selected", field)], foreground=[("selected", "#ffffff"), ("active", text)],
           lightcolor=[("selected", accent)], bordercolor=[("selected", field)], darkcolor=[("selected", field)],
           padding=[("selected", (18, 8))])
    st.configure("Treeview", background=field, fieldbackground=field, foreground=text, bordercolor=border,
                 lightcolor=field, darkcolor=field, rowheight=28, font=(FONT, 10))
    st.map("Treeview", background=[("selected", accent)], foreground=[("selected", "#ffffff")])
    st.configure("Treeview.Heading", background=bg, foreground=muted, bordercolor=border, lightcolor=bg, darkcolor=bg,
                 relief="flat", font=(FONT, 9, "bold"), padding=(6, 4))
    st.map("Treeview.Heading", background=[("active", field)])
    st.configure("Vertical.TScrollbar", background=field, troughcolor=bg, bordercolor=bg, lightcolor=field,
                 darkcolor=field, arrowcolor=muted, gripcount=0)
    st.map("Vertical.TScrollbar", background=[("active", C["hover"])], arrowcolor=[("active", "#ffffff")])
    st.configure("Horizontal.TProgressbar", troughcolor=field, background=accent, bordercolor=field, lightcolor=accent,
                 darkcolor=accent, thickness=8)


def dark_titlebar(root):
    try:
        hwnd = ctypes.windll.user32.GetParent(root.winfo_id())
        on = ctypes.c_int(1)
        ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(on), 4)  # DWMWA_USE_IMMERSIVE_DARK_MODE
    except (AttributeError, OSError):
        pass


def make_header(root, tk):
    """Purple gradient band with the title; redrawn when the window width changes."""
    cv = tk.Canvas(root, height=92, highlightthickness=0, bd=0, bg=C["bg"])

    def draw(e=None):
        w = max(cv.winfo_width(), 1)
        cv.delete("all")
        a, b = C["head_a"], C["head_b"]
        step = 4
        for x in range(0, w, step):
            t = (x / w) ** 2
            col = "#%02x%02x%02x" % tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
            cv.create_rectangle(x, 0, x + step, 92, fill=col, outline=col)
        for r in (150, 105, 60):  # thin rings on the right
            cv.create_oval(w - 70 - r, 46 - r, w - 70 + r, 46 + r, outline="#c9a6ff", width=1)
        cv.create_text(26, 36, anchor="w", text=APP, fill="#ffffff", font=(FONT, 21, "bold"))
        cv.create_text(28, 66, anchor="w", text="Native PC port, built from your own disc", fill="#e3d5ff",
                       font=(FONT, 10))
    cv.bind("<Configure>", draw)
    return cv


def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    root.withdraw()
    root.title(APP + " - Launcher")
    root.minsize(720, 700)
    apply_theme(root, ttk)
    pad = {"padx": 12, "pady": 6}
    make_header(root, tk).pack(fill="x")
    nb = ttk.Notebook(root)
    nb.pack(fill="both", expand=True, padx=14, pady=(10, 12))
    play_tab, set_tab, mod_tab = ttk.Frame(nb), ttk.Frame(nb), ttk.Frame(nb)
    nb.add(play_tab, text="  Play  ")
    nb.add(set_tab, text="  Settings  ")
    nb.add(mod_tab, text="  Mods  ")
    q = queue.Queue()
    busy = {"on": False}

    def ini_file():
        d = installed_dir()
        return os.path.join(d, "rugrats.ini") if d else ""

    # -- Play tab
    status = ttk.Label(play_tab, justify="center", wraplength=620, style="Muted.TLabel")
    status.pack(pady=(20, 12))
    play_btn = ttk.Button(play_tab, text="Play", width=16, style="Accent.TButton")
    play_btn.pack()
    row = ttk.Frame(play_tab)
    row.pack(pady=(12, 8))
    folder_btn = ttk.Button(row, text="Open game folder", command=lambda: os.startfile(installed_dir()))
    folder_btn.pack(side="left", padx=4)
    ttk.Button(row, text="Settings", command=lambda: nb.select(set_tab)).pack(side="left", padx=4)
    lnk_btn = ttk.Button(row, text="Desktop shortcut")
    lnk_btn.pack(side="left", padx=4)
    unin_btn = ttk.Button(row, text="Uninstall")
    unin_btn.pack(side="left", padx=4)
    ttk.Label(play_tab, text="In game:  F1 options   |   F5 / F7 quick save / load   |   F11 fullscreen   |   Esc quit",
              style="Muted.TLabel").pack(side="bottom", pady=(8, 4))

    # -- install options (on the Play page)
    inst_tab = ttk.LabelFrame(play_tab, text="  Install / update  ", padding=(6, 4))
    inst_tab.pack(fill="both", expand=True, padx=4, pady=(6, 0))
    disc_var = tk.StringVar()
    dest_var = tk.StringVar(value=cfg_load().get("install_dir") or default_install_dir())
    cc_var = tk.StringVar(value=cfg_load().get("cc") or find_cc() or "")
    copy_var = tk.IntVar(value=0)
    ttk.Label(inst_tab, wraplength=640, justify="left", style="Muted.TLabel", text=(
        "The game itself is not included. Pick your own disc image of Rugrats: Search for Reptar (USA, SLUS-00650). "
        "It is checked (SHA1), then the PC version is built from it. The disc image is only read, never changed.")
    ).grid(row=0, column=0, columnspan=3, sticky="w", **pad)

    def pick_disc():
        p = filedialog.askopenfilename(title="Your Rugrats: Search for Reptar (USA) disc image",
                                       filetypes=[("PS1 disc image", "*.bin *.cue"), ("All files", "*.*")])
        if p:
            disc_var.set(os.path.normpath(p))

    def pick_dest():
        p = filedialog.askdirectory(title="Install folder")
        if p:
            dest_var.set(os.path.normpath(p))

    def pick_cc():
        p = filedialog.askopenfilename(title="clang.exe", filetypes=[("clang", "clang*.exe"), ("Programs", "*.exe")])
        if p:
            cc_var.set(os.path.normpath(p))

    for r, (label, var, cmd) in enumerate([("Disc image (.bin / .cue)", disc_var, pick_disc),
                                           ("Install folder", dest_var, pick_dest),
                                           ("Compiler (clang)", cc_var, pick_cc)], start=1):
        ttk.Label(inst_tab, text=label).grid(row=r, column=0, sticky="w", **pad)
        ttk.Entry(inst_tab, textvariable=var).grid(row=r, column=1, sticky="ew", pady=6)
        ttk.Button(inst_tab, text="Browse...", command=cmd).grid(row=r, column=2, **pad)
    ttk.Checkbutton(inst_tab, variable=copy_var,
                    text="Copy the disc image into the install folder (about 600 MB; otherwise it must stay where it is)"
                    ).grid(row=4, column=0, columnspan=3, sticky="w", **pad)
    inst_btn = ttk.Button(inst_tab, text="Check disc and install", style="Go.TButton")
    inst_btn.grid(row=5, column=0, columnspan=3, pady=8)
    bar = ttk.Progressbar(inst_tab, maximum=1.0)
    bar.grid(row=6, column=0, columnspan=3, sticky="ew", **pad)
    logbox = tk.Text(inst_tab, height=4, state="disabled", wrap="word", font=("Consolas", 9), bg=C["field"],
                     fg=C["muted"], relief="flat", bd=0, highlightthickness=1, highlightbackground=C["border"],
                     highlightcolor=C["border"], padx=8, pady=6, selectbackground=C["accent"])
    logbox.grid(row=7, column=0, columnspan=3, sticky="nsew", **pad)
    inst_tab.columnconfigure(1, weight=1)
    inst_tab.rowconfigure(7, weight=1)

    def log(s):
        logbox.config(state="normal")
        logbox.insert("end", s + "\n")
        logbox.see("end")
        logbox.config(state="disabled")

    def start_install():
        if busy["on"]:
            return
        if not disc_var.get().strip():
            pick_disc()
            if not disc_var.get().strip():
                return
        busy["on"] = True
        inst_btn.config(state="disabled")
        play_btn.config(state="disabled")
        bar.config(mode="determinate", value=0)
        args = (disc_var.get().strip(), dest_var.get().strip() or default_install_dir(), cc_var.get().strip(),
                bool(copy_var.get()))

        def work():
            try:
                install(*args, log=lambda s: q.put(("log", s)), progress=lambda f: q.put(("bar", f)))
                q.put(("done", None))
            except InstallError as e:
                q.put(("fail", str(e)))
            except Exception as e:  # show anything unexpected instead of dying silently
                q.put(("fail", f"{type(e).__name__}: {e}"))
        threading.Thread(target=work, daemon=True).start()

    inst_btn.config(command=start_install)

    # -- Settings tab
    rows = []  # (row, StringVar, combobox)
    hint = ttk.Label(set_tab, wraplength=640, justify="left", style="Muted.TLabel",
                     text="Point at a setting to see what it does.")
    snb = ttk.Notebook(set_tab)
    snb.pack(fill="both", expand=True, padx=4, pady=(12, 8))
    pages = {}
    for t in TABS:
        pages[t] = ttk.Frame(snb)
        snb.add(pages[t], text=f"  {t}  ")
        pages[t].columnconfigure(1, weight=1)
    for row in SETTINGS:
        page = pages[row[0]]
        r = page.grid_size()[1]
        if row[1] in SECTIONS:  # sub-heading row, like the in-game menu
            ttk.Label(page, text=SECTIONS[row[1]], style="Section.TLabel").grid(
                row=r, column=0, columnspan=2, sticky="w", padx=(12, 12), pady=(14 if r == 0 else 12, 0))
            r += 1
        lab = ttk.Label(page, text=row[1])
        lab.grid(row=r, column=0, sticky="w", padx=(18, 12), pady=(14 if r == 0 else 4, 4))
        var = tk.StringVar()
        cb = ttk.Combobox(page, textvariable=var, state="readonly", width=30)
        cb.grid(row=r, column=1, sticky="w", padx=12, pady=(14 if r == 0 else 4, 4))
        for w in (lab, cb):
            w.bind("<Enter>", lambda e, h=row[7]: hint.config(text=h))
        cb.bind("<FocusIn>", lambda e, h=row[7]: hint.config(text=h))
        rows.append((row, var, cb))
    def bindings_dialog():
        ini = ini_file()
        if not ini:
            messagebox.showinfo(APP, "Install the game first.")
            return
        keys, pads = bindings_load(ini)
        dlg = tk.Toplevel(root)
        dlg.title("Control Bindings")
        dlg.configure(bg=C["bg"])
        dlg.transient(root)
        dlg.resizable(False, False)
        ttk.Label(dlg, wraplength=520, justify="left", style="Muted.TLabel", text=(
            "One keyboard key and one controller button per PS1 button. Click a key to change it, then press the new "
            "key (Esc cancels). Taking a key or button that is already used swaps the two. Reserved: Esc, Tab, "
            "F1-F12, J/L/I/K; L3 / R3 / PS and the right stick can't be bound. The game's F1 menu has the same page "
            "(there you can press the controller button too).")).grid(row=0, column=0, columnspan=3, sticky="w",
                                                                       padx=16, pady=(14, 8))
        for c, t in enumerate(("PS1 button", "Keyboard", "Controller")):
            ttk.Label(dlg, text=t, style="Section.TLabel").grid(row=1, column=c, sticky="w", padx=16, pady=(4, 2))
        kbtn, pcb = [], []
        msg = ttk.Label(dlg, style="Ok.TLabel", text="")
        waiting = [None]
        pad_ids = [p[0] for p in PAD_BUTTONS]

        def show():
            for i in range(len(BINDINGS)):
                kbtn[i].config(text="Press a key..." if waiting[0] == i else key_name(keys[i]))
                pcb[i].current(pad_ids.index(pads[i]))

        def on_key(e):
            i = waiting[0]
            if i is None:
                return None
            vk = e.keycode
            if vk == 0x1B:
                waiting[0] = None
            elif key_reserved(vk):
                msg.config(text="That key is reserved (Esc, Tab, F1-F12, J/L/I/K). Try another.")
                return "break"
            else:
                binding_swap(keys, i, vk)
                waiting[0] = None
                msg.config(text="")
            show()
            return "break"

        def wait_key(i):
            waiting[0] = i
            msg.config(text="")
            show()
            dlg.focus_set()

        def pad_pick(i):
            binding_swap(pads, i, pad_ids[pcb[i].current()])
            show()

        for i, b in enumerate(BINDINGS):
            ttk.Label(dlg, text=b[0]).grid(row=2 + i, column=0, sticky="w", padx=16, pady=2)
            kb = ttk.Button(dlg, width=18, command=lambda i=i: wait_key(i))
            kb.grid(row=2 + i, column=1, sticky="w", padx=16, pady=2)
            cb = ttk.Combobox(dlg, state="readonly", width=18, values=[p[1] for p in PAD_BUTTONS])
            cb.grid(row=2 + i, column=2, sticky="w", padx=16, pady=2)
            cb.bind("<<ComboboxSelected>>", lambda e, i=i: pad_pick(i))
            kbtn.append(kb)
            pcb.append(cb)
        dlg.bind("<KeyPress>", on_key)
        n = 2 + len(BINDINGS)
        msg.grid(row=n, column=0, columnspan=3, sticky="w", padx=16, pady=(8, 0))
        bar = ttk.Frame(dlg)
        bar.grid(row=n + 1, column=0, columnspan=3, sticky="ew", padx=12, pady=12)

        def defaults():
            keys[:] = [b[2] for b in BINDINGS]
            pads[:] = [b[3] for b in BINDINGS]
            waiting[0] = None
            show()

        def save():
            try:
                for i, b in enumerate(BINDINGS):
                    ini_set(ini, "bindings", "key_" + b[1], keys[i])
                    ini_set(ini, "bindings", "pad_" + b[1], pads[i])
            except OSError as e:
                messagebox.showerror(APP, str(e), parent=dlg)
                return
            dlg.destroy()
            set_msg.config(text="Control bindings saved.")
            root.after(2500, lambda: set_msg.config(text=""))

        ttk.Button(bar, text="Save", command=save, style="Go.TButton").pack(side="right", padx=4)
        ttk.Button(bar, text="Cancel", command=dlg.destroy).pack(side="right", padx=4)
        ttk.Button(bar, text="Reset to defaults", command=defaults).pack(side="left", padx=4)
        show()
        dlg.grab_set()
        dlg.focus_set()

    bind_page = pages["Audio/Input"]
    bind_btn = ttk.Button(bind_page, text="Control Bindings...", command=bindings_dialog)
    bind_btn.grid(row=bind_page.grid_size()[1], column=0, columnspan=2, sticky="w", padx=(18, 12), pady=(18, 4))
    bind_hint = "Choose the keyboard key and controller button for each PS1 button."
    bind_btn.bind("<Enter>", lambda e: hint.config(text=bind_hint))
    bind_btn.bind("<FocusIn>", lambda e: hint.config(text=bind_hint))
    text_rows = []  # (row, StringVar)
    for row in TEXT_SETTINGS:
        page = pages[row[0]]
        r = page.grid_size()[1]
        lab = ttk.Label(page, text=row[1])
        lab.grid(row=r, column=0, sticky="w", padx=(18, 12), pady=4)
        var = tk.StringVar()
        ent = ttk.Entry(page, textvariable=var, width=33)
        ent.grid(row=r, column=1, sticky="w", padx=12, pady=4)
        for w in (lab, ent):
            w.bind("<Enter>", lambda e, h=row[5]: hint.config(text=h))
        ent.bind("<FocusIn>", lambda e, h=row[5]: hint.config(text=h))
        text_rows.append((row, var))
    hint.pack(fill="x", padx=14)
    sbar = ttk.Frame(set_tab)
    sbar.pack(fill="x", padx=8, pady=8)
    set_msg = ttk.Label(sbar, style="Ok.TLabel")
    set_msg.pack(side="left", padx=6)

    def settings_show(values):
        for (row, var, cb), v in zip(rows, values):
            vals = list(row[5]) if v in row[5] else list(row[5]) + [v]  # keep a hand-edited value selectable
            cb.vals = vals
            cb.config(values=[value_text(row, x) for x in vals])
            cb.current(vals.index(v))

    def settings_load():
        ini = ini_file()
        settings_show([ini_int(ini, r[2], r[3], r[4]) if ini else r[4] for r in SETTINGS])
        for row, var in text_rows:
            var.set(ini_str(ini, row[2], row[3]) if ini else "")
        set_msg.config(text="" if ini else "Install the game first.")

    def settings_defaults():
        settings_show([r[4] for r in SETTINGS])
        for row, var in text_rows:
            var.set("")

    def settings_save():
        ini = ini_file()
        if not ini:
            return
        for row, var in text_rows:
            v = var.get().strip()
            if v and not re.fullmatch(row[4], v):
                messagebox.showerror(APP, f"{row[1]}: '{v}' is not valid. {row[5]}")
                return
        try:
            for row, var in text_rows:
                v = var.get().strip()
                if v != ini_str(ini, row[2], row[3]):
                    ini_set(ini, row[2], row[3], v)
            for row, var, cb in rows:
                v = cb.vals[cb.current()]
                if ini_has(ini, row[2], row[3]) or v != row[4]:
                    if ini_int(ini, row[2], row[3], row[4]) != v or not ini_has(ini, row[2], row[3]):
                        ini_set(ini, row[2], row[3], v)
        except OSError as e:
            messagebox.showerror(APP, str(e))
            return
        set_msg.config(text="Saved.")
        root.after(2500, lambda: set_msg.config(text=""))

    save_btn = ttk.Button(sbar, text="Save", command=settings_save, style="Go.TButton")
    save_btn.pack(side="right", padx=4)
    ttk.Button(sbar, text="Undo changes", command=settings_load).pack(side="right", padx=4)
    ttk.Button(sbar, text="Defaults", command=settings_defaults).pack(side="right", padx=4)

    # -- Mods tab: list in load order (later wins), on/off, add / remove. Saved to rugrats.ini right away.
    ttk.Label(mod_tab, wraplength=660, justify="left", style="Muted.TLabel", text=(
        "Mods change the game: cheats, memory patches, replaced game files and texture packs. Turn one on with a double-click or Space; "
        "mods lower in the list win over the ones above. Changes to mods with files apply at the next game start. "
        "In game: F1 > Game > Mods."
    )).pack(fill="x", padx=12, pady=(14, 6))
    mbox = ttk.Frame(mod_tab)
    mbox.pack(fill="both", expand=True, padx=8)
    tree = ttk.Treeview(mbox, columns=("on", "name", "ver", "author", "what"), show="headings", selectmode="browse",
                        height=8)
    for col, txt, w, st in (("on", "On", 60, False), ("name", "Mod", 210, True), ("ver", "Version", 70, False),
                            ("author", "Author", 150, True), ("what", "Contents", 200, True)):
        tree.heading(col, text=txt, anchor="w")
        tree.column(col, width=w, stretch=st, anchor="w")
    tree.tag_configure("off", foreground=C["muted"])
    tree.tag_configure("bad", foreground="#ff8fa3")
    tree.pack(side="left", fill="both", expand=True)
    msb = ttk.Scrollbar(mbox, orient="vertical", command=tree.yview)
    msb.pack(side="right", fill="y")
    tree.config(yscrollcommand=msb.set)
    mod_info = ttk.Label(mod_tab, wraplength=660, justify="left", text="")
    mod_info.pack(fill="x", padx=12, pady=(8, 2))
    mod_warn = ttk.Label(mod_tab, wraplength=660, justify="left", style="Ok.TLabel", text="")
    mod_warn.pack(fill="x", padx=12, pady=(0, 4))
    mbar = ttk.Frame(mod_tab)
    mbar.pack(fill="x", padx=8, pady=(4, 10))
    mstate = {"list": []}

    def mods_refresh(keep=None):
        d = installed_dir()
        tree.delete(*tree.get_children())
        mstate["list"] = list_mods(d) if d else []
        for m in mstate["list"]:
            tree.insert("", "end", iid=m["id"], tags=("bad" if m["error"] else "on" if m["on"] else "off",),
                        values=("Error" if m["error"] else "On" if m["on"] else "Off", m["name"] or m["id"],
                                m["version"], m["author"], mod_summary(m)))
        if keep and tree.exists(keep):
            tree.selection_set(keep)
            tree.see(keep)
        elif mstate["list"]:
            tree.selection_set(mstate["list"][0]["id"])
        for b in mod_btns:
            b.config(state="normal" if d else "disabled")
        mod_warn.config(text="\n".join(mod_conflicts(mstate["list"])))
        if not d:
            mod_info.config(text="Install the game first.")
        elif not mstate["list"]:
            mod_info.config(text="No mods installed. Add one from a .zip or a folder.")
        mods_select()

    def mods_cur():
        sel = tree.selection()
        return next((m for m in mstate["list"] if m["id"] == sel[0]), None) if sel else None

    def mods_select(e=None):
        m = mods_cur()
        if m:
            head = f"{m['name'] or m['id']} {m['version']}".strip() + (f"  by {m['author']}" if m["author"] else "")
            mod_info.config(text=head + "\n" + (("Not loaded: " + m["error"]) if m["error"] else m["description"]))

    def mods_save(keep):
        ini = ini_file()
        try:
            mods_set_enabled(ini, [m["id"] for m in mstate["list"] if m["on"]])
        except OSError as e:
            messagebox.showerror(APP, str(e))
        mods_refresh(keep)

    def mods_toggle(e=None):
        m = mods_cur()
        if not m:
            return
        if m["error"]:
            messagebox.showerror(APP, f"{m['id']} can't be turned on:\n{m['error']}")
            return
        m["on"] = not m["on"]
        mstate["list"].remove(m)  # on: last of the enabled ones (end of the load order); off: back among the rest
        mstate["list"].insert(sum(1 for x in mstate["list"] if x["on"]), m)
        mods_save(m["id"])

    def mods_move(dirn):
        m = mods_cur()
        if not m or not m["on"]:
            return
        lst = mstate["list"]
        i = lst.index(m)
        j = i + dirn
        if 0 <= j < len(lst) and lst[j]["on"]:
            lst[i], lst[j] = lst[j], lst[i]
            mods_save(m["id"])

    def mods_add(folder):
        d = installed_dir()
        if not d:
            return
        src = filedialog.askdirectory(title="Mod folder (with mod.ini)") if folder else filedialog.askopenfilename(
            title="Mod", filetypes=[("Mod (.zip)", "*.zip"), ("All files", "*.*")])
        if not src:
            return
        src = os.path.normpath(src)
        try:
            try:
                m = add_mod(src, d)
            except ModError as e:
                if "already installed" not in str(e) or not messagebox.askyesno(APP, f"{e}.\n\nReplace it?"):
                    raise
                m = add_mod(src, d, replace=True)
        except (ModError, OSError) as e:
            messagebox.showerror(APP, f"Can't add the mod:\n{e}")
            return
        mods_refresh(m["id"])
        if m["error"]:
            messagebox.showwarning(APP, f"Added '{m['id']}', but the game will not load it:\n{m['error']}")

    def mods_remove():
        m, d = mods_cur(), installed_dir()
        if not m or not d:
            return
        if not messagebox.askyesno(APP, f"Delete the mod '{m['name'] or m['id']}'?\n\n{m['path']}\n\n"
                                        "This deletes its folder and can't be undone.", icon="warning", default="no"):
            return
        try:
            remove_mod(d, m["id"])
        except (ModError, OSError) as e:
            messagebox.showerror(APP, str(e))
        mods_refresh()

    def mods_open():
        d = installed_dir()
        if d:
            os.makedirs(mods_folder(d), exist_ok=True)
            os.startfile(mods_folder(d))

    mod_btns = [ttk.Button(mbar, text="On / off", command=mods_toggle, style="Go.TButton"),
                ttk.Button(mbar, text="Up", command=lambda: mods_move(-1)),
                ttk.Button(mbar, text="Down", command=lambda: mods_move(+1)),
                ttk.Button(mbar, text="Add .zip...", command=lambda: mods_add(False)),
                ttk.Button(mbar, text="Add folder...", command=lambda: mods_add(True)),
                ttk.Button(mbar, text="Remove", command=mods_remove),
                ttk.Button(mbar, text="Open folder", command=mods_open)]
    for b in mod_btns:
        b.pack(side="left", padx=3)
    tree.bind("<<TreeviewSelect>>", mods_select)
    tree.bind("<Double-1>", mods_toggle)
    tree.bind("<space>", mods_toggle)

    # -- shared
    def refresh():
        d = installed_dir()
        ok = bool(d)
        if ok:
            disc = ini_str(os.path.join(d, "rugrats.ini"), "game", "disc")
            text = f"Installed in {d}"
            if not os.path.isfile(disc):
                text += "\n\nThe disc image is missing (moved or deleted?). Play will ask for it."
            elif not disc_var.get():
                disc_var.set(disc)
        else:
            text = "Not installed yet. Pick your own disc image of the game below, then press Install."
        status.config(text=text)
        play_btn.config(text="Play" if ok else "Install", state="normal")
        for b in (folder_btn, lnk_btn, unin_btn):
            b.config(state="normal" if ok else "disabled")
        save_btn.config(state="normal" if ok else "disabled")
        inst_btn.config(text="Check disc and reinstall / update" if ok else "Check disc and install", state="normal")
        settings_load()
        mods_refresh()

    def relocate_disc(ini):
        """Disc path in the ini is dead: ask for the image again and check it."""
        p = filedialog.askopenfilename(title="The disc image has moved. Where is it now?",
                                       filetypes=[("PS1 disc image", "*.bin *.cue"), ("All files", "*.*")])
        if not p:
            return False
        try:
            p = resolve_disc(p)
            root.config(cursor="watch")
            root.update()
            verify_disc(p)
        except InstallError as e:
            messagebox.showerror(APP, str(e))
            return False
        finally:
            root.config(cursor="")
        try:
            ini_set(ini, "game", "disc", disc_for_ini(p))
        except (InstallError, OSError) as e:
            messagebox.showerror(APP, str(e))
            return False
        return True

    def add_shortcut():
        d = installed_dir()
        if not d:
            return
        try:
            lnk = make_shortcut(d)
        except InstallError as e:
            messagebox.showerror(APP, str(e))
            return
        messagebox.showinfo(APP, f"Shortcut made:\n{lnk}")

    def do_uninstall():
        d = installed_dir()
        if busy["on"] or not d:
            return
        keep = messagebox.askyesnocancel(
            APP, f"Uninstall the game from\n{d} ?\n\nYour disc image is not touched "
                 "(only a copy made by the installer is removed).\n\n"
                 "Yes = uninstall, keep my settings and saves\n"
                 "No = uninstall and DELETE my settings and saves too\n"
                 "Cancel = do nothing", icon="warning", default="cancel")
        if keep is None:
            return
        if not keep and not messagebox.askyesno(
                APP, f"Delete rugrats.ini and every save in\n{os.path.join(d, 'saves')} ?\n\nThis can't be undone.",
                icon="warning", default="no"):
            return
        try:
            uninstall(d, purge=not keep, log=log)
        except InstallError as e:
            messagebox.showerror(APP, str(e))
        refresh()

    lnk_btn.config(command=add_shortcut)
    unin_btn.config(command=do_uninstall)

    def play():
        d = installed_dir()
        if busy["on"]:
            return
        if not d:
            start_install()
            return
        ini = os.path.join(d, "rugrats.ini")
        if not os.path.isfile(ini_str(ini, "game", "disc")) and not relocate_disc(ini):
            refresh()
            return
        try:
            proc = subprocess.Popen([os.path.join(d, "Rugrats.exe")], cwd=d)
        except OSError as e:
            messagebox.showerror(APP, f"Can't start the game: {e}")
            return
        busy["on"] = True
        root.iconify()

        def wait():
            proc.wait()
            q.put(("game_over", None))
        threading.Thread(target=wait, daemon=True).start()

    play_btn.config(command=play)

    def pump():
        try:
            while True:
                kind, val = q.get_nowait()
                if kind == "log":
                    log(val)
                elif kind == "bar":
                    bar.config(value=val)
                elif kind == "done":
                    busy["on"] = False
                    bar.config(value=1.0)
                    refresh()
                    nb.select(play_tab)
                    messagebox.showinfo(APP, "Installed. Press Play to start the game.")
                elif kind == "fail":
                    busy["on"] = False
                    bar.config(value=0)
                    log("FAILED: " + val)
                    refresh()
                    messagebox.showerror(APP, val)
                elif kind == "game_over":
                    busy["on"] = False
                    root.deiconify()
                    refresh()  # the game's F1 menu may have changed settings
        except queue.Empty:
            pass
        root.after(80, pump)

    refresh()
    tab = os.environ.get("RUGRATS_LAUNCHER_TAB")  # testing: open on a given tab (0 play, 1 settings, 2 mods, "1,3" = settings page 3)
    if tab:
        nb.select(int(tab.split(",")[0]))
        if "," in tab:
            snb.select(int(tab.split(",")[1]))
    pump()
    root.update_idletasks()
    dark_titlebar(root)
    root.deiconify()
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--install", metavar="DISC", help="install without the window")
    ap.add_argument("--dir", help="install folder")
    ap.add_argument("--cc", default="", help="clang to use")
    ap.add_argument("--copy-disc", action="store_true")
    ap.add_argument("--uninstall", action="store_true", help="remove the installed game (keeps settings and saves)")
    ap.add_argument("--purge", action="store_true", help="with --uninstall: delete settings and saves too")
    ap.add_argument("--shortcut", action="store_true", help="make a desktop shortcut to the installed game")
    ap.add_argument("--check", action="store_true", help="compare the settings table with host_win32.c")
    ap.add_argument("--validate-mod", metavar="DIR", help="check a mod folder the way the game reads it")
    ap.add_argument("--list-mods", action="store_true", help="list the installed mods in load order")
    ap.add_argument("--add-mod", metavar="ZIP_OR_DIR", help="install a mod (add --replace to overwrite)")
    ap.add_argument("--replace", action="store_true")
    a = ap.parse_args()
    if a.check:
        sys.exit(0 if check_table() else 1)
    if a.validate_mod:
        m = parse_mod(a.validate_mod)
        check_mod_files(m, open_disc(a.dir or installed_dir()))
        print(f"{m['id']}: {m['name']} {m['version']} - {mod_summary(m)}")
        sys.exit(1 if m["error"] else 0)
    if a.list_mods or a.add_mod:
        d = a.dir or installed_dir()
        if not d:
            sys.exit("no installed game found")
        if a.add_mod:
            try:
                m = add_mod(a.add_mod, d, a.replace)
            except (ModError, OSError) as e:
                sys.exit(f"can't add the mod: {e}")
            print(f"added {m['id']}: {mod_summary(m)}")
        mods = list_mods(d)
        for m in mods:
            print(f"{'ON ' if m['on'] else 'off'} {m['id']:24} {m['version']:8} {mod_summary(m)}")
        for w in mod_conflicts(mods):
            print("warning:", w)
        return
    if a.uninstall or a.shortcut:
        d = a.dir or installed_dir()
        if not d or not os.path.isfile(os.path.join(d, "Rugrats.exe")):
            sys.exit("no installed game found")
        try:
            if a.shortcut:
                print("Shortcut:", make_shortcut(d))
            else:
                uninstall(d, a.purge)
        except InstallError as e:
            sys.exit(str(e))
        return
    if a.install:
        try:
            install(a.install, a.dir or cfg_load().get("install_dir") or default_install_dir(), a.cc, a.copy_disc)
        except InstallError as e:
            sys.exit(f"install failed: {e}")
        return
    try:
        gui()
    except Exception:  # under pythonw (Launcher.bat) there is no console to show a crash
        import traceback
        ctypes.windll.user32.MessageBoxW(None, traceback.format_exc()[-1500:], APP + " - Launcher error", 0x10)
        raise


if __name__ == "__main__":
    main()
