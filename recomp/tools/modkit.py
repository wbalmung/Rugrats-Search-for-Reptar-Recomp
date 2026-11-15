"""Modder's kit for Rugrats: Search for Reptar (native port). Works on YOUR disc image; ships nothing from the game.

  python -I recomp/tools/modkit.py list [text]                    files the game loads (table id, LBA, size, kind)
  python -I recomp/tools/modkit.py extract <disc path> <out>      copy one file out (streams: raw 2352-byte sectors)
  python -I recomp/tools/modkit.py diff <disc path> <edited> <out.bps>   BPS patch from the disc file to your edit
  python -I recomp/tools/modkit.py bps <original> <edited> <out.bps>     BPS patch between two files
  python -I recomp/tools/modkit.py apply <patch.bps> <original> <out>    apply a BPS patch (to check one)
  python -I recomp/tools/modkit.py tim2png <file.tim> <out.png>          picture out of a TIM (first palette)
  python -I recomp/tools/modkit.py png2tim <orig.tim> <edited.png> <out.tim>   edited picture back into the TIM
  python -I recomp/tools/modkit.py pack <mod folder> <out.zip>           check a mod and zip it for sharing

The disc image is the one in the installed game's rugrats.ini (or --disc <.bin>). Format of mods: recomp/MODDING.md.
Share patches (.bps) of game files, never the files themselves.
"""
import argparse
import os
import re
import struct
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from psxdisc import SECTOR, Disc  # noqa: E402

FILE_TAB, FILE_TAB_N = 0x800B2C40, 714


def find_disc(arg):
    if arg:
        return arg
    import launcher
    d = launcher.installed_dir()
    p = launcher.ini_str(os.path.join(d, "rugrats.ini"), "game", "disc") if d else ""
    if not p or not os.path.isfile(p):
        sys.exit("no disc image: pass --disc <your .bin> (or install the game with the launcher first)")
    return p


def norm(p):
    return "/" + p.replace("\\", "/").strip("/").split(";")[0].upper()


class Game:
    def __init__(self, path):
        self.disc = Disc(path)
        self.stream = {}
        exe = self.disc.read_file(self.disc.boot_exe_name())
        taddr = struct.unpack_from("<I", exe, 24)[0]
        o = 0x800 + FILE_TAB - taddr
        self.tab = [struct.unpack_from("<II", exe, o + 8 * i) for i in range(FILE_TAB_N)]

    def is_stream(self, name):
        """Same rule as mods.c: real-time / form-2 sectors among the first 8 = movie or XA audio."""
        if name not in self.stream:
            lba, size, _ = self.disc.files[name]
            self.stream[name] = any(self.disc.raw(lba + k)[15] == 2 and self.disc.raw(lba + k)[18] & 0x60
                                    for k in range(min(8, (size + 2047) // 2048)))
        return self.stream[name]

    def ids(self, name):
        lba = self.disc.files[name][0]
        return [i for i, (s, _) in enumerate(self.tab) if s == lba]

    def get(self, name):
        name = norm(name)
        if name not in self.disc.files:
            sys.exit(f"no file {name} on the disc (see: modkit.py list)")
        return name

    def data(self, name):
        lba, size, _ = self.disc.files[name]
        if self.is_stream(name):
            return b"".join(self.disc.raw(lba + k) for k in range((size + 2047) // 2048))
        return self.disc.read_extent(lba, size)


# ---- BPS (beat patch format by byuu; spec: github.com/blakesmith/beat/blob/master/doc/bps.txt) ------------------
def _enc(n):
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1


def _dec(p, i):
    d, sh = 0, 1
    while True:
        x = p[i]
        i += 1
        d += (x & 0x7F) * sh
        if x & 0x80:
            return d, i
        sh <<= 7
        d += sh


def bps_make(src, tgt):
    out = bytearray(b"BPS1" + _enc(len(src)) + _enc(len(tgt)) + _enc(0))
    K = 8
    index = {}
    for i in range(0, len(src) - K + 1):
        index.setdefault(src[i:i + K], i)
    o, srel, pend = 0, 0, bytearray()

    def flush():
        if pend:
            out.extend(_enc(((len(pend) - 1) << 2) | 1) + pend)
            pend.clear()
    while o < len(tgt):
        n = 0
        while o + n < len(tgt) and o + n < len(src) and src[o + n] == tgt[o + n]:
            n += 1
        if n >= 4 or (n and o + n == len(tgt)):
            flush()
            out.extend(_enc(((n - 1) << 2) | 0))
            o += n
            continue
        p = index.get(tgt[o:o + K])
        if p is not None:
            n = 0
            while o + n < len(tgt) and p + n < len(src) and src[p + n] == tgt[o + n]:
                n += 1
            if n >= 16:
                flush()
                d = p - srel
                out.extend(_enc(((n - 1) << 2) | 2) + _enc((abs(d) << 1) | (d < 0)))
                srel = p + n
                o += n
                continue
        pend.append(tgt[o])
        o += 1
    flush()
    out += struct.pack("<II", zlib.crc32(src), zlib.crc32(tgt))
    out += struct.pack("<I", zlib.crc32(out))
    return bytes(out)


def bps_apply(p, src):
    if p[:4] != b"BPS1" or zlib.crc32(p[:-4]) != struct.unpack_from("<I", p, len(p) - 4)[0]:
        raise ValueError("not a BPS patch, or it is damaged")
    c_src, c_tgt = struct.unpack_from("<II", p, len(p) - 12)
    i = 4
    ss, i = _dec(p, i)
    ts, i = _dec(p, i)
    ms, i = _dec(p, i)
    i += ms
    if ss != len(src) or zlib.crc32(src) != c_src:
        raise ValueError("the patch is for a different version of this file")
    t = bytearray()
    srel = trel = 0
    end = len(p) - 12
    while i < end:
        d, i = _dec(p, i)
        n = (d >> 2) + 1
        cmd = d & 3
        if cmd == 0:
            t += src[len(t):len(t) + n]
        elif cmd == 1:
            t += p[i:i + n]
            i += n
        else:
            k, i = _dec(p, i)
            off = -(k >> 1) if k & 1 else (k >> 1)
            if cmd == 2:
                srel += off
                t += src[srel:srel + n]
                srel += n
            else:
                trel += off
                for _ in range(n):
                    t.append(t[trel])
                    trel += 1
    if len(t) != ts or zlib.crc32(t) != c_tgt:
        raise ValueError("the patch doesn't fit this file")
    return bytes(t)


# ---- TIM <-> PNG ------------------------------------------------------------------------------------------------
def tim_parse(b):
    if len(b) < 8 or struct.unpack_from("<I", b, 0)[0] != 0x10:
        raise ValueError("not a TIM image")
    flags = struct.unpack_from("<I", b, 4)[0]
    bpp = {0: 4, 1: 8, 2: 16, 3: 24}[flags & 3]
    o = 8
    clut = None
    if flags & 8:
        ln, cx, cy, cw, ch = struct.unpack_from("<IHHHH", b, o)
        clut = (o, cw, ch)
        o += ln
    ln, ix, iy, iw, ih = struct.unpack_from("<IHHHH", b, o)
    w = {4: iw * 4, 8: iw * 2, 16: iw, 24: iw * 2 // 3}[bpp]
    return {"bpp": bpp, "clut": clut, "pix": o + 12, "w": w, "h": ih, "rowbytes": iw * 2}


def _c15(v):
    return ((v & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v >> 10 & 31) * 255 // 31, 0 if v == 0 else 255)


def _png(w, h, ctype, rows, plte=None, trns=None):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
    if plte:
        out += chunk(b"PLTE", plte)
    if trns:
        out += chunk(b"tRNS", trns)
    return out + chunk(b"IDAT", zlib.compress(b"".join(b"\0" + r for r in rows), 9)) + chunk(b"IEND", b"")


def tim2png(b):
    t = tim_parse(b)
    w, h, bpp = t["w"], t["h"], t["bpp"]
    rows = [b[t["pix"] + y * t["rowbytes"]: t["pix"] + (y + 1) * t["rowbytes"]] for y in range(h)]
    if bpp in (4, 8):
        n = 16 if bpp == 4 else 256
        co = t["clut"][0] + 12 if t["clut"] else None
        pal = [struct.unpack_from("<H", b, co + 2 * k)[0] if co and co + 2 * k + 2 <= len(b) else k * 0x421 % 0x8000
               for k in range(n)]
        cols = [_c15(v) for v in pal]
        plte = b"".join(bytes(c[:3]) for c in cols)
        trns = bytes(c[3] for c in cols)
        if bpp == 4:
            rows = [bytes(v for x in r for v in (x & 15, x >> 4))[:w] for r in rows]
        return _png(w, h, 3, rows, plte, trns)
    if bpp == 16:
        rows = [b"".join(bytes(_c15(struct.unpack_from("<H", r, 2 * x)[0])) for x in range(w)) for r in rows]
        return _png(w, h, 6, rows)
    return _png(w, h, 2, [r[:w * 3] for r in rows])


def png_read(data):
    """Minimal PNG reader: 8-bit RGB/RGBA/grey and palette images (1/2/4/8 bit), no interlace."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    i, idat, plte, trns = 8, b"", b"", b""
    while i < len(data):
        n, t = struct.unpack_from(">I4s", data, i)
        d = data[i + 8:i + 8 + n]
        i += 12 + n
        if t == b"IHDR":
            w, h, depth, ctype, _, _, il = struct.unpack(">IIBBBBB", d)
        elif t == b"PLTE":
            plte = d
        elif t == b"tRNS":
            trns = d
        elif t == b"IDAT":
            idat += d
    if il:
        raise ValueError("interlaced PNGs are not supported; save it without interlacing")
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if depth != 8 and ctype != 3:
        raise ValueError("save the PNG with 8 bits per channel")
    bits = chans * depth
    stride, bpp = (w * bits + 7) // 8, max(1, bits // 8)
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b_, c = prev[x], prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b_) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b_) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b_ - c), abs(a - c), abs(a + b_ - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b_ if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    if ctype == 3 and depth < 8:
        per = 8 // depth
        rows = [bytes((r[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1) for x in range(w)) for r in rows]
    return {"w": w, "h": h, "ctype": ctype, "rows": rows, "plte": plte, "trns": trns}


def _to15(r, g, b, a, stp):
    if a < 128:
        return 0
    v = (r * 31 + 127) // 255 | (g * 31 + 127) // 255 << 5 | (b * 31 + 127) // 255 << 10
    return v | stp if v else 0x8000   # opaque black needs the STP bit (0x0000 is see-through)


def png2tim(tim, png):
    t, p = tim_parse(tim), png_read(png)
    if (p["w"], p["h"]) != (t["w"], t["h"]):
        raise ValueError(f"the picture must stay {t['w']} x {t['h']}")
    out = bytearray(tim)
    w, h, bpp = t["w"], t["h"], t["bpp"]
    if bpp in (4, 8):
        if p["ctype"] != 3:
            raise ValueError("this TIM uses a palette: keep the PNG in indexed (palette) mode")
        n = 16 if bpp == 4 else 256
        if any(v >= n for r in p["rows"] for v in r):
            raise ValueError(f"the picture may use only {n} palette colours")
        for y, r in enumerate(p["rows"]):
            o = t["pix"] + y * t["rowbytes"]
            if bpp == 8:
                out[o:o + w] = r
            else:
                out[o:o + (w + 1) // 2] = bytes(r[x] | (r[x + 1] << 4 if x + 1 < w else 0) for x in range(0, w, 2))
        if t["clut"]:
            co = t["clut"][0] + 12
            for k in range(min(n, len(p["plte"]) // 3)):
                old = struct.unpack_from("<H", out, co + 2 * k)[0]
                a = p["trns"][k] if k < len(p["trns"]) else 255
                struct.pack_into("<H", out, co + 2 * k, _to15(*p["plte"][3 * k:3 * k + 3], a, old & 0x8000))
        return bytes(out)
    if bpp == 16:
        if p["ctype"] not in (2, 6):
            raise ValueError("save the PNG as RGB or RGBA")
        ch = 4 if p["ctype"] == 6 else 3
        for y, r in enumerate(p["rows"]):
            for x in range(w):
                o = t["pix"] + y * t["rowbytes"] + 2 * x
                old = struct.unpack_from("<H", out, o)[0]
                px = r[x * ch:x * ch + ch]
                struct.pack_into("<H", out, o, _to15(px[0], px[1], px[2], px[3] if ch == 4 else 255, old & 0x8000))
        return bytes(out)
    raise ValueError("24-bit TIMs are not supported")


# ---- commands ---------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], usage=__doc__)
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--disc", help="your Rugrats (USA) .bin")
    a = ap.parse_args()
    c, x = a.cmd, a.args

    def need(n):
        if len(x) != n:
            sys.exit(__doc__)

    def rd(p):
        with open(p, "rb") as f:
            return f.read()

    def wr(p, d):
        with open(p, "wb") as f:
            f.write(d)
        print(f"wrote {p} ({len(d)} bytes)")

    if c == "list":
        g = Game(find_disc(a.disc))
        flt = x[0].upper() if x else ""
        print(f"{'ids':>9}  {'LBA':>7}  {'bytes':>9}  kind    path")
        for name, (lba, size, _) in sorted(g.disc.files.items(), key=lambda kv: kv[1][0]):
            if flt and flt not in name:
                continue
            ids = g.ids(name)
            kind = "stream" if g.is_stream(name) else "data"
            print(f"{','.join(map(str, ids)) or '-':>9}  {lba:7}  {size:9}  {kind:6}  {name[1:]}"
                  + ("" if ids else "   (not in the game's table: never loaded)"))
    elif c == "extract":
        need(2)
        g = Game(find_disc(a.disc))
        n = g.get(x[0])
        wr(x[1], g.data(n))
        if g.is_stream(n):
            print("streamed file: raw 2352-byte sectors; a replacement must be raw sectors too")
    elif c == "diff":
        need(3)
        g = Game(find_disc(a.disc))
        n = g.get(x[0])
        if g.is_stream(n):
            sys.exit("streamed files (movies, XA audio) can't be patched; replace them with [files]")
        wr(x[2], bps_make(g.data(n), rd(x[1])))
    elif c == "bps":
        need(3)
        wr(x[2], bps_make(rd(x[0]), rd(x[1])))
    elif c == "apply":
        need(3)
        try:
            wr(x[2], bps_apply(rd(x[0]), rd(x[1])))
        except ValueError as e:
            sys.exit(str(e))
    elif c == "tim2png":
        need(2)
        wr(x[1], tim2png(rd(x[0])))
    elif c == "png2tim":
        need(3)
        try:
            wr(x[2], png2tim(rd(x[0]), rd(x[1])))
        except ValueError as e:
            sys.exit(str(e))
    elif c == "pack":
        need(2)
        import launcher
        m = launcher.parse_mod(x[0])
        if m["error"]:
            sys.exit(f"{m['id']}: {m['error']}")
        names = []
        for r, _, fs in os.walk(x[0]):
            names += [os.path.relpath(os.path.join(r, f), x[0]) for f in fs]
        try:
            launcher._check_names(names)
        except launcher.ModError as e:
            sys.exit(str(e))
        with zipfile.ZipFile(x[1], "w", zipfile.ZIP_DEFLATED) as z:
            for n in sorted(names):
                z.write(os.path.join(x[0], n), m["id"] + "/" + n.replace(os.sep, "/"))
        print(f"wrote {x[1]}: {m['id']} ({launcher.mod_summary(m)}), {len(names)} files")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
