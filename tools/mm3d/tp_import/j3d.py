from pathlib import Path
import struct, math, numpy as np

U = lambda d, o: struct.unpack_from(">I", d, o)[0]
H = lambda d, o: struct.unpack_from(">H", d, o)[0]
S = lambda d, o: struct.unpack_from(">h", d, o)[0]
F = lambda d, o: struct.unpack_from(">f", d, o)[0]


def strings(d, o):
    return [
        d[o + H(d, o + 6 + 4 * i) :].split(b"\0")[0].decode() for i in range(H(d, o))
    ]


def transform(scale, rot, pos):
    x, y, z = np.array(rot) * math.pi / 32768
    cx, sx = math.cos(x), math.sin(x)
    cy, sy = math.cos(y), math.sin(y)
    cz, sz = math.cos(z), math.sin(z)
    m = np.eye(4)
    m[:3, :3] = (
        np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
        @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
        @ np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
        @ np.diag(scale)
    )
    m[:3, 3] = pos
    return m


class J3D:
    def __init__(self, path):
        self.path = Path(path)
        b = self.path.read_bytes()
        assert b[:8] == b"J3D2bmd3"
        self.chunks = {}
        at = 32
        for i in range(U(b, 12)):
            n = U(b, at + 4)
            assert n >= 8 and at + n <= len(b)
            self.chunks[b[at : at + 4].decode()] = b[at : at + n]
            at += n
        j = self.chunks["JNT1"]
        self.names = strings(j, U(j, 20))
        self.parents = [-1] * len(self.names)
        self.world = []
        inf = self.chunks["INF1"]
        at = U(inf, 20)
        stack = []
        last = -1
        self.shape_material = {}
        material = 0
        while True:
            typ, idx = struct.unpack_from(">HH", inf, at)
            at += 4
            if typ == 0:
                break
            if typ == 1:
                stack.append(last)
            elif typ == 2:
                stack.pop()
            elif typ == 0x10:
                self.parents[idx] = stack[-1] if stack else -1
                last = idx
            elif typ == 0x11:
                material = idx
            elif typ == 0x12:
                self.shape_material[idx] = material
        for i, p in enumerate(self.parents):
            o = U(j, 12) + H(j, U(j, 16) + i * 2) * 64
            m = transform(
                struct.unpack_from(">3f", j, o + 4),
                struct.unpack_from(">3h", j, o + 16),
                struct.unpack_from(">3f", j, o + 24),
            )
            if p >= 0:
                assert p < i
                m = self.world[p] @ m
            self.world.append(m)
        e = self.chunks["EVP1"]
        self.envelopes = []
        at = 0
        maxbone = -1
        for i in range(H(e, 8)):
            n = e[U(e, 12) + i]
            weights = []
            for k in range(n):
                bone = H(e, U(e, 16) + at * 2)
                weight = F(e, U(e, 20) + at * 4)
                at += 1
                weights.append((bone, weight))
                maxbone = max(maxbone, bone)
            self.envelopes.append(weights)
        self.invbind = []
        for i in range(maxbone + 1):
            m = np.eye(4)
            m[:3, :] = np.array(
                struct.unpack_from(">12f", e, U(e, 24) + i * 48)
            ).reshape(3, 4)
            self.invbind.append(m)
        dr = self.chunks["DRW1"]
        self.draw = [
            (dr[U(dr, 12) + i], H(dr, U(dr, 16) + i * 2)) for i in range(H(dr, 8))
        ]
        v = self.chunks["VTX1"]
        self.formats = {}
        self.arrays = {}
        at = U(v, 8)
        while U(v, at) != 255:
            attr, cnt, typ = struct.unpack_from(">III", v, at)
            self.formats[attr] = (cnt, typ, v[at + 12])
            at += 16
        for i, attr in enumerate([9, 10, 25, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]):
            off = U(v, 12 + i * 4)
            if off:
                self.arrays[attr] = (v, off)
        mat = self.chunks["MAT3"]
        self.matnames = strings(mat, U(mat, 20))
        self.materials = []
        for i in range(H(mat, 8)):
            at = U(mat, 12) + H(mat, U(mat, 16) + i * 2) * 0x14C
            tex = []
            for k in range(8):
                idx = H(mat, at + 0x84 + k * 2)
                if idx != 65535:
                    tex.append(H(mat, U(mat, 0x48) + idx * 2))
            self.materials.append(
                {"name": self.matnames[i], "textures": tex, "translucent": mat[at] == 4}
            )
        tex = self.chunks["TEX1"]
        self.texnames = strings(tex, U(tex, 16))
        self.textures = []
        for i, name in enumerate(self.texnames):
            at = U(tex, 12) + i * 32
            self.textures.append(
                {
                    "name": name,
                    "format": tex[at],
                    "width": H(tex, at + 2),
                    "height": H(tex, at + 4),
                    "data": tex[at + U(tex, at + 28) :],
                }
            )
        self.geometry = []
        sh = self.chunks["SHP1"]
        self.shape_count = H(sh, 8)
        for i in range(self.shape_count):
            self.geometry.append(self.shape(i))

    def value(self, attr, idx):
        cnt, typ, shift = self.formats[attr]
        dims = (
            (2 if cnt == 0 else 3)
            if attr == 9
            else 3 if attr in (10, 25) else (1 if cnt == 0 else 2)
        )
        assert attr not in (11, 12, 25), (attr, "unsupported vertex payload")
        fmt, size = {0: ("B", 1), 1: ("b", 1), 2: ("H", 2), 3: ("h", 2), 4: ("f", 4)}[
            typ
        ]
        data, at = self.arrays[attr]
        return np.array(
            struct.unpack_from(">" + fmt * dims, data, at + idx * size * dims),
            dtype=float,
        ) / (2**shift if typ != 4 else 1)

    def shape(self, i):
        d = self.chunks["SHP1"]
        at = U(d, 12) + i * 40
        mode = d[at]
        groups = H(d, at + 2)
        decl = U(d, 24) + H(d, at + 4)
        vcd = []
        while U(d, decl) != 255:
            vcd.append((U(d, decl), U(d, decl + 4)))
            decl += 8
        vertices = []
        triangles = []
        previous = []
        for gi in range(groups):
            m = U(d, 36) + (H(d, at + 6) + gi) * 8
            count = H(d, m + 2)
            start = U(d, m + 4)
            palette = []
            for k in range(count):
                idx = H(d, U(d, 28) + (start + k) * 2)
                if idx == 65535:
                    assert k < len(previous)
                    idx = previous[k]
                palette.append(idx)
            previous = palette[:]
            p = U(d, 40) + (H(d, at + 8) + gi) * 8
            size = U(d, p)
            o = U(d, 32) + U(d, p + 4)
            end = o + size
            while o < end:
                op = d[o]
                o += 1
                if op == 0:
                    continue
                assert op & 0xF8 in (0x80, 0x90, 0x98, 0xA0), (self.path, i, hex(op))
                n = H(d, o)
                o += 2
                ids = []
                for _ in range(n):
                    vals = {}
                    for attr, kind in vcd:
                        if kind == 1:
                            assert attr <= 8
                            val = d[o]
                            o += 1
                        elif kind == 2:
                            val = d[o]
                            o += 1
                        elif kind == 3:
                            val = H(d, o)
                            o += 2
                        else:
                            raise ValueError((attr, kind))
                        vals[attr] = val
                    slot = vals.get(0, 0) // 3 if mode == 3 else 0
                    assert slot < len(palette), (slot, palette)
                    draw = palette[slot]
                    kind, param = self.draw[draw]
                    weights = self.envelopes[param] if kind else [(param, 1.0)]
                    pos = self.value(9, vals[9])
                    normal = self.value(10, vals[10])
                    uv = self.value(13, vals[13]) if 13 in vals else np.zeros(2)
                    ids.append(len(vertices))
                    vertices.append(
                        {
                            "pos": pos,
                            "normal": normal,
                            "uv": uv,
                            "weights": weights,
                            "smooth": bool(kind),
                        }
                    )
                op &= 0xF8
                if op == 0x90:
                    assert n % 3 == 0
                    triangles.extend(tuple(ids[j : j + 3]) for j in range(0, n, 3))
                elif op == 0x80:
                    assert n % 4 == 0
                    for j in range(0, n, 4):
                        triangles.extend(
                            [
                                (ids[j], ids[j + 1], ids[j + 2]),
                                (ids[j], ids[j + 2], ids[j + 3]),
                            ]
                        )
                elif op == 0x98:
                    for j in range(2, n):
                        triangles.append(
                            (ids[j - 2], ids[j - 1], ids[j])
                            if j % 2 == 0
                            else (ids[j - 1], ids[j - 2], ids[j])
                        )
                else:
                    for j in range(2, n):
                        triangles.append((ids[0], ids[j - 1], ids[j]))
            assert o == end
        return {
            "vertices": vertices,
            "triangles": triangles,
            "material": self.shape_material[i],
            "declarations": vcd,
        }


if __name__ == "__main__":
    import sys

    for p in sys.argv[1:]:
        m = J3D(p)
        print(
            m.path.name,
            len(m.names),
            "bones",
            len(m.geometry),
            "shapes",
            sum(len(x["triangles"]) for x in m.geometry),
            "triangles",
        )
        print(m.materials)
