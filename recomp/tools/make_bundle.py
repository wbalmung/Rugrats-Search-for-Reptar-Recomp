"""Make the ready-to-run release folder: our source + a private Python + the C compiler. No game data.

usage: python -I recomp/tools/make_bundle.py [--out DIR] [--no-zip]

Result (default dist/RugratsRecomp/ and dist/RugratsRecomp.zip next to the repo's recomp/ folder):
  Launcher.bat    double-click start; uses python\\ and toolchain\\ below, so the PC needs nothing installed
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
    write_licenses(out)
    refuse_game_data(out)
    size = sum(os.path.getsize(os.path.join(d, n)) for d, _, ns in os.walk(out) for n in ns)
    step(f"bundle: {out} ({size / 2**20:.0f} MB)")
    if not a.no_zip:
        z = shutil.make_archive(out, "zip", os.path.dirname(out), os.path.basename(out))
        step(f"zip: {z} ({os.path.getsize(z) / 2**20:.0f} MB)")


if __name__ == "__main__":
    main()
