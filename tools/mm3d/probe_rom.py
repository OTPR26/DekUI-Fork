#!/usr/bin/env python3
"""Read-only local NCSD fingerprint. Emits metadata/hashes only; never extracts content.
Offsets are from Azahar src/core/file_sys/ncch_container.h and .cpp at the recorded baseline.
This is a candidate probe, NOT a revision whitelist or proof of an unmodified game.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def probe(path):
    if path.suffix.lower() not in ('.3ds', '.cci'):
        raise ValueError('Select a decrypted .3ds or .cci dump')
    with path.open('rb') as stream:
        size = path.stat().st_size
        def read(offset, count):
            if offset < 0 or count < 0 or offset + count > size:
                raise ValueError('Truncated or invalid container')
            stream.seek(offset)
            data = stream.read(count)
            if len(data) != count:
                raise ValueError('Short read')
            return data
        header = read(0, 512)
        if header[0x100:0x104] != b'NCSD':
            raise ValueError('Expected NCSD cartridge container')
        offset, blocks = struct.unpack_from('<II', header, 0x120)
        base, length = offset * 512, blocks * 512
        if base < 512 or length < 0x210 or base + length > size:
            raise ValueError('Invalid main partition bounds')
        ncch = read(base, 512)
        if ncch[0x100:0x104] != b'NCCH':
            raise ValueError('Expected main NCCH partition')
        if not ncch[0x18f] & 4:
            raise ValueError('Main partition is encrypted; no keys or decryption are provided')
        program_id = struct.unpack_from('<Q', ncch, 0x118)[0]
        remaster = struct.unpack('<H', read(base + 0x20e, 2))[0]
        digest = hashlib.sha256()
        stream.seek(base)
        remaining = length
        while remaining:
            data = stream.read(min(1024 * 1024, remaining))
            if not data:
                raise ValueError('Short partition read')
            digest.update(data)
            remaining -= len(data)
        return {'schema': 1, 'program_id': f'{program_id:016X}',
                'remaster_version': remaster, 'main_partition_bytes': length,
                'main_partition_sha256': digest.hexdigest(), 'no_crypto_flag': True,
                'mm3d_semantic_mode': False, 'revision_verified': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.rom), indent=2))
    except (OSError, ValueError) as error:
        # Avoid exposing local paths in output intended for sharing.
        parser.exit(1, f'Probe failed: {error.__class__.__name__}: '
                    + (str(error) if isinstance(error, ValueError) else 'cannot read input') + '\n')
