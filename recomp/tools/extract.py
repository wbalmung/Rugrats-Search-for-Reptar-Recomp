"""Step 1: verify the disc image and pull out the boot EXE.

usage: python -I extract.py <disc.bin> <outdir>
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psxdisc import Disc, EXPECTED_SHA1, sha1_file  # noqa: E402


def main():
    disc_path, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    print("hashing disc image...")
    h = sha1_file(disc_path)
    if h != EXPECTED_SHA1:
        sys.exit(f"SHA1 mismatch: got {h}, want {EXPECTED_SHA1}. Only the USA SLUS-00650 image is supported.")
    print("SHA1 ok")
    d = Disc(disc_path)
    exe_name = d.boot_exe_name()
    exe = d.read_file(exe_name)
    if exe[:8] != b"PS-X EXE":
        sys.exit("boot file is not a PS-X EXE")
    pc0, gp0, taddr, tsize = struct.unpack("<IIII", exe[16:32])
    sp = struct.unpack("<I", exe[48:52])[0]
    with open(os.path.join(out, "game.exe.bin"), "wb") as f:
        f.write(exe)
    info = {"exe": exe_name, "pc0": pc0, "gp0": gp0, "text_addr": taddr, "text_size": tsize,
            "sp": sp, "disc_sha1": h,
            "files": {k: list(v) for k, v in sorted(d.files.items())}}
    with open(os.path.join(out, "disc.json"), "w") as f:
        json.dump(info, f, indent=1)
    print(f"{exe_name}: entry {pc0:08x}, load {taddr:08x}+{tsize:x}, {len(d.files)} files listed")


if __name__ == "__main__":
    main()
