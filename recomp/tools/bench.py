"""Per-level performance benchmark for the native Rugrats build.

Runs the exe once per (config, level): boots, jumps straight into the level (RUGRATS_LEVEL skips the hub),
waits for its gameplay loop, settles, then measures the real time of every presented frame while a fixed
walk/turn script plays (RUGRATS_BENCH in host_win32.c). Results go to <out>/results.csv, one screenshot per
level to <out>/shots, each run's log to <out>/logs, and a summary table to <out>/report.md.

usage:
  python -I recomp/tools/bench.py run [--exe build/Rugrats.exe] [--levels 0-31] [--configs gpu:4,cpu:4]
                                      [--fps 60] [--settle 300] [--measure 1800] [--out DIR] [--present]
  python -I recomp/tools/bench.py report <results.csv> [<baseline results.csv>]

Configs are renderer:internal (renderer gpu or cpu, internal 1..9). Times are in 60 Hz game frames.
By default the final Present is skipped and the GPU is waited on each frame (RUGRATS_NOPRESENT=1,
RUGRATS_GPUSYNC=1): a window's Present is capped at the display refresh, which would hide the real cost.
--present measures with real presentation instead (capped at the display Hz unless fullscreen + vsync off).

The run never touches rugrats.ini (test mode); renderer, resolution and fps come from the environment,
everything else (widescreen, overclock, sub-pixel) from the ini next to the exe and is recorded in the CSV.
Levels whose gameplay never starts in time are recorded as "timeout"; crashes as "crash(<code>)"; runs that
exceed --proc-timeout seconds are killed and recorded as "hang".
"""
import argparse, csv, datetime, os, shutil, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIELDS = ["build", "level", "status", "renderer", "internal", "fps", "present_fps", "overclock", "widescreen", "frames",
          "avg_ms", "p50_ms", "p95_ms", "p99_ms", "max_ms", "avg_fps", "p99_fps", "game_frames"]

# names from the game's level table / disc layout (database number -> name); unknown ones show as "db NN"
# database number -> name, from the game's own table (name index = short at 0x800B7D60 + 2*db, string pointer at
# 0x800BB4E8 + 4*index)
NAMES = ["Menus", "Pickles' Home (hub)", "Grandpa's Teeth", "Incident on Aisle 7", "7 Voyages of Cynthia",
         "Visitors from Outer Space", "Toy Palace", "Reptar 2010", "Ice Cream Mountain", "The Mysterious Mr. Friend",
         "Chuckie's Glasses", "Circus Angelicus", "Let There Be Light", "Cookie Race", "The Return of Mr. Friend",
         "Mirrorland", "Training Level", "Gold Rush", "Egg Hunt", "Be Bad", "No More Cookies", "Touchdown Tommy",
         "Multiplayer Golf"] + [f"Golf hole {i}" for i in (5, 3, 1, 7, 9, 2, 4, 6, 8)]  # db 23..31: game hole index
                                                                                           # (80057d50) + 1
# databases the released game cannot play from the hub (found 2026-11-06, see README "Per-level performance test"):
#  0 Menus: no gameplay loop; 8 Ice Cream Mountain: LEVEL08.BIN uses an older 32-bit layout the engine can't parse
#  (it reads past RAM and draws nothing); 19 Be Bad: its level file (file id 453) is not on the disc;
#  22 Multiplayer Golf: started from the main menu with player setup, needs file id 328 (not on the disc) from the hub
UNPLAYABLE = {0, 8, 19, 22}


def parse_levels(s):
    if s == "playable":
        return [d for d in range(32) if d not in UNPLAYABLE]
    if s == "all":
        return list(range(32))
    out = []
    for part in s.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += range(int(a), int(b) + 1)
        elif part:
            out.append(int(part))
    return out


def run(a):
    exe = os.path.abspath(a.exe)
    exe_dir = os.path.dirname(exe)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.abspath(a.out or os.path.join(ROOT, "bench", stamp))  # outside build/: kept across builds
    os.makedirs(os.path.join(out, "shots"), exist_ok=True)
    os.makedirs(os.path.join(out, "logs"), exist_ok=True)
    res_csv = os.path.join(out, "results.csv")
    configs = [c.split(":") for c in a.configs.split(",")]
    levels = parse_levels(a.levels)
    total = len(configs) * len(levels)
    print(f"bench: {total} runs -> {out}")
    n = 0
    for rend, res in configs:
        for lv in levels:
            n += 1
            env = dict(os.environ)
            for k in list(env):
                if k.startswith("RUGRATS_"):
                    del env[k]  # only what the bench sets
            env.update({
                "RUGRATS_LEVEL": str(lv), "RUGRATS_BENCH": f"{a.settle},{a.measure}",
                "RUGRATS_BENCH_OUT": res_csv, "RUGRATS_BENCH_TIMEOUT": str(a.timeout),
                "RUGRATS_RENDERER": "1" if rend == "gpu" else "0", "RUGRATS_RES": str(res), "RUGRATS_FPS": str(a.fps),
            })
            for kv in a.env or []:  # --env RUGRATS_X=1 (debug switches for A/B runs)
                k, _, v = kv.partition("=")
                env[k] = v
            if not a.present:
                env["RUGRATS_NOPRESENT"] = "1"
                env["RUGRATS_GPUSYNC"] = "1"
            t0 = time.time()
            status = None
            try:
                p = subprocess.run([exe], cwd=exe_dir, env=env, timeout=a.proc_timeout)
                if p.returncode not in (0, 3):
                    status = f"crash({p.returncode & 0xffffffff:x})"
            except subprocess.TimeoutExpired:
                status = "hang"
            log = os.path.join(exe_dir, "rugrats.log")
            if os.path.exists(log):
                shutil.copy(log, os.path.join(out, "logs", f"{rend}{res}_db{lv:02d}.log"))
            shot = os.path.join(exe_dir, "shots", f"bench_db{lv:02d}.bmp")
            if os.path.exists(shot):
                shutil.move(shot, os.path.join(out, "shots", f"{rend}{res}_db{lv:02d}.bmp"))
            if status:  # the exe could not write its row
                fresh = not os.path.exists(res_csv)
                with open(res_csv, "a", newline="") as f:
                    w = csv.writer(f)
                    if fresh:
                        w.writerow(FIELDS)
                    w.writerow(["?", lv, status, rend, res, a.fps] + [""] * (len(FIELDS) - 6))
            last = read(res_csv)[-1] if os.path.exists(res_csv) else {}
            print(f"[{n}/{total}] {rend}:{res} db {lv:2d}: {last.get('status', status)} avg {last.get('avg_ms', '-')} ms "
                  f"p99 {last.get('p99_ms', '-')} ms ({time.time() - t0:.0f} s)", flush=True)
    for rend, res in configs:
        contact_sheet(os.path.join(out, "shots"), f"{rend}{res}_", os.path.join(out, f"contact_{rend}{res}.png"))
    rep = report(res_csv, a.baseline)
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write(rep)
    print(rep)


def load_bmp(p):
    """32-bit top-down BMP as written by the exe -> (w, h, bytes BGRA)"""
    d = open(p, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    return w, abs(h), d[off:]


def save_png(p, w, h, rgb_rows):
    raw = b"".join(b"\0" + bytes(r) for r in rgb_rows)
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    with open(p, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def contact_sheet(shots, prefix, out, tw=256, th=120, cols=6):
    """one thumbnail per level (file order = level order): a quick look that every level really loaded"""
    files = sorted(f for f in os.listdir(shots) if f.startswith(prefix) and f.endswith(".bmp"))
    if not files:
        return
    rows = (len(files) + cols - 1) // cols
    img = [bytearray(tw * cols * 3) for _ in range(th * rows)]
    for i, name in enumerate(files):
        w, h, px = load_bmp(os.path.join(shots, name))
        ox, oy = (i % cols) * tw, (i // cols) * th
        for y in range(th):
            src = (y * h // th) * w
            row = img[oy + y]
            for x in range(tw):
                s = (src + x * w // tw) * 4
                row[(ox + x) * 3:(ox + x) * 3 + 3] = bytes((px[s + 2], px[s + 1], px[s]))
    save_png(out, tw * cols, th * rows, img)


def read(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def key(r):
    return (r["renderer"] + ":" + r["internal"], int(r["level"]))


def report(path, baseline=None):
    rows = read(path)
    base = {key(r): r for r in read(baseline)} if baseline else {}
    cfgs = []
    for r in rows:
        c = r["renderer"] + ":" + r["internal"]
        if c not in cfgs:
            cfgs.append(c)
    by = {key(r): r for r in rows}
    levels = sorted({int(r["level"]) for r in rows})
    out = [f"# Rugrats per-level bench\n", f"results: `{path}`" + (f"  baseline: `{baseline}`" if baseline else ""), ""]
    b0 = rows[0]
    out.append(f"build {b0['build']}, fps setting {b0['fps']}, overclock {b0['overclock']}, widescreen {b0['widescreen']}. "
               "avg / p99 = real ms per presented frame (lower is better)" +
               ("; (+x%) = change vs baseline avg" if baseline else "") + ".\n")
    hdr = "| level | " + " | ".join(f"{c} avg | {c} p99" for c in cfgs) + " |"
    out += [hdr, "|" + "---|" * (1 + 2 * len(cfgs))]
    worst = {c: (0.0, None) for c in cfgs}
    for lv in levels:
        cells = []
        for c in cfgs:
            r = by.get((c, lv))
            if not r:
                cells += ["-", "-"]
                continue
            if r["status"] != "ok":
                cells += [r["status"], ""]
                continue
            avg, p99 = float(r["avg_ms"]), float(r["p99_ms"])
            d = ""
            b = base.get((c, lv))
            if b and b["status"] == "ok" and float(b["avg_ms"]) > 0:
                d = f" ({100 * (avg / float(b['avg_ms']) - 1):+.0f}%)"
            cells += [f"{avg:.2f}{d}", f"{p99:.2f}"]
            if p99 > worst[c][0] and lv not in UNPLAYABLE:
                worst[c] = (p99, lv)
        name = NAMES[lv] if 0 <= lv < len(NAMES) else ""
        out.append(f"| {lv:02d} {name}{' (unplayable)' if lv in UNPLAYABLE else ''} | " + " | ".join(cells) + " |")
    out.append("")
    for c in cfgs:
        ok = [r for r in rows if r["renderer"] + ":" + r["internal"] == c and r["status"] == "ok" and int(r["level"]) not in UNPLAYABLE]
        bad = [r for r in rows if r["renderer"] + ":" + r["internal"] == c and r["status"] != "ok" and int(r["level"]) not in UNPLAYABLE]
        if ok:
            mean = sum(float(r["avg_ms"]) for r in ok) / len(ok)
            out.append(f"- **{c}**: {len(ok)} levels ok, mean avg {mean:.2f} ms ({1000 / mean:.0f} fps), worst p99 "
                       f"{worst[c][0]:.2f} ms (db {worst[c][1]}), failed (playable levels): " +
                       (", ".join(f"db {r['level']} {r['status']}" for r in bad) or "none"))
        else:
            out.append(f"- **{c}**: no level measured")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--exe", default=os.path.join(ROOT, "build", "Rugrats.exe"))
    r.add_argument("--levels", default="playable", help="playable (default: 1-31 without 8, 19, 22), all, or e.g. 2,5,7-9")
    r.add_argument("--configs", default="gpu:4,cpu:4")
    r.add_argument("--fps", type=int, default=60)
    r.add_argument("--settle", type=int, default=300, help="60 Hz frames after gameplay starts before measuring")
    r.add_argument("--measure", type=int, default=1800, help="60 Hz frames measured")
    r.add_argument("--timeout", type=int, default=30000, help="60 Hz test frames to wait for the level's gameplay")
    r.add_argument("--proc-timeout", type=int, default=300, help="seconds before a run is killed as a hang")
    r.add_argument("--present", action="store_true", help="measure with real presentation")
    r.add_argument("--baseline", help="older results.csv to compare against in the report")
    r.add_argument("--env", action="append", help="extra environment variable K=V for every run (repeatable)")
    r.add_argument("--out")
    p = sub.add_parser("report")
    p.add_argument("results")
    p.add_argument("baseline", nargs="?")
    a = ap.parse_args()
    if a.cmd == "run":
        run(a)
    else:
        print(report(a.results, a.baseline))


if __name__ == "__main__":
    sys.exit(main())
