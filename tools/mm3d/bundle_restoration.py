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


def copy_desktop_hud(repo, destination):
    source = repo/'src/android/app/src/main/assets/dekui-hud8x'
    spec = json.loads((source/'upgrade.json').read_bytes())
    texture = source/spec['texture']
    if hashlib.sha256(texture.read_bytes()).hexdigest() != spec['texture_sha256']:
        raise ValueError('Android HUD texture checksum mismatch')
    destination.mkdir(parents=True)
    for name in ('layout.json','play.json'):
        shutil.copy2(repo/'resources/mm3d/desktop-hud'/name, destination/name)
    shutil.copy2(source/'upgrade.json', destination/'upgrade.json')
    shutil.copy2(texture, destination/texture.name)

    controls = repo/'src/android/app/src/main/assets/dekui-pc-controls'
    control_spec = json.loads((controls/'provenance.json').read_bytes())
    if hashlib.sha256((controls/'code.bps').read_bytes()).hexdigest() != control_spec['patch_sha256']:
        raise ValueError('Desktop gameplay input patch checksum mismatch')
    shutil.copytree(controls, destination.parent/'desktop-controls')
