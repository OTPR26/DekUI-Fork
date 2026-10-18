"""Reject mislabeled or mixed-architecture desktop packages before upload."""
import argparse
from pathlib import Path
import struct
import tarfile
import zipfile


def hybrid_arm64(data, pe):
    # Windows ARM64 runtime DLLs can carry an AMD64 COFF header plus CHPE
    # metadata. Inspect that metadata instead of rejecting Microsoft's native
    # ARM64X/EC runtime as an x64-only binary. App executables remain strict.
    optional = pe + 24
    if len(data) < optional + 200 or struct.unpack_from('<H', data, optional)[0] != 0x20b:
        return False
    image_base = struct.unpack_from('<Q', data, optional + 24)[0]
    sections = pe + 24 + struct.unpack_from('<H', data, pe + 20)[0]
    def position(rva):
        for i in range(struct.unpack_from('<H', data, pe + 6)[0]):
            at = sections + i * 40
            virtual, size, raw = struct.unpack_from('<III', data, at + 12)
            if virtual <= rva < virtual + size:
                result = raw + rva - virtual
                if result < len(data):
                    return result
        return None
    rva, size = struct.unpack_from('<II', data, optional + 112 + 10 * 8)
    config = position(rva) if rva and size >= 208 else None
    if config is None or config + 208 > len(data):
        return False
    pointer = struct.unpack_from('<Q', data, config + 200)[0]
    metadata = position(pointer - image_base) if pointer >= image_base else None
    return metadata is not None and metadata + 12 <= len(data) and struct.unpack_from('<I', data, metadata)[0] in (1, 2)


def architecture(data):
    if data[:2] == b'MZ':
        offset = struct.unpack_from('<I', data, 0x3c)[0]
        if data[offset:offset + 4] != b'PE\0\0':
            raise ValueError('Invalid PE header')
        machine = struct.unpack_from('<H', data, offset + 4)[0]
        if machine == 0x8664 and hybrid_arm64(data, offset):
            return 'arm64-hybrid'
        return {0xaa64: 'arm64', 0x8664: 'x64'}.get(machine, f'PE:{machine:x}')
    if data[:4] == b'\x7fELF':
        if data[4] != 2 or data[5] not in (1, 2):
            raise ValueError('Expected 64-bit ELF')
        machine = struct.unpack_from('<H' if data[5] == 1 else '>H', data, 18)[0]
        return {183: 'arm64', 62: 'x64'}.get(machine, f'ELF:{machine:x}')
    raise ValueError('Not a PE or ELF executable')


def check(package, expected):
    count = 0
    def inspect(name, data):
        nonlocal count
        actual = architecture(data)
        compatible_runtime = expected == 'arm64' and actual == 'arm64-hybrid' and name.lower().endswith('.dll')
        if actual != expected and not compatible_runtime:
            raise ValueError(f'{name}: expected {expected}, found {actual}')
        count += 1
    if zipfile.is_zipfile(package):
        with zipfile.ZipFile(package) as archive:
            for name in archive.namelist():
                if Path(name).suffix.lower() in ('.exe', '.dll', '.pyd'):
                    inspect(name, archive.read(name))
    else:
        with tarfile.open(package) as archive:
            for entry in archive:
                if entry.isfile() and entry.name.lower().endswith('.appimage'):
                    inspect(entry.name, archive.extractfile(entry).read(4096))
    if not count:
        raise ValueError('No executable payload was checked')
    print(f'Verified {count} {expected} executable payloads')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('architecture', choices=('x64', 'arm64'))
    args = parser.parse_args()
    check(args.package, args.architecture)
