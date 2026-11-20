import numpy as np, struct
from .j3d import H


def rgb565(n):
    return (
        (n >> 11) * 255 // 31,
        ((n >> 5) & 63) * 255 // 63,
        (n & 31) * 255 // 31,
        255,
    )


def decode(t):
    w, h, fmt = t["width"], t["height"], t["format"]
    d = t["data"]
    out = np.zeros((h, w, 4), dtype=np.uint8)
    at = 0
    if fmt == 14:
        for yy in range(0, h, 8):
            for xx in range(0, w, 8):
                for oy, ox in [(0, 0), (0, 4), (4, 0), (4, 4)]:
                    a, b = H(d, at), H(d, at + 2)
                    colors = [rgb565(a), rgb565(b)]
                    if a > b:
                        colors += [
                            tuple(
                                (2 * colors[0][i] + colors[1][i]) // 3 for i in range(4)
                            ),
                            tuple(
                                (colors[0][i] + 2 * colors[1][i]) // 3 for i in range(4)
                            ),
                        ]
                    else:
                        colors += [
                            tuple((colors[0][i] + colors[1][i]) // 2 for i in range(4)),
                            (0, 0, 0, 0),
                        ]
                    for y in range(4):
                        for x in range(4):
                            if yy + oy + y < h and xx + ox + x < w:
                                out[yy + oy + y, xx + ox + x] = colors[
                                    (d[at + 4 + y] >> (6 - 2 * x)) & 3
                                ]
                    at += 8
    elif fmt in (0, 1, 5):
        bw, bh = (8, 8) if fmt == 0 else (8, 4) if fmt == 1 else (4, 4)
        for yy in range(0, h, bh):
            for xx in range(0, w, bw):
                for y in range(bh):
                    for x in range(bw):
                        if fmt == 5:
                            n = H(d, at)
                            at += 2
                            if n & 0x8000:
                                c = (
                                    ((n >> 10) & 31) * 255 // 31,
                                    ((n >> 5) & 31) * 255 // 31,
                                    (n & 31) * 255 // 31,
                                    255,
                                )
                            else:
                                c = (
                                    ((n >> 8) & 15) * 17,
                                    ((n >> 4) & 15) * 17,
                                    (n & 15) * 17,
                                    ((n >> 12) & 7) * 255 // 7,
                                )
                        elif fmt == 1:
                            v = d[at]
                            at += 1
                            c = (v, v, v, 255)
                        else:
                            v = ((d[at // 2] >> (4 if at % 2 == 0 else 0)) & 15) * 17
                            at += 1
                            c = (v, v, v, 255)
                        if yy + y < h and xx + x < w:
                            out[yy + y, xx + x] = c
    else:
        raise ValueError(fmt)
    return out


def encode(image):
    h, w, _ = image.shape
    out = bytearray()
    for yy in range(0, h, 8):
        for xx in range(0, w, 8):
            for i in range(64):
                x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
                y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
                out.extend(image[yy + y, xx + x][::-1].tobytes())
    return bytes(out)


def encode_etc1a4(image):
    h, w, _ = image.shape
    tables = np.array(
        [
            [2, 8, -2, -8],
            [5, 17, -5, -17],
            [9, 29, -9, -29],
            [13, 42, -13, -42],
            [18, 60, -18, -60],
            [24, 80, -24, -80],
            [33, 106, -33, -106],
            [47, 183, -47, -183],
        ]
    )
    blocks = np.array(
        [
            image[y + dy : y + dy + 4, x + dx : x + dx + 4]
            for y in range(0, h, 8)
            for x in range(0, w, 8)
            for dy in (0, 4)
            for dx in (0, 4)
        ]
    )
    rgb = blocks[:, :, :, :3].astype(np.int32)
    N = len(blocks)
    options = []
    for flip in (0, 1):
        error = np.zeros(N, dtype=np.int64)
        control = np.full(N, flip, dtype=np.uint32)
        selectors = np.zeros((N, 4, 4), dtype=np.uint32)
        for half in (0, 1):
            sl = (
                (slice(None), slice(half * 2, half * 2 + 2), slice(None))
                if flip
                else (slice(None), slice(None), slice(half * 2, half * 2 + 2))
            )
            values = rgb[sl].reshape(N, 8, 3)
            base = np.clip(np.rint(values.mean(1) / 17), 0, 15).astype(np.int32)
            costs = []
            choices = []
            for table in tables:
                palette = np.clip(base[:, None, :] * 17 + table[None, :, None], 0, 255)
                dist = ((values[:, :, None, :] - palette[:, None, :, :]) ** 2).sum(3)
                choice = dist.argmin(2)
                cost = dist.min(2).sum(1)
                costs.append(cost)
                choices.append(choice)
            costs = np.array(costs).T
            ti = costs.argmin(1)
            error += costs[np.arange(N), ti]
            choice = np.stack(choices, axis=1)[np.arange(N), ti]
            selectors[sl] = choice.reshape((N, 2, 4) if flip else (N, 4, 2))
            control |= (
                (base[:, 0].astype("u4") << (28 - half * 4))
                | (base[:, 1].astype("u4") << (20 - half * 4))
                | (base[:, 2].astype("u4") << (12 - half * 4))
                | (ti.astype("u4") << (5 - half * 3))
            )
        bits = np.zeros(N, dtype="u4")
        alpha = np.zeros(N, dtype="u8")
        for x in range(4):
            for y in range(4):
                i = x * 4 + y
                v = selectors[:, y, x]
                bits |= (v & 1) << i
                bits |= (v >> 1) << (16 + i)
                alpha |= (np.rint(blocks[:, y, x, 3] / 17).astype("u8") & 15) << (4 * i)
        options.append((error, control, bits, alpha))
    choose = options[1][0] < options[0][0]
    control = np.where(choose, options[1][1], options[0][1])
    bits = np.where(choose, options[1][2], options[0][2])
    alpha = options[0][3]
    out = bytearray()
    for i in range(N):
        out.extend(struct.pack("<QII", int(alpha[i]), int(bits[i]), int(control[i])))
    return bytes(out)
