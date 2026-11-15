"""Step 2: find functions, BIOS calls, indirect jumps in the boot EXE.

usage: python -I analyze.py <extract_dir>   -> writes analysis.json there
"""
import collections
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mips import Ins  # noqa: E402


def load(extract_dir):
    info = json.load(open(os.path.join(extract_dir, "disc.json")))
    exe = open(os.path.join(extract_dir, "game.exe.bin"), "rb").read()
    text = exe[0x800:0x800 + info["text_size"]]
    return info, text


def decode_all(info, text):
    base = info["text_addr"]
    n = len(text) // 4
    words = struct.unpack("<%dI" % n, text[:n * 4])
    return [Ins(base + i * 4, w) for i, w in enumerate(words)], words


def find_bios_stubs(ins):
    """PsyQ BIOS stub: addiu t2,zero,0xA0|B0|C0 ; jr t2 ; addiu t1,zero,N"""
    stubs = {}
    for i in range(len(ins) - 2):
        a, b, c = ins[i], ins[i + 1], ins[i + 2]
        if (a.name == "addiu" and a.rs == 0 and a.rt == 10 and a.imm in (0xA0, 0xB0, 0xC0)
                and b.name == "jr" and b.rs == 10
                and c.name == "addiu" and c.rs == 0 and c.rt == 9):
            stubs[a.addr] = (a.imm, c.imm)
    return stubs


def main():
    d = sys.argv[1]
    info, text = load(d)
    ins, words = decode_all(info, text)
    base, end = info["text_addr"], info["text_addr"] + len(text)
    in_text = lambda a: base <= a < end and a % 4 == 0  # noqa: E731

    entries = {info["pc0"]}
    jal_targets = collections.Counter()
    for x in ins:
        if x.name == "jal" and in_text(x.target):
            entries.add(x.target)
            jal_targets[x.target] += 1

    stubs = find_bios_stubs(ins)
    entries |= set(stubs)

    # pointers into text found in the data/text image (function pointer tables, jump tables)
    data_ptrs = collections.Counter()
    for i, w in enumerate(words):
        if in_text(w):
            data_ptrs[w] += 1

    # also lui/addiu pairs forming text addresses (function pointers passed in code)
    code_ptrs = set()
    last_lui = {}
    for x in ins:
        if x.name == "lui":
            last_lui[x.rt] = (x.imm << 16, x.addr)
        elif x.name in ("addiu", "ori") and x.rs in last_lui and x.addr - last_lui[x.rs][1] < 64:
            v = last_lui[x.rs][0] + (x.simm if x.name == "addiu" else x.imm)
            v &= 0xFFFFFFFF
            if in_text(v):
                code_ptrs.add(v)

    stats = collections.Counter(x.name for x in ins)
    jr_other = [x.addr for x in ins if x.name == "jr" and x.rs != 31]
    jalr = [x.addr for x in ins if x.name == "jalr"]
    syscalls = [x.addr for x in ins if x.name == "syscall"]
    invalid = sum(1 for x in ins if x.name == "invalid")

    out = {
        "entries": sorted(entries), "stubs": {f"{k:08x}": v for k, v in stubs.items()},
        "data_ptrs": sorted(data_ptrs), "code_ptrs": sorted(code_ptrs),
        "jr_other": jr_other, "jalr": jalr, "syscalls": syscalls,
    }
    json.dump(out, open(os.path.join(d, "analysis.json"), "w"), indent=0)

    print(f"instructions: {len(ins)}  invalid words: {invalid}")
    print(f"jal entries: {len(jal_targets)}  bios stubs: {len(stubs)}  syscalls: {len(syscalls)}")
    print(f"jr (non-ra): {len(jr_other)}  jalr: {len(jalr)}")
    print(f"text pointers in image: {len(data_ptrs)}  lui/addiu text pointers: {len(code_ptrs)}")
    print("cop2:", stats["cop2"], "lwc2:", stats["lwc2"], "swc2:", stats["swc2"],
          "mtc2:", stats["mtc2"], "mfc2:", stats["mfc2"], "ctc2:", stats["ctc2"], "cfc2:", stats["cfc2"])
    print("cop0:", stats["mtc0"], stats["mfc0"], "rfe:", stats["rfe"], "lwl/lwr:", stats["lwl"], stats["lwr"])
    calls = collections.Counter()
    for x in ins:
        if x.name == "jal" and x.target in stubs:
            calls[stubs[x.target]] += 1
    print("BIOS functions:", " ".join(f"{t:X}:{n:02X}" for t, n in sorted(set(stubs.values()))))


if __name__ == "__main__":
    main()
