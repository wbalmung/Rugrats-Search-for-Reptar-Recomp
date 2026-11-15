import sys, struct, zlib, glob, os
# sheet.py out.png kind pattern cols : kind emu (raw 16-bit 512x240) or bmp (32-bit); thumbs 1/4 size, with frame grid
def load(p, kind):
    if kind == 'emu':
        d = open(p, 'rb').read(); w, h = 512, 240
        return w, h, lambda x, y: (lambda v: ((v & 31) * 8, ((v >> 5) & 31) * 8, ((v >> 10) & 31) * 8))(d[(y*w+x)*2] | d[(y*w+x)*2+1] << 8)
    d = open(p, 'rb').read(); off = struct.unpack_from('<I', d, 10)[0]; w, h = struct.unpack_from('<ii', d, 18); h = abs(h)
    return w, h, lambda x, y: (d[off+(y*w+x)*4+2], d[off+(y*w+x)*4+1], d[off+(y*w+x)*4])
out, kind, pat, cols = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
files = sorted(glob.glob(pat))
K = 4; tw, th = 128, 60
rows_n = (len(files) + cols - 1) // cols
W, H = cols * (tw + 2), rows_n * (th + 2)
img = [bytearray(W * 3) for _ in range(H)]
for i, p in enumerate(files):
    w, h, px = load(p, kind)
    if w != 512: continue
    ox, oy = (i % cols) * (tw + 2), (i // cols) * (th + 2)
    for y in range(th):
        for x in range(tw):
            r, g, b = px(x * K, y * K)
            j = (ox + x) * 3; img[oy + y][j:j+3] = bytes((r, g, b))
raw = b''.join(b'\0' + bytes(r) for r in img)
def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
print(out, len(files))
