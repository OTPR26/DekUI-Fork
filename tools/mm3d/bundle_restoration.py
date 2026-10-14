"""Copy only the pinned official Restoration release and its source/notices."""
import hashlib
import json
import shutil

FILES = ('code.bps', 'exheader.bin', 'LICENSE.md', 'NOTICE.txt', 'SOURCE.tar.gz', 'manifest.json',
         'romfs/layout/Joker.Main.New.gar', 'romfs/layout/Joker.Main.Play.gar', 'romfs/layout/Joker.mfpj')


def copy_restoration(repo, destination):
    source = repo / 'resources/mm3d/project-restoration'
    manifest = json.loads((source / 'manifest.json').read_text())
    fingerprints = json.loads((repo / 'resources/mm3d/setup-fingerprints.json').read_text())
    for name in FILES:
        path = source / name
        if not path.is_file() or path.is_symlink():
            raise ValueError(f'Missing bundled Restoration file: {name}')
        if name != 'manifest.json':
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != manifest['sha256'][name]:
                raise ValueError(f'Bundled Restoration checksum mismatch: {name}')
            if name in ('code.bps', 'exheader.bin') or name.startswith('romfs/layout/'):
                expected = next(e['input_sha256'] for e in fingerprints['files']
                                if e['input_root'] == 'restoration' and e['input'] == name)
                if digest != expected:
                    raise ValueError(f'Bundled Restoration is incompatible with setup: {name}')
    destination.mkdir(parents=True)
    for name in FILES:
        (destination / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / name, destination / name)
