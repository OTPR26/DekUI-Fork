#!/usr/bin/env python3
"""Compare uniform ranges; changes are candidates, never proof of camera identity."""
import argparse
import json
import math
from collections import defaultdict


def summarize(path):
    with open(path) as source:
        capture = json.load(source)
    groups = defaultdict(list)
    for draw in capture['draws']:
        key = tuple(draw[k] for k in ('shader', 'swizzle', 'vertices', 'width', 'height', 'depth_state'))
        groups[key].append(draw)
    return capture, groups


def bounds(draws, slot):
    result = []
    for component in range(4):
        values = [d['uniforms'][slot][component] for d in draws]
        values = [v for v in values if isinstance(v, (float, int)) and math.isfinite(v)]
        result.extend((min(values), max(values)) if values else (None, None))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('a')
    parser.add_argument('b')
    parser.add_argument('--limit', type=int, default=30)
    args = parser.parse_args()
    a, ga = summarize(args.a)
    b, gb = summarize(args.b)
    for name, capture in [('A', a), ('B', b)]:
        print(f"{name}: {capture['label']}; {len(capture['draws'])} draws; {capture['stop_reason']}; software skipped={capture['fallback_draws']}")
    print(f"Shared signatures: {len(ga.keys() & gb.keys())}; A-only: {len(ga.keys()-gb.keys())}; B-only: {len(gb.keys()-ga.keys())}")
    print('Range changes may reflect actors, animation, draw order, or camera. Signatures are not object identities.')
    candidates = []
    for key in ga.keys() & gb.keys():
        for slot in range(96):
            ra, rb = bounds(ga[key], slot), bounds(gb[key], slot)
            deltas = [abs(x-y) for x, y in zip(ra, rb) if x is not None and y is not None]
            if deltas and max(deltas) > 1e-6:
                candidates.append((max(deltas), key, slot, ra, rb))
    for delta, key, slot, ra, rb in sorted(candidates, reverse=True)[:max(0, args.limit)]:
        print(f'shader={key[0]} swizzle={key[1]} vertices={key[2]} viewport={key[3:5]} depth={key[5]} slot={slot} max_range_delta={delta:.6g}')
        print(f'  A min/max xyzw: {ra}\n  B min/max xyzw: {rb}')


if __name__ == '__main__':
    main()
