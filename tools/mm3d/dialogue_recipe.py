"""Emit sparse text edits; unchanged dialogue is read from the user's game."""
from difflib import SequenceMatcher
import struct


def fnv(raw):
    value = 0xcbf29ce484222325
    for byte in raw:
        value = ((value ^ byte) * 0x100000001b3) & ((1 << 64) - 1)
    return value


def replace_reviewed(text, rule, replacement):
    """Match a reviewed substring fingerprint without bundling its source text."""
    length = rule['length']
    if length <= 0:
        raise ValueError('Invalid reviewed substring length')
    expected = int(rule['source_hash'], 16)
    parts = []
    start = previous = matches = 0
    while start+length <= len(text):
        if fnv(text[start:start+length].encode()) == expected:
            parts.extend((text[previous:start], replacement))
            start += length
            previous = start
            matches += 1
        else:
            start += 1
    parts.append(text[previous:])
    return ''.join(parts), matches


def recipe(original, edited=None):
    edited = original if edited is None else edited
    if not original or max(len(original), len(edited)) > 4096:
        raise ValueError('Unsupported dialogue allocation')
    commands = bytearray()
    if edited != original:
        for tag, i, j, a, b in SequenceMatcher(None, original, edited, autojunk=False).get_opcodes():
            if tag == 'equal':
                commands += struct.pack('<HHH', i, j-i, 0)
            elif tag in ('replace', 'insert'):
                commands += struct.pack('<HHH', 0, 0, b-a) + edited[a:b]
        if not commands:
            commands += struct.pack('<HHH', 0, 0, 0)
    literal = ''.join('\\%03o' % byte for byte in commands)
    return ('{' + f'{len(original)},0x{fnv(original):016x}ULL,{len(edited)},'
            f'0x{fnv(edited):016x}ULL,{{"{literal}",{len(commands)}}}' + '}')
