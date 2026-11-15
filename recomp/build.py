"""Build a native Windows executable from Rugrats: Search for Reptar (USA) disc image.

usage: python -I build.py <path to .bin> [--out DIR] [--cc clang]
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "tools")
RUNTIME = os.path.join(HERE, "runtime")


FPS_INI = """; Frames per second during gameplay (menus and cutscenes keep their own rate).
; The game's clock runs at 300 Hz, so 60, 75, 100, 150 and 300 give perfectly even motion.
; 120/144/240 work at correct speed with slight unevenness. F8 cycles the rate in game.
fps=60
; Virtual PS1 CPU multiplier: headroom so heavy scenes keep up at high fps. 1 = original console.
overclock=4
; 1 = game time follows real elapsed time (fixes the game running 2x fast when frames finish early).
realtime_step=1
"""


def run(cmd):
    print(">", " ".join(cmd))
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(f"step failed: {' '.join(cmd)}")


def find_cc(name):
    p = shutil.which(name)
    if p:
        return p
    cand = os.path.expanduser("~/.local/share/retcomm/toolchains/cmake-clang-v1/latest/bin/" + name + ".exe")
    if os.path.exists(cand):
        return cand
    sys.exit(f"C compiler '{name}' not found; pass --cc")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disc")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "build"))
    ap.add_argument("--cc", default="clang")
    ap.add_argument("--debug", action="store_true")
    a = ap.parse_args()
    disc = os.path.abspath(a.disc)
    out = os.path.abspath(a.out)
    ext, gen, obj = (os.path.join(out, d) for d in ("extract", "gen", "obj"))
    py = sys.executable

    run([py, "-I", os.path.join(TOOLS, "extract.py"), disc, ext])
    run([py, "-I", os.path.join(TOOLS, "analyze.py"), ext])
    if os.path.isdir(gen):
        shutil.rmtree(gen)
    run([py, "-I", os.path.join(TOOLS, "recomp.py"), ext, gen])

    cc = find_cc(a.cc)
    os.makedirs(obj, exist_ok=True)
    opt = ["-O0", "-g"] if a.debug else ["-O2"]
    common = ["-std=c11", "-I", RUNTIME, "-I", gen, "-w", "-fwrapv", "-fno-strict-aliasing"] + opt
    srcs = [os.path.join(gen, f) for f in sorted(os.listdir(gen)) if f.endswith(".c")]
    srcs += [os.path.join(RUNTIME, f) for f in sorted(os.listdir(RUNTIME)) if f.endswith(".c")]

    def compile_one(src):
        o = os.path.join(obj, os.path.basename(src) + ".o")
        flags = list(common)
        if os.path.basename(src).startswith("recomp_") and not a.debug:
            flags = [f for f in flags if f != "-O2"] + ["-O1"]  # huge generated functions: keep compile time sane
        r = subprocess.run([cc] + flags + ["-c", src, "-o", o], capture_output=True, text=True)
        return src, o, r.returncode, r.stderr

    objs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for src, o, rc, err in ex.map(compile_one, srcs):
            print("cc", os.path.basename(src), "ok" if rc == 0 else "FAILED")
            if rc:
                print(err)
                sys.exit(1)
            objs.append(o)

    exe = os.path.join(out, "Rugrats.exe")
    run([cc] + objs + ["-o", exe, "-lgdi32", "-luser32", "-lwinmm", "-ld3d11", "-ldxgi", "-ldxguid", "-lhid", "-lsetupapi", "-lwindowscodecs", "-lole32"] + ([] if a.debug else ["-s"]))

    ini = os.path.join(out, "rugrats.ini")
    if not os.path.exists(ini):
        with open(ini, "w") as f:
            f.write(f"[game]\ndisc={disc}\n\n[video]\n; 1 = 16:9 widescreen, 0 = original 4:3\nwidescreen=1\nscale=3\n"
                    + FPS_INI)
    print(f"\nbuilt {exe}\nconfig {ini}")


if __name__ == "__main__":
    main()
