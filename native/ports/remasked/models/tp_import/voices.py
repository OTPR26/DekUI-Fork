import struct, hashlib
from .binary import u, put, align, require
from .sound_archive import Sar, keys, waves, pack_waves


def metadata(b):
    U = lambda d, o: struct.unpack_from(">I", d, o)[0]
    F = lambda d, o: struct.unpack_from(">f", d, o)[0]
    require(b[:4] == b"AA_<", "Invalid TP sound archive")
    sizes = {
        b"ws  ": 3,
        b"bnk ": 2,
        b"bl_<": 2,
        b">_bl": 0,
        b"bsc ": 2,
        b"bst ": 2,
        b"bstn": 2,
        b"bms ": 3,
        b"bmsa": 1,
        b"vbnk": 2,
        b"dsqb": 1,
        b"bsft": 1,
        b"sect": 1,
        b"bfca": 1,
    }
    idx = []
    at = 4
    while True:
        cmd = b[at : at + 4]
        at += 4
        if cmd == b">_AA":
            break
        require(cmd in sizes, "Unsupported TP sound command")
        n = sizes[cmd]
        args = [U(b, at + i * 4) for i in range(n)]
        at += n * 4
        idx.append((cmd.decode(), args))
    banks = {
        U(b, a[1] + 8): b[a[1] : a[1] + U(b, a[1] + 4)]
        for cmd, a in idx
        if cmd == "bnk "
    }
    d = banks[0x33]
    at = 32
    while d[at : at + 4] != b"LIST":
        at += 8 + U(d, at + 4)
    prog = {}
    for pr in (0, 5, 7, 8):
        p = U(d, at + 12 + pr * 4)
        assert d[p : p + 4] == b"Perc"
        notes = {}
        for n in range(U(d, p + 4)):
            off = U(d, p + 8 + n * 4)
            if not off:
                continue
            count = U(d, off + 16)
            r = off + 20 + count * 4
            wave = U(d, r + 8) & 65535
            notes[n] = {
                "wave": wave,
                "pitch": F(d, off + 8) * F(d, r + 16),
                "volume": F(d, off + 4) * F(d, r + 12),
            }
        prog[pr] = notes
    ws = next(
        b[a[1] : a[1] + U(b, a[1] + 4)] for cmd, a in idx if cmd == "ws  " and a[0] == 0
    )
    win = U(ws, 16)
    ctrl = U(ws, 20)
    waves = {}
    for g in range(U(ws, win + 4)):
        ar = U(ws, win + 8 + 4 * g)
        name = ws[ar : ar + 112].split(b"\0")[0].decode()
        sc = U(ws, ctrl + 12 + g * 4)
        ct = U(ws, sc + 12)
        for j in range(U(ws, ar + 112)):
            p = U(ws, ar + 116 + j * 4)
            id = U(ws, U(ws, ct + 8 + j * 4)) & 65535
            waves[id] = {
                "file": name,
                "format": ws[p + 1],
                "key": ws[p + 2],
                "rate": F(ws, p + 4),
                "offset": U(ws, p + 8),
                "size": U(ws, p + 12),
                "samples": U(ws, p + 28),
                "hist1": struct.unpack_from(">h", ws, p + 32)[0],
                "hist2": struct.unpack_from(">h", ws, p + 34)[0],
            }
    return {str(k): v for k, v in waves.items()}, {
        str(k): {str(n): v for n, v in ns.items()} for k, ns in prog.items()
    }


def convert(mm, baa, read_file):
    W, P = metadata(baa)
    CO0 = [
        0,
        2048,
        0,
        1024,
        4096,
        3584,
        3072,
        4608,
        4200,
        4800,
        5120,
        2048,
        1024,
        -1024,
        -1024,
        -2048,
    ]
    CO1 = [
        0,
        0,
        2048,
        1024,
        -2048,
        -1536,
        -1024,
        -2560,
        -2248,
        -2300,
        -3072,
        -2048,
        -1024,
        1024,
        0,
        0,
    ]
    cache = {}
    samples = {}
    report = []

    def clip(program, note):
        meta = P[str(program)][str(note)]
        wid = meta["wave"]
        t = W[str(wid)]
        if wid in samples:
            return samples[wid]
        file = t["file"]
        if file not in cache:
            cache[file] = read_file("Audiores/Waves/" + file)
        raw = cache[file][t["offset"] : t["offset"] + t["size"]]
        assert len(raw) == t["size"]
        assert t["format"] == 0 and len(raw) % 9 == 0
        hist1 = t["hist1"]
        hist2 = t["hist2"]
        pcm = []
        for at in range(0, len(raw), 9):
            h = raw[at]
            scale = 1 << (h >> 4)
            c = h & 15
            for i in range(16):
                n = (raw[at + 1 + i // 2] >> (4 if i % 2 == 0 else 0)) & 15
                n = n - 16 if n >= 8 else n
                value = max(
                    -32768,
                    min(
                        32767,
                        ((n * scale << 11) + CO0[c] * hist1 + CO1[c] * hist2) >> 11,
                    ),
                )
                pcm.append(value)
                hist2, hist1 = hist1, value
        pcm = pcm[: t["samples"]]
        assert len(pcm) == t["samples"]
        rate = round(t["rate"])
        payload = struct.pack("<" + "h" * len(pcm), *pcm)
        # Retain the known mono CWAV structure while switching its codec to PCM16.
        out = bytearray(0xE0)
        out[:4] = b"CWAV"
        put(out, 4, 0xFEFF, 2)
        put(out, 6, 64, 2)
        put(out, 8, 0x02010000)
        put(out, 16, 2, 2)
        for off, value in [
            (20, 0x7000),
            (24, 0x40),
            (28, 0x80),
            (32, 0x7001),
            (36, 0xC0),
        ]:
            put(out, off, value)
        out[0x40:0x44] = b"INFO"
        put(out, 0x44, 0x80)
        out[0x48] = 1
        put(out, 0x4C, rate)
        put(out, 0x54, len(pcm))
        put(out, 0x5C, 1)
        put(out, 0x60, 0x7100)
        put(out, 0x64, 12)
        put(out, 0x68, 0x1F00)
        put(out, 0x6C, 24)
        put(out, 0x74, 0xFFFFFFFF)
        out[0xC0:0xC4] = b"DATA"
        out.extend(payload)
        align(out, 32)
        put(out, 12, len(out))
        put(out, 40, len(out) - 0xC0)
        put(out, 0xC4, len(out) - 0xC0)
        samples[wid] = bytes(out)
        report.append(
            {
                "program": program,
                "note": note,
                "wave": wid,
                "rate": rate,
                "samples": len(pcm),
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        )
        return samples[wid]

    # Map the existing human action slots; keep unsupported and transformation voices intact.
    actions = {
        0: (0, 0),
        1: (0, 1),
        2: (0, 2),
        3: (0, 3),
        4: (0, 7),
        5: (0, 8),
        6: (0, 57),
        7: (0, 64),
        8: (0, 65),
        9: (0, 29),
        10: (0, 30),
        11: (0, 31),
        12: (5, 8),
        13: (5, 9),
        14: (5, 8),
        15: (0, 36),
        16: (0, 37),
        17: (0, 61),
        18: (0, 62),
        19: (0, 83),
        21: (0, 4),
        23: (0, 87),
        24: (0, 36),
        25: (0, 29),
        55: (0, 101),
        56: (0, 102),
    }
    mapping = {(0, n + 28): actions[n] for n in range(19)}
    for to, fr in [
        (47, 19),
        (48, 23),
        (49, 24),
        (50, 25),
        (51, 55),
        (52, 56),
        (105, 21),
    ]:
        mapping[0, to] = actions[fr]
    # Also replace the native adult keys used by human-only scripted actions.
    for n in range(26):
        if n in actions:
            mapping[0, n] = actions[n]
    dst = Sar(mm)
    bank_id = dst.voice_bank()
    original_bank = dst.payload(bank_id)
    bank = bytearray(original_bank)
    target = keys(original_bank, 0)
    archive = target[0][2]
    archive_id = dst.wave_file(archive)
    old_waves = waves(dst.payload(archive_id))
    new_waves = old_waves[:]
    body = u(bank, 24) + 8
    old_table = body + u(bank, body + 4)
    count = u(bank, old_table)
    table = bytearray(bank[old_table : old_table + 4 + 8 * count])
    imported = {}
    bindings = []
    for (pr, note), (tppr, tpnote) in mapping.items():
        if note not in target:
            continue
        sample = clip(tppr, tpnote)
        sha = hashlib.sha256(sample).hexdigest()
        if sha not in imported:
            imported[sha] = len(new_waves)
            new_waves.append(sample)
        table.extend(struct.pack("<II", 0x05000000 | archive, imported[sha]))
        put(bank, target[note][0], count)
        count += 1
        bindings.append((pr, note, tppr, tpnote))
    put(table, 0, count)
    align(bank, 4)
    at = len(bank)
    bank.extend(table)
    put(bank, body + 4, at - body)
    put(bank, 12, len(bank))
    put(bank, u(bank, 24) + 4, len(bank) - u(bank, 24))
    replacements = {
        bank_id: bytes(bank),
        archive_id: pack_waves(dst.payload(archive_id), new_waves),
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
        require(i not in replacements or root == i)
        put(out, ref, positions[root] + off - dst.files[root][0] - dst.file - 8)
        put(out, ref + 4, len(replacements[i]) if i in replacements else size)
    put(out, 12, len(out))
    put(out, 52, len(out) - dst.file)
    put(out, dst.file + 4, len(out) - dst.file)
    verified = Sar(out)
    for i, entry in enumerate(dst.files):
        if entry and i not in replacements:
            require(verified.payload(i) == dst.payload(i), "Unrelated sound changed")
    check = verified.payload(bank_id)
    for pr in range(4):
        before = keys(original_bank, pr)
        after = keys(check, pr)
        for note, key in before.items():
            if (pr, note) not in mapping:
                require(
                    original_bank[key[0] : key[0] + 40]
                    == check[after[note][0] : after[note][0] + 40],
                    "Unrelated voice key changed",
                )
    require(waves(verified.payload(archive_id))[: len(old_waves)] == old_waves)
    return bytes(out), len(imported), len(bindings)
