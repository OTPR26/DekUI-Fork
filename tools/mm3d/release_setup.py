#!/usr/bin/env python3
"""Private baseline capture and deterministic staging. Never installs into live user data.
Generated recipes may contain game-derived bytes: do not publish without review.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe(root, name):
    p = PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or '\\' in name or ':' in name:
        raise ValueError(f'Unsafe relative path: {name}')
    result = root.joinpath(*p.parts)
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError(f'Path escapes root: {name}')
    return result


def write(root, name, data):
    p = safe(root, name)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


def edits(source, target):
    changes = []
    i = 0
    while i < len(target):
        if i < len(source) and source[i] == target[i]:
            i += 1
            continue
        start = i
        while i < len(target) and (i >= len(source) or source[i] != target[i]):
            i += 1
        changes.append([start, base64.b64encode(target[start:i]).decode('ascii')])
    return changes


def inputs(roots):
    result = []
    for label, root in roots.items():
        for p in sorted(root.rglob('*')):
            if p.is_file() and p.suffix in ('.gar', '.mfpj', '.bps', '.bin'):
                result.append((label, p.relative_to(root).as_posix(), p))
    return result


def capture(args, stage):
    roots = {'original': args.original, 'restoration': args.restoration}
    candidates = inputs(roots)
    recipe = {'version': 1, 'private': True, 'title_id': '0004000000125500',
              'files': [], 'settings': []}
    # Include all runtime mod files and source baselines; historical backups are excluded.
    for p in sorted(args.mods.rglob('*')):
        if not p.is_file() or not (p.suffix in ('.gar', '.mfpj', '.bps', '.bin')
                                  or p.name.endswith('-source')):
            continue
        target = p.read_bytes()
        basename = p.name.split('.mm3d-')[0]
        choices = [x for x in candidates if Path(x[1]).name == basename]
        if not choices:
            raise ValueError(f'No original/Restoration source for {p.name}')
        scored = []
        for label, name, source_path in choices:
            source = source_path.read_bytes()
            delta = edits(source, target)
            scored.append((sum(len(v) + 16 for _, v in delta), label, name, source, delta))
        _, label, name, source, delta = min(scored, key=lambda x: x[0])
        rel = p.relative_to(args.mods).as_posix()
        recipe['files'].append({'output': rel, 'input_root': label, 'input': name,
                                'input_sha256': digest(source), 'output_sha256': digest(target),
                                'size': len(target), 'edits': delta})
        write(stage, 'baseline/mods/' + rel, target)
    # Match the profile folders used by this installation, excluding diagnostic backups.
    selected = [args.config / 'presentation.json', args.config / 'notebook.json']
    for folder in ('title-screen', 'notebook', 'hud'):
        selected.extend(sorted((args.config / folder).rglob('*.json')))
    for p in selected:
        if not p.is_file():
            continue
        rel = p.relative_to(args.config).as_posix()
        raw = p.read_bytes()
        obj = json.loads(raw)
        portable = dict(obj)
        if rel == 'notebook.json':
            for key in ('source_archive', 'output_archive', 'fade_source'):
                portable[key] = ''
        payload = (json.dumps(portable, indent=2, sort_keys=True) + '\n').encode()
        recipe['settings'].append({'path': rel, 'sha256': digest(payload)})
        write(stage, 'defaults/' + rel, payload)
        write(stage, 'baseline/config/' + rel, raw)
    if not recipe['files'] or not recipe['settings']:
        raise ValueError('Empty baseline')
    write(stage, 'recipe.json', (json.dumps(recipe, indent=2) + '\n').encode())


def replay(args, stage):
    recipe = json.loads((args.recipe / 'recipe.json').read_text())
    if recipe['version'] != 1:
        raise ValueError('Unsupported recipe version')
    roots = {'original': args.original, 'restoration': args.restoration}
    for entry in recipe['files']:
        source = safe(roots[entry['input_root']], entry['input']).read_bytes()
        if digest(source) != entry['input_sha256']:
            raise ValueError('Unsupported input: ' + entry['input'])
        result = bytearray(source[:entry['size']])
        result.extend(b'\0' * (entry['size'] - len(result)))
        end = 0
        for offset, payload in entry['edits']:
            data = base64.b64decode(payload, validate=True)
            if offset < end or offset + len(data) > len(result):
                raise ValueError('Invalid patch range')
            result[offset:offset + len(data)] = data
            end = offset + len(data)
        if digest(result) != entry['output_sha256']:
            raise ValueError('Output mismatch: ' + entry['output'])
        write(stage, 'load/mods/' + recipe['title_id'] + '/' + entry['output'], result)
    for entry in recipe['settings']:
        data = safe(args.recipe / 'defaults', entry['path']).read_bytes()
        if digest(data) != entry['sha256']:
            raise ValueError('Settings mismatch: ' + entry['path'])
        if entry['path'] == 'notebook.json':
            obj = json.loads(data)
            # Bind to user-supplied extracted inputs, never this developer's home path.
            obj['source_archive'] = str((args.original / 'romfs/layout/Joker.Main.Play.SubMenu.gar').resolve())
            obj['fade_source'] = str((args.original / 'romfs/layout/Joker.gar').resolve())
            data = (json.dumps(obj, indent=2, sort_keys=True) + '\n').encode()
        write(stage, 'config/mm3d-local/' + entry['path'], data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('capture', 'replay'))
    for flag in ('original', 'restoration', 'output'):
        parser.add_argument('--' + flag, type=Path, required=True)
    for flag in ('mods', 'config', 'recipe'):
        parser.add_argument('--' + flag, type=Path)
    args = parser.parse_args()
    required = ('mods', 'config') if args.mode == 'capture' else ('recipe',)
    for name in ('original', 'restoration') + required:
        value = getattr(args, name)
        if value is None or not value.is_dir():
            parser.error('--' + name + ' must be an existing directory')
    if args.output.exists():
        parser.error('Output already exists; refusing to overwrite')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.mm3d-stage-', dir=args.output.parent))
    try:
        (capture if args.mode == 'capture' else replay)(args, stage)
        stage.rename(args.output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print('Verified staging created:', args.output)


if __name__ == '__main__':
    main()
