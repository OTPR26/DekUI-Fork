"""Compile and execute nested AOT state transitions across real DLL boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / 'tools/oot3d/native_a32_runtime'))
from whole_aot_cpp import PROGRAM_FORMAT, SELECTION_FORMAT
from tools.triaevum_release.whole_aot_plugin_backend import WholeAotPluginToolchain, build_whole_aot_plugin


def main():
    parser = argparse.ArgumentParser(__doc__)
    for name in ('compiler', 'support', 'include', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    words = [0xE1A00000] * (0xD4 // 4)
    for offset, body in ((0, [0xE1A0800E, 0xE3A00005, 0xEB00000C, 0xE2800003,
                              0xE3500010, 0x03A0102A, 0x13A01063, 0xE12FFF18]),
                         (0x40, [0xE1A0900E, 0xEB00000D, 0xE2800006, 0xE1A0E009, 0xE12FFF1E]),
                         (0x80, [0xE2800002, 0xE280200A, 0xE5832000, 0xE12FFF1E]),
                         (0xC0, [0xE3500001, 0x03A0102A, 0x012FFF1E, 0xE3A01063, 0xE12FFF1E])):
        words[offset // 4:offset // 4 + len(body)] = body
    code = struct.pack('<' + 'I' * len(words), *words)
    blocks = []
    for pc, end, successors in (
        (0x1000, 0x100C, [('call', 0x1040), ('fallthrough', 0x100C)]),
        (0x100C, 0x1020, [('return', None)]),
        (0x1040, 0x1048, [('call', 0x1080), ('fallthrough', 0x1048)]),
        (0x1048, 0x1054, [('return', None)]),
        (0x1080, 0x1084, [('fallthrough', 0x1084)]),
        (0x1084, 0x1090, [('return', None)]),
        (0x10C0, 0x10CC, [('return', None), ('fallthrough', 0x10CC)]),
        (0x10CC, 0x10D4, [('return', None)]),
    ):
        blocks.append({'id': len(blocks), 'pc': pc, 'end_pc': end,
                       'successors': [{'site': end - 4, 'kind': kind,
                                       **({'target': target} if target is not None else {})}
                                      for kind, target in successors]})
    functions = []
    for entry, name, ids, calls in ((0x1000, 'Caller', [0, 1], [(0x1008, 0x1040)]),
                                    (0x1040, 'Middle', [2, 3], [(0x1044, 0x1080)]),
                                    (0x1080, 'Leaf', [4, 5], []),
                                    (0x10C0, 'ConditionalReturn', [6, 7], [])):
        functions.append({'entry': entry, 'name': name, 'blocks': ids, 'closed_static_cfg': True,
                          'direct_calls': [{'site': site, 'target': target} for site, target in calls],
                          'tail_calls': [], 'indirect_sites': []})
    program = {'format': PROGRAM_FORMAT, 'base': 0x1000, 'code_sha256': hashlib.sha256(code).hexdigest(),
               'blocks': blocks, 'functions': functions}
    selection = {'format': SELECTION_FORMAT, 'functions': [{'entry': f['entry']} for f in functions]}
    for name, data in (('program.json', program), ('selection.json', selection)):
        (args.output / name).write_text(json.dumps(data, indent=2), encoding='utf-8')
    (args.output / 'synthetic.bin').write_bytes(code)
    built = build_whole_aot_plugin(
        program_path=args.output / 'program.json', selection_path=args.output / 'selection.json',
        code_path=args.output / 'synthetic.bin', cache_root=args.output / 'cache', shard_count=1, jobs=1,
        toolchain=WholeAotPluginToolchain(args.compiler, args.compiler.with_name('llvm-lib.exe'),
                                        args.support, args.include))
    includes = [REPO / 'tools/oot3d/native_game_runtime', REPO / 'tools/oot3d/native_a32_runtime',
                REPO / 'tools/oot3d/native_a32_runtime/upstream', args.include]
    executable = args.output / 'probe.exe'
    command = [str(args.compiler), '/nologo', '/EHsc', '/MT', '/std:c++20', '/O2', '/fp:strict',
               '/DNOMINMAX', *[f'/I{path}' for path in includes], '/c',
               str(Path(__file__).with_name('aot_state_boundary_probe.cpp')),
               f'/Fo{args.output / "probe.obj"}']
    link = [str(args.compiler), '/nologo', '/MT', str(args.output / 'probe.obj'),
            str(args.support), '-fuse-ld=lld', f'/Fe{executable}']
    subprocess.run(command, check=True)
    subprocess.run(link, check=True)
    result = subprocess.run([str(executable), built['plugin']], capture_output=True, text=True)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()
    (args.output / 'receipt.json').write_text(json.dumps({'checks': json.loads(result.stdout),
        'build': built, 'probe_command': command, 'link_command': link}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
