"""Read-only access to a PS1 MODE2/2352 disc image (ISO9660 + XA)."""
import hashlib
import struct

SECTOR = 2352
EXPECTED_SHA1 = "b547ef42310a5b04ccfd62d12aa9c41af1c5d6f5"  # Rugrats: Search for Reptar (USA) SLUS-00650


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.files = {}  # upper-case path without ;1 -> (lba, size, form2)
        pvd = self.user(16)
        if pvd[1:6] != b"CD001":
            raise ValueError("not an ISO9660 MODE2/2352 image")
        root = pvd[156:190]
        self._walk(struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0], "")

    def raw(self, lba):
        self.f.seek(lba * SECTOR)
        return self.f.read(SECTOR)

    def user(self, lba):
        return self.raw(lba)[24:24 + 2048]

    def read_extent(self, lba, size):
        out = bytearray()
        for i in range((size + 2047) // 2048):
            out += self.user(lba + i)
        return bytes(out[:size])

    def _walk(self, lba, size, prefix):
        data = self.read_extent(lba, size)
        i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + n]
            i += n
            elba = struct.unpack("<I", rec[2:6])[0]
            esz = struct.unpack("<I", rec[10:14])[0]
            name = rec[33:33 + rec[32]]
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii").split(";")[0]
            full = prefix + "/" + name
            if rec[25] & 2:
                self._walk(elba, esz, full)
            else:
                form2 = bool(self.raw(elba)[18] & 0x20) if esz else False
                self.files[full.upper()] = (elba, esz, form2)

    def read_file(self, name):
        lba, size, form2 = self.files[name.upper()]
        if form2:
            raise ValueError("form2 file; read raw sectors instead")
        return self.read_extent(lba, size)

    def boot_exe_name(self):
        cnf = self.read_file("/SYSTEM.CNF").decode("ascii", "replace")
        for line in cnf.splitlines():
            if line.upper().startswith("BOOT"):
                return "/" + line.split("\\")[-1].split(";")[0].strip().upper()
        raise ValueError("no BOOT line")
