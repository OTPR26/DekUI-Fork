"""Generate a private TP model stage; the desktop frontend finalizes its textures."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--mm-rom')
    parser.add_argument('--tp-rom')
    parser.add_argument('--probe-runtime', action='store_true')
    args = parser.parse_args()
    runtime = args.package / 'runtime'
    sys.path.insert(0, str(runtime / 'tp-modules'))
    dll_dirs = []
    if sys.platform == 'win32':
        for directory in (args.package, runtime / 'tp'):
            dll_dirs.append(os.add_dll_directory(str(directory)))
    library = 'nod.dll' if sys.platform == 'win32' else 'libnod.dylib' if sys.platform == 'darwin' else 'libnod.so'
    from tp_import.generate import generate
    if args.probe_runtime:
        import numpy
        reader = ctypes.CDLL(str(runtime / 'tp' / library))
        for symbol in ('nod_disc_open', 'nod_read', 'nod_seek', 'nod_free'):
            getattr(reader, symbol)
        assert numpy.array([1, 2, 3]).sum() == 6
        print('TP converter modules and native disc reader loaded successfully.')
        return
    if not args.root or not args.mm_rom or not args.tp_rom:
        parser.error('--root, --mm-rom and --tp-rom are required for generation')
    stage = generate(args.root / 'user', args.mm_rom, args.tp_rom,
                     runtime / 'tp' / library,
                     lambda message: print(json.dumps({'progress': message}), flush=True))
    report = json.loads((Path(stage) / 'generated.json').read_text())
    print(json.dumps({'stage': stage, 'variants': report['variants'],
                      'clips': report['voices']['clips']}), flush=True)


if __name__ == '__main__':
    main()
