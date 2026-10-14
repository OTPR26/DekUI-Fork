#!/usr/bin/env python3
"""Hold map entrance/exit tracks at their open pose in a user's local GAR."""
import argparse
import struct
from pathlib import Path
from restore_action_flips import entries, layout, u16, u32


def patch(data):
    result = bytearray(data)
    _, _, resource = entries(data)['MenuMapDungeon.mfl']
    widgets, animations = layout(resource)
    base = u32(data, entries(data)['MenuMapDungeon.mfl'][1])
    pos = u32(resource, 24)
    changed = 0
    for name, (raw, targets) in animations.items():
        p = 8
        for _ in targets:
            obj, prop, count, kind, duration = struct.unpack_from('<HHHHI', raw, p)
            p += 12
            offsets = list(range(p, p + (duration + 1) * 4, 4) if kind == 2
                           else range(p + 8, p + count * 12, 12))
            value = None
            if name in ('in_menu_map_anim', 'out_menu_map_anim') and prop <= 2:
                # Native open positions; retain all visibility/selection tracks.
                value = 202.0 if widgets[obj] == 'btn_return_l' and prop == 1 else 0.0
            if name in ('in_bb_map_area', 'out_bb_map_area'):
                if prop == 0:
                    value = 256.0 if widgets[obj] == 'bb_map_area_right' else 0.0
                elif prop == 18:
                    value = 1.0
            if value is not None:
                for offset in offsets:
                    at = base + pos + offset
                    packed = struct.pack('<f', value)
                    changed += result[at:at + 4] != packed
                    result[at:at + 4] = packed
            p += (duration + 1) * 4 if kind == 2 else count * 12
        pos += len(raw)
    return bytes(result), changed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    original = args.archive.read_bytes()
    updated, count = patch(original)
    if count:
        backup = args.archive.with_name(args.archive.name + '.before-static-map')
        if not backup.exists():
            backup.write_bytes(original)
        temporary = args.archive.with_name(args.archive.name + '.static-map.tmp')
        temporary.write_bytes(updated)
        temporary.replace(args.archive)
    print(f'Updated {count} map keyframes. Restart the game to load the archive.')
