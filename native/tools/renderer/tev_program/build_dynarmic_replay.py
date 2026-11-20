"""Build only the developer Dynarmic bridge against read-only Azahar libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def build(compiler, azahar, donor_build, output):
    here = Path(__file__).resolve().parent
    source = here / 'dynarmic_replay_bridge.cpp'
    header = here / 'dynarmic_replay_bridge.h'
    dynarmic = azahar / 'externals/dynarmic'
    libs = [donor_build / 'externals' / name for name in (
        'dynarmic/src/dynarmic/Release/dynarmic.lib',
        'dynarmic/externals/mcl/src/Release/mcl.lib',
        'dynarmic/externals/zydis/Release/Zydis.lib',
        'dynarmic/externals/zydis/zycore/Release/Zycore.lib',
        'fmt/Release/fmt.lib')]
    for path in [compiler, source, header, *libs]:
        if not path.is_file():
            raise FileNotFoundError(path)
    output.mkdir(parents=True, exist_ok=False)
    output = output.resolve()
    dll = output / 'dynarmic-replay.dll'
    includes = [dynarmic / 'src', dynarmic / 'externals/mcl/include',
                azahar / 'externals/boost', azahar / 'externals/fmt/include']
    obj = output / 'bridge.obj'
    commands = [[str(compiler), '/nologo', '/c', '/MD', '/O2', '/EHsc', '/std:c++20',
                 '/DNOMINMAX', '/DNDEBUG', *(f'/I{path}' for path in includes),
                 str(source), f'/Fo{obj}'],
                [str(compiler), '/nologo', '/LD', '/MD', str(obj),
                 *(str(path) for path in libs), f'/Fe{dll}', '-fuse-ld=lld',
                 '/link', '/NOIMPLIB', '/threads:2']]
    started = time.monotonic()
    for command in commands:
        subprocess.run(command, check=True, timeout=120)
    receipt = {'developer_only': True, 'product_dependency': False,
               'donor_checkout_modified': False, 'commands': commands,
               'elapsed_seconds': time.monotonic() - started,
               'sha256': {str(path): digest(path) for path in [source, header, *libs, dll]}}
    (output / 'build.json').write_text(json.dumps(receipt, indent=2))
    return dll


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    for name in ('compiler', 'azahar-root', 'azahar-build', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    print(build(args.compiler, args.azahar_root, args.azahar_build, args.output))
