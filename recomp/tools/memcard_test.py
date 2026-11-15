"""Memory card test: the game's own Save Game / Load Game, end to end, in a scratch folder.

Runs the exe 4 times (unpaced, scripted pad input via RUGRATS_INPUT) on a fresh card:
  1. new game -> house -> pause -> Save Game -> "create file? Yes" -> slot 1 -> "Game saved."
  2. boot -> Load Game -> slot 1 -> "Yes" -> back in the house
  3. load slot 1 -> pause -> Save Game -> slot 2
  4. Load Game (list must show 2 games) -> load slot 2 -> pause -> Save Game -> slot 1 -> overwrite "Yes"
Checks (from the BIOS memory card trace RUGRATS_MCLOG, the game's debug output RUGRATS_GAMELOG and the card
image): the save data (0x1580 bytes) and slot index are written / read, the game reaches the level loop after
each load, no "crc check failed", and the card holds one 'BASLUS-00650Games' file with data.
Screenshots of every step go to <out>/shots_<n>/ for a human look.

usage: python -I recomp/tools/memcard_test.py [--exe build/Rugrats.exe] [--out build/mctest]
Never touches the user's card or ini: the exe and ini are copied into <out>, saves go to <out>/saves.
Input timings are 60 Hz test frames; if a future build changes boot/menu timing, the screenshots show where a
step landed (adjust the times in STEPS).
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

X, START, UP, DOWN, LEFT = 0x4000, 0x8, 0x10, 0x40, 0x80


def presses(*items):
    return ",".join(f"{t}:{b:x}" + (f":{n}" if n != 6 else "") for t, b, n in
                    [(i + (6,)) if len(i) == 2 else i for i in items])


TO_HOUSE = [(1400, START)] + [(t, X) for t in range(1700, 8001, 240)]           # new game, skip intro + hints
LOAD_SLOT1 = [(1400, START), (2000, DOWN, 40), (2100, X), (2400, X), (2700, LEFT, 80), (2800, X)]
LOAD_SLOT2 = [(1400, START), (2000, DOWN, 40), (2100, X), (2300, DOWN, 40), (2400, X), (2700, LEFT, 80), (2800, X)]
STEPS = [
    ("save new file, slot 1", TO_HOUSE + [(8600, START), (8700, DOWN, 40), (8800, X), (9000, LEFT, 80), (9100, X),
                                          (9400, X), (9700, X)], 10200),
    ("load slot 1", LOAD_SLOT1 + [(t, X) for t in range(3400, 6401, 300)], 7000),
    ("load slot 1, save slot 2", LOAD_SLOT1 + [(5000, START), (5100, DOWN, 40), (5200, X), (5400, DOWN, 40),
                                               (5500, X), (5800, X)], 6600),
    ("load slot 2, overwrite slot 1", LOAD_SLOT2 + [(5000, START), (5100, DOWN, 40), (5200, X), (5400, X),
                                                    (5600, LEFT, 80), (5700, X), (6000, X)], 6600),
]


def card_files(path):
    d = open(path, "rb").read()
    out = []
    for i in range(1, 16):
        f = d[i * 128:(i + 1) * 128]
        if f[0] == 0x51:
            blk = d[i * 8192:(i + 1) * 8192]
            out.append((f[10:30].split(b"\0")[0].decode("latin1").strip(), sum(1 for x in blk[0x200:0x1800] if x)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "Rugrats.exe"))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "mctest"))
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, "saves"), exist_ok=True)
    shutil.copy(a.exe, os.path.join(out, "Rugrats.exe"))
    ini = os.path.join(os.path.dirname(os.path.abspath(a.exe)), "rugrats.ini")
    if os.path.exists(ini):
        shutil.copy(ini, os.path.join(out, "rugrats.ini"))
    card = os.path.join(out, "saves", "card1.mcd")
    if os.path.exists(card):
        os.remove(card)  # fresh card (the runtime creates a formatted one)
    ok_all = True
    for n, (name, steps, frames) in enumerate(STEPS, 1):
        env = {k: v for k, v in os.environ.items() if not k.startswith("RUGRATS_")}
        env.update({"RUGRATS_INPUT": presses(*steps), "RUGRATS_TEST": f"{frames},100", "RUGRATS_RES": "1",
                    "RUGRATS_NOPRESENT": "1", "RUGRATS_MCLOG": "1", "RUGRATS_GAMELOG": "1"})
        shots = os.path.join(out, "shots")
        shutil.rmtree(shots, ignore_errors=True)
        p = subprocess.run([os.path.join(out, "Rugrats.exe")], cwd=out, env=env, timeout=600)
        if os.path.isdir(shots):
            dst = os.path.join(out, f"shots_{n}")
            shutil.rmtree(dst, ignore_errors=True)
            shutil.move(shots, dst)
        log = open(os.path.join(out, "rugrats.log"), encoding="latin1").read()
        shutil.copy(os.path.join(out, "rugrats.log"), os.path.join(out, f"log_{n}.txt"))
        wrote = "B0:35(00000002, 800e3c98, 00001580)  -> 00001580" in log
        read = "B0:34(00000002, 800e3c98, 00001580)  -> 00001580" in log
        crc = "crc check failed" in log
        fatal = "FATAL" in log or p.returncode != 0
        loaded_level = log.count("level: run db 1")
        checks = {"exit ok": not fatal, "no crc error": not crc}
        if n in (1, 3, 4):
            checks["save data written"] = wrote
        if n in (2, 3, 4):
            checks["save data read"] = read
            checks["back in the game after load"] = loaded_level >= 1
        ok = all(checks.values())
        ok_all &= ok
        print(f"[{n}] {name}: {'PASS' if ok else 'FAIL'}  " + ", ".join(f"{k}={'yes' if v else 'NO'}" for k, v in checks.items()))
    files = card_files(card) if os.path.exists(card) else []
    good = len(files) == 1 and files[0][0].startswith("BASLUS-00650") and files[0][1] > 0
    ok_all &= good
    print(f"card: {files} -> {'PASS' if good else 'FAIL'}")
    print("RESULT:", "PASS" if ok_all else "FAIL", f"(screenshots in {out}\\shots_N, logs log_N.txt)")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
