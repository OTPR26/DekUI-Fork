"""Read selected files from a user's GameCube image without copying the disc."""

import ctypes as c, struct
from .binary import require

U = lambda d, o: struct.unpack_from(">I", d, o)[0]


class Disc:
    def __init__(self, path, library):
        self.lib = c.CDLL(str(library))
        self.handle = c.c_void_p()
        self.lib.nod_disc_open.argtypes = [
            c.c_char_p,
            c.c_void_p,
            c.POINTER(c.c_void_p),
        ]
        self.lib.nod_disc_open.restype = c.c_int
        self.lib.nod_read.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t]
        self.lib.nod_read.restype = c.c_int64
        self.lib.nod_seek.argtypes = [c.c_void_p, c.c_int64, c.c_int32]
        self.lib.nod_seek.restype = c.c_int64
        self.lib.nod_free.argtypes = [c.c_void_p]
        require(
            self.lib.nod_disc_open(str(path).encode(), None, c.byref(self.handle)) == 0,
            "Cannot open the selected disc image",
        )
        try:
            header = self.read(0, 0x440)
            require(
                header[0x1C:0x20] == bytes.fromhex("c2339f3d") and header[:3] == b"GZ2",
                "Select a GameCube Twilight Princess ROM (ISO or RVZ)",
            )
            require(
                header[3:4] in (b"E", b"P", b"J"),
                "Unsupported Twilight Princess region",
            )
            fst = self.read(U(header, 0x424), U(header, 0x428))
            count = U(fst, 8)
            require(
                1 <= count <= 100000 and count * 12 <= len(fst),
                "Invalid disc directory",
            )
            self.files = {}
            stack = []
            for i in range(1, count):
                while stack and i >= stack[-1][1]:
                    stack.pop()
                flag = U(fst, i * 12)
                at = count * 12 + (flag & 0xFFFFFF)
                require(at < len(fst))
                end = fst.find(b"\0", at)
                require(end >= at)
                name = fst[at:end].decode()
                require(
                    name not in (".", "..") and "/" not in name and "\\" not in name,
                    "Unsafe disc filename",
                )
                path = "/".join([x[0] for x in stack] + [name])
                if flag >> 24:
                    stack.append((name, U(fst, i * 12 + 8)))
                else:
                    self.files[path] = (U(fst, i * 12 + 4), U(fst, i * 12 + 8))
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.handle:
            self.lib.nod_free(self.handle)
            self.handle = c.c_void_p()

    def read(self, at, n):
        require(at >= 0 and 0 <= n <= 64 * 1024 * 1024, "Invalid disc read")
        require(self.lib.nod_seek(self.handle, at, 0) == at, "Cannot seek in disc")
        b = c.create_string_buffer(n)
        done = 0
        while done < n:
            got = self.lib.nod_read(self.handle, c.byref(b, done), n - done)
            require(got > 0, "Incomplete disc read")
            done += got
        return b.raw

    def file(self, name):
        require(name in self.files, "Required TP file is missing: " + name)
        return self.read(*self.files[name])


def yaz(data):
    if data[:4] != b"Yaz0":
        return data
    size = U(data, 4)
    require(0 < size <= 64 * 1024 * 1024, "Archive is too large")
    out = bytearray()
    at = 16
    while len(out) < size:
        flags = data[at]
        at += 1
        for bit in range(8):
            if len(out) == size:
                break
            if flags & (128 >> bit):
                out.append(data[at])
                at += 1
            else:
                a, b = data[at : at + 2]
                at += 2
                distance = ((a & 15) << 8 | b) + 1
                length = a >> 4
                if not length:
                    length = data[at] + 18
                    at += 1
                else:
                    length += 2
                require(
                    distance <= len(out) and len(out) + length <= size,
                    "Invalid archive back reference",
                )
                for _ in range(length):
                    out.append(out[-distance])
    return bytes(out)


def archive(data):
    data = yaz(data)
    require(data[:4] == b"RARC", "Unsupported TP archive")
    H = lambda o: struct.unpack_from(">H", data, o)[0]
    nodes = U(data, 0x24) + 32
    files = U(data, 0x2C) + 32
    strings = U(data, 0x34) + 32
    base = U(data, 12) + 32
    result = {}
    seen = set()

    def walk(ni, path=""):
        require(ni not in seen and len(seen) < 10000, "Invalid archive directory")
        seen.add(ni)
        no = nodes + ni * 16
        for i in range(U(data, no + 12), U(data, no + 12) + H(no + 10)):
            o = files + i * 20
            at = strings + H(o + 6)
            end = data.find(b"\0", at)
            require(end >= at)
            name = data[at:end].decode()
            if name in (".", ".."):
                continue
            require("/" not in name and "\\" not in name, "Unsafe archive filename")
            p = path + name
            if H(o + 4) & 0x200:
                walk(U(data, o + 8), p + "/")
            else:
                off = base + U(data, o + 8)
                size = U(data, o + 12)
                require(off + size <= len(data))
                result[p] = yaz(data[off : off + size])

    walk(0)
    return result
