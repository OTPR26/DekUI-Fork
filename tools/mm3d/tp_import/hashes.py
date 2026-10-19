"""Dolphin texture names use XXH64 over the original base mip level."""

import struct

MASK = (1 << 64) - 1
P1, P2, P3, P4, P5 = (
    11400714785074694791,
    14029467366897019727,
    1609587929392839161,
    9650029242287828579,
    2870177450012600261,
)


def rot(x, n):
    return ((x << n) | (x >> (64 - n))) & MASK


def step(a, b):
    return rot((a + b * P2) & MASK, 31) * P1 & MASK


def xxh64(data):
    n = len(data)
    i = 0
    if n >= 32:
        v = [(P1 + P2) & MASK, P2, 0, (-P1) & MASK]
        while i <= n - 32:
            for j, x in enumerate(struct.unpack_from("<4Q", data, i)):
                v[j] = step(v[j], x)
            i += 32
        h = sum(rot(x, r) for x, r in zip(v, [1, 7, 12, 18])) & MASK
        for x in v:
            h = ((h ^ step(0, x)) * P1 + P4) & MASK
    else:
        h = P5
    h = (h + n) & MASK
    while i <= n - 8:
        h = (
            rot(h ^ step(0, struct.unpack_from("<Q", data, i)[0]), 27) * P1 + P4
        ) & MASK
        i += 8
    if i <= n - 4:
        h = (
            rot(h ^ (struct.unpack_from("<I", data, i)[0] * P1 & MASK), 23) * P2 + P3
        ) & MASK
        i += 4
    while i < n:
        h = rot(h ^ (data[i] * P5 & MASK), 11) * P1 & MASK
        i += 1
    h ^= h >> 33
    h = h * P2 & MASK
    h ^= h >> 29
    h = h * P3 & MASK
    h ^= h >> 32
    return f"{h:016X}"


def dolphin_hash(t):
    w, h, f = t["width"], t["height"], t["format"]
    bw, bh, bits = {0: (8, 8, 4), 1: (8, 4, 8), 5: (4, 4, 16), 14: (8, 8, 4)}[f]
    size = ((w + bw - 1) // bw * bw) * ((h + bh - 1) // bh * bh) * bits // 8
    return xxh64(t["data"][:size])
