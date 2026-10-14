#!/usr/bin/env python3
"""Create a 16:9 layout variant of the supplied Restoration HD HUD archive.
No Nintendo assets are included. Always derive from the unmodified mod archive.
"""
import argparse
import hashlib
from pathlib import Path
import struct

SOURCE_SHA256 = '3b5918df9251366f3688965bed882e56d1056d5e6dee2a479a8596cb884fc639'
# Keep native edge distances with the emulator's 400/240 -> 16/9 correction.
MARGIN = (240 * 16 / 9 - 400) / 2
TARGETS = {
    'Hud2': {'item_icon_set': MARGIN, 'icon_item_oca_g': MARGIN,
             'hud_dpad': -MARGIN, 'rupee_g': -MARGIN, 'key_g': -MARGIN,
             'heart_g': -MARGIN},
    'Hud': {'action_button_g': MARGIN, 'chat_alart': -MARGIN},
}

def patch(source):
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError('Unrecognized source archive; use the original supplied HD HUD layout.')
    result = bytearray(source)
    changes = []
    start = 0
    while True:
        start = source.find(b'MFL ', start)
        if start < 0:
            break
        count = struct.unpack_from('<H', source, start + 10)[0]
        panes = struct.unpack_from('<H', source, start + 14)[0]
        strings = start + struct.unpack_from('<I', source, start + 28)[0]
        names = source[strings:].split(b'\0')
        layout = names[0].decode('ascii')
        for index in range(count):
            name = names[1 + panes + index].decode('ascii')
            delta = TARGETS.get(layout, {}).get(name)
            if delta is None:
                continue
            offset = start + 0x20 + index * 0x44 + 8
            old = struct.unpack_from('<f', source, offset)[0]
            struct.pack_into('<f', result, offset, old + delta)
            changes.append((layout, name, offset, old, old + delta))
        start += 4
    if len(changes) != sum(map(len, TARGETS.values())):
        raise ValueError('Expected HUD groups were not found exactly once.')
    allowed = {offset + i for _, _, offset, _, _ in changes for i in range(4)}
    assert len(source) == len(result)
    assert all(a == b or i in allowed for i, (a, b) in enumerate(zip(source, result)))
    return result, changes

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error('Source and output must differ.')
    result, changes = patch(args.source.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    for layout, name, _, old, new in changes:
        print(f'{layout}.{name}: X {old:g} -> {new:.4f}')
