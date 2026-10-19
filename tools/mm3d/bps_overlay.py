"""Overlay authored edits on a BPS patch without embedding its source executable."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def number(value):
    out = bytearray()
    while True:
        byte = value & 127
        value >>= 7
        if not value:
            return out + bytes([byte | 128])
        out.append(byte)
        value -= 1


def parse(data):
    if data[:4] != b'BPS1' or zlib.crc32(data[:-4]) != struct.unpack_from('<I', data, len(data)-4)[0]:
        raise ValueError('Invalid BPS patch')
    at = 4
    def read():
        nonlocal at
        value, shift = 0, 1
        while at < len(data)-12:
            byte = data[at]; at += 1
            value += (byte & 127) * shift
            if byte & 128: return value
            shift <<= 7; value += shift
        raise ValueError('Truncated BPS instruction')
    source, target, metadata = read(), read(), read()
    at += metadata
    commands = []
    output = source_pos = target_pos = 0
    while at < len(data)-12:
        word = read(); mode, size = word & 3, (word >> 2) + 1
        if mode == 0:
            argument = output
        elif mode == 1:
            argument = data[at:at+size]; at += size
            if len(argument) != size: raise ValueError('Truncated literal')
        else:
            delta = read(); delta = -(delta >> 1) if delta & 1 else delta >> 1
            if mode == 2:
                source_pos += delta; argument = source_pos; source_pos += size
            else:
                target_pos += delta; argument = target_pos; target_pos += size
                if not 0 <= argument < output: raise ValueError('Invalid target copy')
        if mode in (0, 2) and not 0 <= argument <= source-size:
            raise ValueError('Invalid source copy')
        commands.append((output, mode, size, argument)); output += size
    if at != len(data)-12 or output != target: raise ValueError('Invalid BPS output size')
    return source, target, commands


def apply(source, patch):
    source_size, target_size, commands = parse(patch)
    if len(source) != source_size or zlib.crc32(source) != struct.unpack_from('<I', patch, len(patch)-12)[0]:
        raise ValueError('BPS source mismatch')
    out = bytearray()
    for _, mode, size, arg in commands:
        if mode in (0, 2): out.extend(source[arg:arg+size])
        elif mode == 1: out.extend(arg)
        else:
            for i in range(size): out.append(out[arg+i])
    if len(out) != target_size or zlib.crc32(out) != struct.unpack_from('<I', patch, len(patch)-8)[0]:
        raise ValueError('BPS target mismatch')
    return bytes(out)


def overlay(baseline, spec):
    if hashlib.sha256(baseline).hexdigest() != spec['baseline_patch_sha256']:
        raise ValueError('Unexpected baseline patch')
    source_size, target_size, commands = parse(baseline)
    edits = sorted((e['offset'], bytes.fromhex(e['data'])) for e in spec['edits'])
    end = 0
    for start, data in edits:
        if not data or start < end or start+len(data) > target_size: raise ValueError('Invalid overlay range')
        end = start+len(data)
    # Resolve every baseline byte to a source offset or a baseline literal.
    # Target copies must refer to the unmodified baseline, even when an earlier
    # location is edited. This map contains offsets, never the ROM contents.
    from array import array
    origins = array('i')
    literals = bytearray()
    for _, mode, size, arg in commands:
        if mode in (0, 2):
            origins.extend(range(arg, arg+size))
        elif mode == 1:
            origins.extend(range(source_size+len(literals), source_size+len(literals)+size))
            literals.extend(arg)
        else:
            remaining = size
            while remaining:
                count = min(remaining, len(origins)-arg)
                origins.extend(origins[arg:arg+count]); arg += count; remaining -= count
    for start, data in edits:
        origins[start:start+len(data)] = array('i', range(source_size+len(literals), source_size+len(literals)+len(data)))
        literals.extend(data)
    out = bytearray(b'BPS1') + number(source_size) + number(target_size) + number(0)
    source_pos = target_pos = literal_count = 0
    def emit(mode, count, argument):
        nonlocal source_pos, target_pos, literal_count
        out.extend(number(((count-1) << 2) | mode))
        if mode == 1:
            out.extend(argument); literal_count += count
        elif mode in (2, 3):
            previous = source_pos if mode == 2 else target_pos
            delta = argument-previous
            out.extend(number((abs(delta) << 1) | (delta < 0)))
            if mode == 2: source_pos = argument+count
            else: target_pos = argument+count
    for begin, mode, size, argument in commands:
        cuts = {begin, begin+size}
        for start, data in edits:
            for point in (start, start+len(data)):
                if begin < point < begin+size: cuts.add(point)
        points = sorted(cuts)
        for start, stop in zip(points, points[1:]):
            replacement = next((data[start-offset:stop-offset] for offset, data in edits
                                if offset <= start and stop <= offset+len(data)), None)
            if replacement is not None:
                emit(1, stop-start, replacement)
            elif mode == 1:
                emit(1, stop-start, argument[start-begin:stop-begin])
            elif mode in (0, 2):
                emit(mode, stop-start, argument+start-begin)
            else:
                reference = argument+start-begin
                if not any(reference < offset+len(data) and offset < reference+stop-start
                           for offset, data in edits):
                    emit(3, stop-start, reference)
                    continue
                # Resolve only target copies that would inherit an edited byte.
                position = start
                while position < stop:
                    origin = origins[position]; finish = position+1
                    if origin < source_size:
                        while finish < stop and origins[finish] < source_size and origins[finish] == origin+finish-position:
                            finish += 1
                        emit(2, finish-position, origin)
                    else:
                        payload = bytearray([literals[origin-source_size]])
                        while finish < stop and origins[finish] >= source_size:
                            payload.append(literals[origins[finish]-source_size]); finish += 1
                        emit(1, finish-position, payload)
                    position = finish
    baseline_literals = sum(size for _, mode, size, _ in commands if mode == 1)
    if literal_count > baseline_literals+sum(len(data) for _, data in edits):
        raise ValueError('Overlay expanded the literal payload')
    out += baseline[-12:-8] + struct.pack('<I', int(spec['target_crc32'], 16))
    out += struct.pack('<I', zlib.crc32(out))
    parse(out)
    return bytes(out)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('recipe', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_bytes(overlay(args.baseline.read_bytes(), json.loads(args.recipe.read_text())))
