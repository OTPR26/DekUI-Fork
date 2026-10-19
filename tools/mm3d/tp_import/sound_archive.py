"""Import the same 42 human voice bindings as Android; leave other sound files intact."""

from .binary import *


class Sar:
    def __init__(self, d):
        require(
            len(d) <= 64 * 1024 * 1024
            and d[:4] == b"CSAR"
            and h(d, 4) == 0xFEFF
            and u(d, 12) == len(d)
            and h(d, 16) == 3
        )
        self.d = d
        blocks = {h(d, 20 + i * 12): u(d, 24 + i * 12) for i in range(3)}
        st = blocks[0x2000]
        info = blocks[0x2001]
        self.file = blocks[0x2002]
        require(
            d[st : st + 4] == b"STRG"
            and d[info : info + 4] == b"INFO"
            and d[self.file : self.file + 4] == b"FILE"
        )
        nt = st + 8 + u(d, st + 12)
        count = u(d, nt)
        require(0 < count <= 100000)
        self.names = [
            take(d, nt + u(d, nt + 8 + i * 12), u(d, nt + 12 + i * 12) - 1).decode()
            for i in range(count)
        ]

        def table(j):
            at = info + 8 + u(d, info + 12 + j * 8)
            n = u(d, at)
            require(n <= 100000)
            return [at + u(d, at + 8 + i * 8) for i in range(n)]

        self.banks = table(2)
        self.archives = table(3)
        self.files = []
        for at in table(6):
            kind = h(d, at)
            require(kind in (0x220C, 0x220D))
            if kind == 0x220D:
                self.files.append(None)
                continue
            ref = at + 4 + u(d, at + 4)
            off = self.file + 8 + u(d, ref)
            size = u(d, ref + 4)
            take(d, off, size)
            require(ref + 8 <= self.file)
            self.files.append((off, size, ref))

    def payload(self, i):
        off, size, _ = self.files[i]
        return take(self.d, off, size)

    def voice_bank(self):
        matches = [
            u(self.d, at)
            for at in self.banks
            if self.names[u(self.d, at + 16)] == "BANK_VOICE"
        ]
        require(len(matches) == 1)
        return matches[0]

    def wave_file(self, i):
        return u(self.d, self.archives[i])


def keys(d, program):
    require(d[:4] == b"CBNK")
    body = u(d, 24) + 8
    instruments = body + u(d, body + 12)
    require(program < u(d, instruments))
    ins = instruments + u(d, instruments + 8 + program * 8)
    require(h(d, ins) == 0x6002)
    table = ins + u(d, ins + 4)
    start, end = take(d, table, 2)
    require(start <= end <= 127)
    waves = body + u(d, body + 4)
    count = u(d, waves)
    require(0 < count <= 10000)
    result = {}
    for note in range(start, end + 1):
        ref = table + 4 + (note - start) * 8
        offset = u(d, ref + 4)
        if offset == 0xFFFFFFFF:
            continue
        key = table + offset
        require(h(d, key) == 0x6000)
        detail = key + u(d, key + 4)
        require(h(d, detail) == 0x5902)
        vel = detail + u(d, detail + 4)
        index = u(d, vel)
        require(index < count)
        wave = waves + 4 + index * 8
        result[note] = (vel, index, u(d, wave) & 0xFFFFFF, u(d, wave + 4))
    return result


def waves(d):
    require(d[:4] == b"CWAR" and u(d, 12) == len(d))
    info = u(d, 24) + 8
    file = u(d, 36) + 8
    n = u(d, info)
    require(0 < n <= 10000)
    result = []
    for i in range(n):
        ref = info + 4 + 12 * i
        require(h(d, ref) == 0x1F00)
        result.append(take(d, file + u(d, ref + 4), u(d, ref + 8)))
    return result


def pack_waves(original, files):
    out = bytearray(original[:64])
    info = len(out)
    out.extend(b"INFO" + bytes(8 + 12 * len(files)))
    align(out, 32)
    file = len(out)
    out.extend(b"FILE" + bytes(4))
    align(out, 32)
    written = {}
    for i, data in enumerate(files):
        if data not in written:
            written[data] = len(out)
            out.extend(data)
            align(out, 32)
        put(out, info + 12 + 12 * i, 0x1F00)
        put(out, info + 16 + 12 * i, written[data] - file - 8)
        put(out, info + 20 + 12 * i, len(data))
    put(out, info + 8, len(files))
    for at, val in (
        (12, len(out)),
        (24, info),
        (28, file - info),
        (36, file),
        (40, len(out) - file),
        (info + 4, file - info),
        (file + 4, len(out) - file),
    ):
        put(out, at, val)
    return bytes(out)


def convert(oot, mm):
    src = Sar(oot)
    dst = Sar(mm)
    source_keys = keys(src.payload(src.voice_bank()), 0)
    bank_id = dst.voice_bank()
    original_bank = dst.payload(bank_id)
    bank = bytearray(original_bank)
    target_keys = {p: keys(original_bank, p) for p in (0, 3)}
    archive = target_keys[0][0][2]
    archive_id = dst.wave_file(archive)
    old_archive = dst.payload(archive_id)
    old_waves = waves(old_archive)
    new_waves = old_waves[:]
    mapping = {(0, n): n - 28 for n in range(28, 47)}
    for to, fr in (
        (47, 19),
        (48, 23),
        (49, 24),
        (50, 25),
        (51, 55),
        (52, 56),
        (53, 26),
        (54, 27),
        (64, 77),
        (67, 80),
        (69, 81),
        (70, 82),
        (71, 83),
        (72, 84),
        (73, 85),
        (74, 86),
        (75, 87),
        (76, 88),
        (105, 21),
    ):
        mapping[0, to] = fr
    for to, fr in ((72, 86), (73, 87), (74, 88), (75, 86)):
        mapping[3, to] = fr
    body = u(bank, 24) + 8
    old_table = body + u(bank, body + 4)
    count = u(bank, old_table)
    table = bytearray(take(bank, old_table, 4 + 8 * count))
    imported = {}
    archives = {}
    for dest, note in mapping.items():
        old = target_keys[dest[0]][dest[1]]
        adult = source_keys[note]
        require(old[2] == archive)
        if adult[2] not in archives:
            archives[adult[2]] = waves(src.payload(src.wave_file(adult[2])))
        sample = archives[adult[2]][adult[3]]
        require(sample[:4] == b"CWAV" and u(sample, 12) == len(sample))
        key = adult[2:4]
        if key not in imported:
            imported[key] = len(new_waves)
            new_waves.append(sample)
        table.extend(struct.pack("<II", 0x05000000 | archive, imported[key]))
        put(bank, old[0], count)
        count += 1
    put(table, 0, count)
    align(bank, 4)
    at = len(bank)
    bank.extend(table)
    put(bank, body + 4, at - body)
    put(bank, 12, len(bank))
    put(bank, u(bank, 24) + 4, len(bank) - u(bank, 24))
    replacements = {
        bank_id: bytes(bank),
        archive_id: pack_waves(old_archive, new_waves),
    }
    roots = {}
    for i, entry in enumerate(dst.files):
        if entry is None:
            continue
        off, size, _ = entry
        containers = [
            (j, other)
            for j, other in enumerate(dst.files)
            if other
            and j != i
            and other[0] <= off
            and off + size <= other[0] + other[1]
            and (other[1] > size or j < i)
        ]
        roots[i] = max(containers, key=lambda x: x[1][1])[0] if containers else i
    out = bytearray(mm[: dst.file + 8])
    align(out, 32)
    positions = {}
    for i in sorted(set(roots.values()), key=lambda i: dst.files[i][0]):
        positions[i] = len(out)
        out.extend(replacements.get(i, dst.payload(i)))
        align(out, 32)
    for i, entry in enumerate(dst.files):
        if entry is None:
            continue
        off, size, ref = entry
        root = roots[i]
        if i in replacements:
            require(root == i)
        put(out, ref, positions[root] + off - dst.files[root][0] - dst.file - 8)
        put(out, ref + 4, len(replacements[i]) if i in replacements else size)
    put(out, 12, len(out))
    put(out, 52, len(out) - dst.file)
    put(out, dst.file + 4, len(out) - dst.file)
    verified = Sar(out)
    for i, entry in enumerate(dst.files):
        if entry and i not in replacements:
            require(
                verified.payload(i) == dst.payload(i), "Unrelated sound file changed"
            )
    check_bank = verified.payload(bank_id)
    for program in range(4):
        before = keys(original_bank, program)
        after = keys(check_bank, program)
        for note, key in before.items():
            if (program, note) not in mapping:
                require(
                    take(original_bank, key[0], 40)
                    == take(check_bank, after[note][0], 40),
                    "Unrelated voice key changed",
                )
    require(waves(verified.payload(archive_id))[: len(old_waves)] == old_waves)
    return bytes(out), len(imported), len(mapping)
