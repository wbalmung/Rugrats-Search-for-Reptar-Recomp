import sys, struct, zlib, glob, os
# raw2png.py dir : converts sNNN_WxH_16.raw (15-bit BGR555 little endian) to PNG next to it
def png(p, w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)
    def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))
for f in sorted(glob.glob(os.path.join(sys.argv[1], 's*_16.raw'))):
    w, h = map(int, os.path.basename(f).split('_')[1].split('x'))
    d = open(f, 'rb').read()
    rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            v = d[(y * w + x) * 2] | d[(y * w + x) * 2 + 1] << 8
            r += bytes(((v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31))
        rows.append(bytes(r))
    png(f[:-4] + '.png', w, h, rows)
