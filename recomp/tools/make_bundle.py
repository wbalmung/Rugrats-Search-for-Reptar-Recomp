"""Make the ready-to-run release folder: our source + a private Python + the C compiler. No game data.

usage: python -I recomp/tools/make_bundle.py [--out DIR] [--no-zip]

Result (default dist/RugratsRecomp/ and dist/RugratsRecomp.zip next to the repo's recomp/ folder):
  RugratsLauncher.exe  double-click start (no console; built here from recomp/tools/launcher_stub.c + a drawn icon)
  Launcher.bat    the same as a batch file; both use python\\ and toolchain\\ below, so the PC needs nothing installed
  recomp/ ...     the same source files git would publish (every file goes through release_check first)
  python/         copy of the Python this script runs with (python.org build with tcl/tk), trimmed
  toolchain/      llvm-mingw (clang + lld + x86_64 Windows libraries), the pinned release below, trimmed
  licenses/       licence texts of Python and llvm-mingw

The user still supplies the disc image: the game is built from it on their PC. Nothing from the game is in the
bundle, and dist/ is never committed (see PUBLISHING.md).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import release_check  # noqa: E402

# Pinned compiler: official llvm-mingw release (https://github.com/mstorsjo/llvm-mingw/releases).
LLVM_NAME = "llvm-mingw-20261104-ucrt-x86_64"
LLVM_URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/{LLVM_NAME}.zip"
LLVM_SHA256 = "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f"

# Of the toolchain's bin/ only these are needed to compile and link C for x86_64.
TC_BIN = re.compile(r"(clang\.exe|clang-\d+\.exe|ld\.lld\.exe|libLLVM-\d+\.dll|libclang-cpp\.dll|libc\+\+\.dll|libunwind\.dll|"
                    r"libwinpthread-1\.dll|x86_64-w64-windows-gnu\.cfg|mingw32-common\.cfg)")
TC_LIB_SKIP = re.compile(r"(libc\+\+|libomp|libclang_rt\.(asan|ubsan|tsan|msan|fuzzer|profile|xray|orc))", re.I)
PY_LIB_SKIP = {"test", "idlelib", "site-packages", "ensurepip", "turtledemo", "venv", "pydoc_data", "__phello__",
               "lib2to3", "unittest", "doctest.py", "turtle.py", "pydoc.py"}
PY_ANY_SKIP = {"__pycache__", "tzdata", "demos"}


def step(s):
    print(s, flush=True)


def source_files():
    """Our own files, exactly as the allow-list .gitignore would publish them."""
    out = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"], cwd=ROOT, capture_output=True,
                         check=True).stdout.decode("utf-8").split("\0")
    return sorted(p for p in out if p and os.path.isfile(os.path.join(ROOT, p)))


def copy_source(dst):
    priv = release_check.private_patterns()
    files, bad = source_files(), []
    for rel in files:
        data = open(os.path.join(ROOT, rel), "rb").read()
        for why in release_check.check_file(rel, data, priv):
            bad.append(f"  {rel}: {why}")
    if bad:
        sys.exit("refused: these files may not be published (same rules as release_check.py)\n" + "\n".join(bad))
    for rel in files:
        t = os.path.join(dst, rel)
        os.makedirs(os.path.dirname(t), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, rel), t)
    step(f"source: {len(files)} files (release check ok)")


def copy_python(dst):
    base = sys.base_prefix
    need = [os.path.join(base, n) for n in ("python.exe", "pythonw.exe", "tcl", os.path.join("DLLs", "_tkinter.pyd"))]
    miss = [n for n in need if not os.path.exists(n)]
    if miss:
        sys.exit("this Python can't be bundled (need the python.org Windows build with tcl/tk): missing " + ", ".join(miss))
    os.makedirs(dst)
    for n in os.listdir(base):
        if re.fullmatch(r"(python(w)?\.exe|python3\d*\.dll|vcruntime\d+(_\d)?\.dll|LICENSE\.txt)", n, re.I):
            shutil.copy2(os.path.join(base, n), dst)

    def skip(top):
        return lambda d, names: [n for n in names if n in PY_ANY_SKIP or
                                 (os.path.normcase(d) == os.path.normcase(top) and n in PY_LIB_SKIP)]
    for sub in ("DLLs", "Lib", "tcl"):
        src = os.path.join(base, sub)
        shutil.copytree(src, os.path.join(dst, sub), ignore=skip(src))
    r = subprocess.run([os.path.join(dst, "python.exe"), "-I", "-c",
                        "import sys, tkinter, ctypes, hashlib, json, queue, argparse, concurrent.futures, "
                        "subprocess, shutil; r = tkinter.Tk(); r.withdraw(); r.destroy(); print(sys.prefix)"],
                       capture_output=True, text=True)
    if r.returncode or os.path.normcase(r.stdout.strip()) != os.path.normcase(dst):
        sys.exit("the bundled Python does not work:\n" + r.stdout + r.stderr)
    step(f"python: {sys.version.split()[0]} copied from {base}")


def fetch_toolchain(cache):
    z = os.path.join(cache, LLVM_NAME + ".zip")
    if not os.path.exists(z):
        os.makedirs(cache, exist_ok=True)
        step(f"downloading {LLVM_URL} (about 180 MB)")
        req = urllib.request.Request(LLVM_URL, headers={"User-Agent": "generic-client"})
        with urllib.request.urlopen(req) as r, open(z + ".part", "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(z + ".part", z)
    h = hashlib.sha256()
    with open(z, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    if h.hexdigest() != LLVM_SHA256:
        sys.exit(f"{z}: wrong SHA256 ({h.hexdigest()}); delete it and run again")
    return z


def want_toolchain(rel):
    p = rel.split("/")
    if rel == "LICENSE.TXT":
        return True
    if p[0] == "bin":
        return len(p) == 2 and bool(TC_BIN.fullmatch(p[1]))
    if p[0] == "include":
        return p[1] != "c++"
    if p[0] == "x86_64-w64-mingw32":
        return p[1] == "lib" and not TC_LIB_SKIP.match(p[-1])
    if p[:2] == ["lib", "clang"]:  # compiler's own headers + the x86_64 builtins library
        return "include" in p[3:4] or "builtins-x86_64" in p[-1]
    return False


def copy_toolchain(dst, cache):
    z = zipfile.ZipFile(fetch_toolchain(cache))
    n = 0
    for i in z.infolist():
        rel = i.filename.split("/", 1)[1] if "/" in i.filename else ""
        if i.is_dir() or not rel or not want_toolchain(rel):
            continue
        t = os.path.normpath(os.path.join(dst, rel))
        if not t.startswith(os.path.normpath(dst) + os.sep):
            sys.exit("bad path in the toolchain zip: " + i.filename)
        os.makedirs(os.path.dirname(t), exist_ok=True)
        with z.open(i) as s, open(t, "wb") as f:
            shutil.copyfileobj(s, f, 1 << 20)
        n += 1
    step(f"toolchain: {n} files from {LLVM_NAME}")


def test_compile(out):
    """The bundled compiler must build and link a Windows program with the libraries build.py asks for."""
    tmp = os.path.join(out, "_cc_test")
    os.makedirs(tmp)
    c = os.path.join(tmp, "t.c")
    with open(c, "w") as f:
        f.write("#include <windows.h>\n#include <d3d11.h>\n#include <stdio.h>\n#include <math.h>\n"
                "int main(void) { printf(\"ok %d\\n\", (int)sqrt(49.0)); return 0; }\n")
    exe = os.path.join(tmp, "t.exe")
    env = {k: v for k, v in os.environ.items() if k.upper() in ("SYSTEMROOT", "TEMP", "TMP")}
    env["PATH"] = os.path.join(os.environ.get("SYSTEMROOT", r"C:\Windows"), "System32")  # nothing else may help
    r = subprocess.run([os.path.join(out, "toolchain", "bin", "clang.exe"), "-std=c11", "-O2", c, "-o", exe, "-lgdi32",
                        "-luser32", "-lwinmm", "-ld3d11", "-ldxgi", "-ldxguid", "-lhid", "-lsetupapi", "-lwindowscodecs", "-lole32", "-s"],
                       capture_output=True, text=True, env=env)
    if r.returncode == 0:
        r = subprocess.run([exe], capture_output=True, text=True, env=env)
    if r.returncode or r.stdout.strip() != "ok 7":
        sys.exit("the bundled compiler does not work:\n" + r.stdout + r.stderr)
    shutil.rmtree(tmp)
    step("toolchain: test program compiled, linked and ran")


def draw_icon(size):
    """Our own app icon (no game artwork): a purple rounded square in the launcher's colours with a white "R".
    Returns bottom-up BGRA rows. Drawn with 4x4 supersampling."""
    top, bot = (0x3b, 0x1a, 0x7e), (0xa8, 0x55, 0xf7)   # launcher.py C["head_a"] / C["head_b"]

    def seg_dist(px, py, ax, ay, bx, by):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        return ((px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2) ** 0.5

    def in_square(x, y):
        r, lo, hi = 0.2, 0.04, 0.96
        cx, cy = min(max(x, lo + r), hi - r), min(max(y, lo + r), hi - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and lo <= x <= hi and lo <= y <= hi

    def in_r(x, y):
        if 0.30 <= x <= 0.42 and 0.21 <= y <= 0.80:                      # stem
            return True
        if 0.36 <= x <= 0.48 and (0.21 <= y <= 0.325 or 0.415 <= y <= 0.53):   # bowl's top and middle bars
            return True
        d = ((x - 0.48) ** 2 + (y - 0.37) ** 2) ** 0.5                     # bowl's round right side
        if x >= 0.48 and 0.045 <= d <= 0.16:
            return True
        return seg_dist(x, y, 0.49, 0.50, 0.68, 0.80) <= 0.057           # leg

    n = 4
    rows = []
    for py in range(size - 1, -1, -1):          # bottom-up
        row = bytearray()
        for px in range(size):
            a = w = 0
            for sy in range(n):
                for sx in range(n):
                    x, y = (px + (sx + 0.5) / n) / size, (py + (sy + 0.5) / n) / size
                    if in_square(x, y):
                        a += 1
                        w += in_r(x, y)
            t = py / max(1, size - 1)
            base = [top[i] + (bot[i] - top[i]) * t for i in range(3)]
            if a:
                f = w / a
                col = [round(base[i] * (1 - f) + 255 * f) for i in range(3)]
            else:
                col = [0, 0, 0]
            row += bytes((col[2], col[1], col[0], round(255 * a / (n * n))))
        rows.append(bytes(row))
    return rows


def icon_res(path):
    """Write the icon as a Windows .res file (RT_ICON images + one RT_GROUP_ICON), which lld links directly."""
    import struct

    def entry(rtype, rid, data, flags):
        hdr = struct.pack("<IIHHHHIHHII", len(data), 32, 0xFFFF, rtype, 0xFFFF, rid, 0, flags, 0x0409, 0, 0)
        return hdr + data + b"\0" * (-len(data) % 4)
    out = struct.pack("<IIHHHHIHHII", 0, 32, 0xFFFF, 0, 0xFFFF, 0, 0, 0, 0, 0, 0)   # the empty first entry
    group = b""
    sizes = (16, 24, 32, 48, 64, 256)
    for i, s in enumerate(sizes, 1):
        rows = draw_icon(s)
        mask_row = b"\0" * (((s + 31) // 32) * 4)            # alpha does the work; AND mask all 0
        img = struct.pack("<IiiHHIIiiII", 40, s, s * 2, 1, 32, 0, 0, 0, 0, 0, 0) + b"".join(rows) + mask_row * s
        out += entry(3, i, img, 0x1010)
        group += struct.pack("<BBBBHHIH", s % 256, s % 256, 0, 0, 1, 32, len(img), i)
    out += entry(14, 1, struct.pack("<HHH", 0, 1, len(sizes)) + group, 0x1030)
    with open(path, "wb") as f:
        f.write(out)


def build_launcher_exe(out):
    """RugratsLauncher.exe (recomp/tools/launcher_stub.c): double-click start without a console window, compiled
    here with the bundled clang, then started once in wait mode with --check to prove it finds Python + launcher."""
    tmp = os.path.join(out, "_stub")
    os.makedirs(tmp)
    res = os.path.join(tmp, "icon.res")
    icon_res(res)
    exe = os.path.join(out, "RugratsLauncher.exe")
    env = {k: v for k, v in os.environ.items() if k.upper() in ("SYSTEMROOT", "TEMP", "TMP")}
    env["PATH"] = os.path.join(os.environ.get("SYSTEMROOT", r"C:\Windows"), "System32")
    r = subprocess.run([os.path.join(out, "toolchain", "bin", "clang.exe"), "-O2", "-municode", "-mwindows",
                        os.path.join(out, "recomp", "tools", "launcher_stub.c"), res, "-o", exe, "-s"],
                       capture_output=True, text=True, env=env)
    if r.returncode:
        sys.exit("could not build RugratsLauncher.exe:\n" + r.stdout + r.stderr)
    r = subprocess.run([exe, "--check"], capture_output=True, text=True, env=dict(env, RUGRATS_STUB_WAIT="1"))
    if r.returncode:
        sys.exit(f"RugratsLauncher.exe --check failed (exit {r.returncode})")
    shutil.rmtree(tmp)
    step("launcher: RugratsLauncher.exe built and started the bundled launcher (--check ok)")


def write_licenses(out):
    d = os.path.join(out, "licenses")
    os.makedirs(d)
    shutil.copyfile(os.path.join(out, "python", "LICENSE.txt"), os.path.join(d, "Python-LICENSE.txt"))
    shutil.copyfile(os.path.join(out, "toolchain", "LICENSE.TXT"), os.path.join(d, "llvm-mingw-LICENSE.txt"))
    with open(os.path.join(d, "README.txt"), "w") as f:
        f.write("Third-party programs shipped in this folder so the game can be built on your PC:\n\n"
                f"python\\     Python {sys.version.split()[0]}, https://www.python.org  (Python-LICENSE.txt)\n"
                f"toolchain\\  {LLVM_NAME}, https://github.com/mstorsjo/llvm-mingw  (llvm-mingw-LICENSE.txt)\n"
                "            LLVM/clang/lld, the mingw-w64 headers and libraries, compiler-rt; unused parts removed.\n\n"
                "Neither is changed. The game itself is not included: it is built from your own disc image.\n")


def refuse_game_data(out):
    """Last check on the finished folder: no file type that could be game data, and no built game."""
    bad = []
    ours = re.compile(r"\.(bin|cue|iso|img|chd|pbp|ecm|state|mcd|mcr|sav|str|xa|tim|vab|vh|vb|ini|log|bps|ips|xdelta|tex|fnt)$", re.I)
    for d, _, names in os.walk(out):
        rel = os.path.relpath(d, out).replace(os.sep, "/")
        third = rel.split("/")[0] in ("python", "toolchain")
        for n in names:
            if release_check.MOD_INI.fullmatch(rel + "/" + n):   # example mods we wrote (checked by release_check)
                continue
            if rel == "." and n == "RugratsLauncher.exe":            # our own stub, built above from launcher_stub.c
                continue
            if n.lower() == "rugrats.exe" or (not third and (ours.search(n) or n.lower().endswith((".exe", ".dll")))):
                bad.append(os.path.join(rel, n))
    if bad:
        sys.exit("refused: the bundle holds files that must not ship:\n  " + "\n  ".join(bad))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "dist", "RugratsRecomp"))
    ap.add_argument("--no-zip", action="store_true")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        if not os.path.isfile(os.path.join(out, "Launcher.bat")):
            sys.exit(f"{out} exists and is not an earlier bundle; pick another --out")
        shutil.rmtree(out)
    os.makedirs(out)
    copy_source(out)
    copy_python(os.path.join(out, "python"))
    copy_toolchain(os.path.join(out, "toolchain"), os.path.join(ROOT, "dist", "cache", "llvm-mingw"))
    test_compile(out)
    build_launcher_exe(out)
    write_licenses(out)
    refuse_game_data(out)
    size = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
    step(f"bundle: {out} ({size / 2**20:.0f} MB)")
    if not a.no_zip:
        z = shutil.make_archive(out, "zip", os.path.dirname(out), os.path.basename(out))
        step(f"zip: {z} ({os.path.getsize(z) / 2**20:.0f} MB)")


if __name__ == "__main__":
    main()
