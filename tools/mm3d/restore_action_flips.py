#!/usr/bin/env python3
"""Restore native A/B prompt tracks using a user's own ROMFS layout archive.
No game assets are bundled. Other GAR entries and layout data remain unchanged.
"""
import argparse
import os
from pathlib import Path
import struct


def u16(b, at):
    return struct.unpack_from('<H', b, at)[0]


def u32(b, at):
    return struct.unpack_from('<I', b, at)[0]


def string(b, at):
    if not 0 <= at < len(b):
        raise ValueError('Invalid string offset')
    return b[at:b.index(b'\0', at)].decode('ascii')


def entries(b):
    if b[:4] != b'GAR\x02' or u32(b, 4) != len(b):
        raise ValueError('Unsupported GAR header')
    count, table, offsets = u16(b, 10), u32(b, 16), u32(b, 20)
    result = {}
    for i in range(count):
        record, address = table + i * 12, offsets + i * 4
        size, start = u32(b, record), u32(b, address)
        name = string(b, u32(b, record + 8))
        if start + size > len(b) or name in result:
            raise ValueError('Invalid or duplicate GAR entry')
        result[name] = (record, address, b[start:start + size])
    return result


def layout(b):
    if b[:4] != b'MFL ' or u32(b, 4) != 4:
        raise ValueError('Unsupported MFL header')
    node_count, panes = u16(b, 10), u16(b, 14)
    players, anim_count = u16(b, 16), u16(b, 18)
    start, strings = u32(b, 24), u32(b, 28)
    names, pos = [], strings
    for _ in range(1 + panes + node_count + players + anim_count):
        name = string(b, pos)
        names.append(name)
        pos += len(name) + 1
    widgets = names[1 + panes:1 + panes + node_count]
    anim_names = names[1 + panes + node_count + players:]
    animations, pos = {}, start
    for name in anim_names:
        begin = pos
        tracks, rate = u16(b, pos), u16(b, pos + 2)
        if rate != 30 or name in animations:
            raise ValueError('Unsupported animation header')
        pos += 8
        targets = []
        for _ in range(tracks):
            target, keys = u16(b, pos), u32(b, pos + 4)
            if target >= node_count or keys > 1024:
                raise ValueError('Invalid animation track')
            targets.append((pos - begin, widgets[target]))
            pos += 12 + keys * 12
            if pos > strings:
                raise ValueError('Animation exceeds section')
        animations[name] = (b[begin:pos], targets)
    if pos != strings:
        raise ValueError('Unexpected animation section padding')
    return widgets, animations


def restore_layout(original, modified):
    _, native = layout(original)
    widgets, current = layout(modified)
    replacements = {}
    for name in ('action_text_display', 'action_text_vanish'):
        data, targets = native[name]
        if not targets:
            raise ValueError('Original animation has no tracks')
        mapped = bytearray(data)
        for at, target in targets:
            if widgets.count(target) != 1:
                raise ValueError('Animation target missing or ambiguous: ' + target)
            struct.pack_into('<H', mapped, at, widgets.index(target))
        old = current[name][0]
        if u16(old, 0) and old != mapped:
            raise ValueError('Existing custom animation; refusing to replace ' + name)
        replacements[name] = bytes(mapped)
    data = b''.join(replacements.get(name, value[0]) for name, value in current.items())
    start, strings = u32(modified, 24), u32(modified, 28)
    result = bytearray(modified[:start] + data + modified[strings:])
    struct.pack_into('<I', result, 28, start + len(data))
    layout(result)
    return bytes(result)


def patch(original, modified):
    originals, installed = entries(original), entries(modified)
    result = bytearray(modified)
    for name in ('AActionIcon.mfl', 'BActionIcon.mfl'):
        record, address, current = installed[name]
        fixed = restore_layout(originals[name][2], current)
        if fixed == current:
            continue
        # Append replacements: unrelated payloads keep their offsets and bytes.
        result.extend(b'\0' * (-len(result) % 8))
        struct.pack_into('<I', result, address, len(result))
        struct.pack_into('<I', result, record, len(fixed))
        result.extend(fixed)
    struct.pack_into('<I', result, 4, len(result))
    actual = entries(result)
    for name, (_, _, payload) in installed.items():
        if name not in ('AActionIcon.mfl', 'BActionIcon.mfl'):
            assert actual[name][2] == payload
    return bytes(result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path, help='Original dumped Joker.Main.Play.gar')
    parser.add_argument('installed', type=Path, help='Restoration HUD Joker.Main.Play.gar')
    args = parser.parse_args()
    if args.original.resolve() == args.installed.resolve():
        parser.error('Original and installed archives must differ')
    before = args.installed.read_bytes()
    result = patch(args.original.read_bytes(), before)
    if result == before:
        print('Native A/B animation tracks are already restored.')
    else:
        backup = args.installed.with_name(args.installed.name + '.before-action-flips')
        if backup.exists() and backup.read_bytes() != before:
            raise ValueError('Backup differs; refusing to overwrite it')
        backup.write_bytes(before)
        temporary = args.installed.with_name(args.installed.name + '.action-flips-tmp')
        temporary.write_bytes(result)
        os.replace(temporary, args.installed)
        print('Restored native A/B display/vanish tracks. Restart the game to load them.')
