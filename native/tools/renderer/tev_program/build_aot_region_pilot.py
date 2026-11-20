"""Developer-only sparse AOT experiment; never populates the release object cache.

Reuses verified unchanged objects. The ONLY permitted common-header difference
is the layout-neutral friend declaration for the scoped memory view. All other
dependency differences stop the build. Production builds use the normal cache.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from triaevum_release import whole_aot_object_cache as cache

FRIEND = ("    // A call-free AOT region may retain views until its next observable exit.\n"
          "    // This does not change the memory object layout or the title-module ABI.\n"
          "    friend class NativeA32UnobservedMemory;\n").encode()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(__doc__)
    for name in ("generated", "baseline-manifest", "baseline-archive", "output", "compiler", "archiver", "support", "include"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--max-new-objects", type=int, default=3)
    args = p.parse_args()
    started = time.monotonic()
    repo = TOOLS.parent
    record = json.loads(args.baseline_archive.with_suffix('.build.json').read_text())
    baseline = json.loads(args.baseline_manifest.read_text())
    manifest, sources = cache._load_generated_sources(args.generated)
    toolchain = cache.NativeToolchain(args.compiler, args.archiver)
    sysroot = cache.compiler_sysroot(toolchain.compiler, toolchain.sysroot)
    tool_id, toolchain = cache._toolchain_identity(toolchain, sysroot)
    if tool_id != record['toolchain_identity_sha256']:
        raise RuntimeError('Toolchain differs from frozen control')
    old_deps = {v['role']: v for v in record['dependencies']}
    deps = cache.default_dependency_files(repo, args.generated, args.include)
    # This new opt-in helper is captured by each changed source's contract hash;
    # unchanged objects neither included it nor used the friend API.
    helper_role = 'repo/tools/oot3d/native_game_runtime/oot3d_native_a32_memory_region.h'
    helper = repo / helper_role.removeprefix('repo/')
    helper_hash = digest(helper)
    deps = tuple(d for d in deps if d.role != helper_role)
    for source in sources:
        text = source.path.read_text()
        if '#include "oot3d_native_a32_memory_region.h"' in text and (
                f'// memory-region contract: {helper_hash}' not in text):
            raise RuntimeError(f'Stale region helper in generated source: {source.name}')
    if set(old_deps) != {v.role for v in deps}:
        raise RuntimeError('Common dependency set differs')
    for dep in deps:
        data = dep.path.read_bytes()
        if dep.role.endswith('/oot3d_native_a32_memory.h'):
            data = data.replace(FRIEND, b'')
        if hashlib.sha256(data).hexdigest() != old_deps[dep.role]['sha256']:
            raise RuntimeError(f'Unapproved common dependency change: {dep.role}')
    if baseline['code_sha256'] != manifest['code_sha256'] or baseline['program_sha256'] != manifest['program_sha256']:
        raise RuntimeError('Structural program/code differs')
    old_sources = sorted(n for n in baseline['files'] if n.endswith('.cpp'))
    if old_sources != [s.name for s in sources] or len(old_sources) != len(record['objects']):
        raise RuntimeError('Object/source ordering differs')
    changes = [s for s in sources if s.sha256 != baseline['files'][s.name]]
    if not changes or len(changes) > args.max_new_objects:
        raise RuntimeError(f'Refusing {len(changes)} changed objects')
    args.output.mkdir(parents=True, exist_ok=True)
    args.output = args.output.resolve()
    compile_flags = cache._compile_arguments(toolchain, args.generated, repo, args.include, sysroot)
    object_cache = args.baseline_archive.parent.parent / 'objects'
    objects = []
    print(json.dumps({'compile': [s.name for s in changes], 'reuse': len(sources) - len(changes)}), flush=True)
    for source, key in zip(sources, record['objects']):
        if source in changes:
            obj = args.output / (source.name + '.obj')
            subprocess.run([str(args.compiler), *compile_flags, f'/Fo{obj}', str(source.path)],
                           check=True, timeout=120, env=cache.build_environment() if sysroot else None)
        else:
            obj = object_cache / key[:2] / (key + '.obj')
            meta = json.loads(obj.with_suffix('.json').read_text())
            if digest(obj) != meta['sha256']:
                raise RuntimeError(f'Frozen object changed: {obj}')
        objects.append(obj)
    runtime = repo / 'tools/oot3d/native_game_runtime'
    a32 = repo / 'tools/oot3d/native_a32_runtime'
    wrapper = args.output / 'wrapper.obj'
    subprocess.run([str(args.compiler), '/nologo', '/MT', '/O2', '/EHsc', '/std:c++20', '/DNOMINMAX',
        *(f'/I{x}' for x in (args.generated, runtime, a32, a32 / 'upstream', args.include)),
        '/c', f'/Fo{wrapper}', str(runtime / 'triaevum_title_whole_aot_plugin.cpp')], check=True, timeout=120)
    dll = args.output / 'triaevum_title_aot.dll'
    response = args.output / 'link.rsp'
    response.write_text('\n'.join(f'"{v}"' for v in (wrapper, *objects, args.support)))
    print('Linking verified sparse pilot (2 workers, persistent ThinLTO cache)', flush=True)
    subprocess.run([str(args.compiler), '/nologo', '/LD', '/MT', f'/Fe{dll}', '@' + str(response),
        '-fuse-ld=lld', '/link', '/NOIMPLIB', '/OPT:REF', '/OPT:ICF', '/INCREMENTAL:NO', '/Brepro',
        '/threads:2', f'/lldltocache:{args.output.parent / "thinlto-cache"}',
        f'/MAP:{args.output / "title.map"}'], check=True, timeout=900)
    (args.output / 'build.json').write_text(json.dumps({
        'developer_only': True, 'common_header_exception': 'friend-only, no layout/method change',
        'dll_sha256': digest(dll), 'support_sha256': digest(args.support),
        'compiled_sources': [s.name for s in changes], 'reused_objects': len(sources) - len(changes),
        'elapsed_seconds': time.monotonic() - started,
    }, indent=2))
    print(str(dll), flush=True)


if __name__ == '__main__':
    main()
