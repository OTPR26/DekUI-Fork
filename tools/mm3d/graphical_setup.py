#!/usr/bin/env python3
"""Automatic first-launch setup; uses the supplied game and verified bundled Restoration/HUD files."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
from build_release_assets import RESOURCES
from setup_game_folder import setup, validate


def complete_setup(root, rom, restoration, **region):
    """Fill an unprepared user folder without replacing existing settings or saves."""
    user = root/'user'
    if user.is_symlink() or not user.is_dir():
        raise ValueError('The user folder must be a real directory; it was not changed.')
    with tempfile.TemporaryDirectory(prefix='.mm3d-recover-', dir=root) as tmp:
        prepared = setup(Path(tmp), restoration, rom, copy_game=False, reference_game=True, **region)
        files = sorted(p for p in prepared.rglob('*') if p.is_file())
        marker = prepared/'setup/ready.json'
        files.remove(marker)
        files.append(marker)  # Publish readiness only after every resource is installed.
        directories = sorted(p for p in prepared.rglob('*') if p.is_dir())
        missing = []
        for source in directories + files:
            relative = source.relative_to(prepared)
            target = user/relative
            for ancestor in (target, *target.parents):
                if ancestor.is_symlink():
                    raise ValueError('Setup refuses symbolic links in the user folder.')
                if ancestor == user:
                    break
            if target.exists():
                compatible = target.is_dir() if source.is_dir() else (
                    target.is_file() and (relative.parts[0] == 'config' or
                                         target.read_bytes() == source.read_bytes()))
                if not compatible:
                    raise ValueError('Existing game resources conflict with setup: '+relative.as_posix()+
                                     '. Existing data was not replaced.')
            else:
                missing.append((source, target))
        for source, target in missing:
            if source.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            # Exclusive creation also protects against another first-launch helper.
            with target.open('xb') as stream:
                stream.write(source.read_bytes())
    return user


def prepare(root, rom, patch=None, exheader=None, hud=None, in_place=False, upgrade_existing=False,
            reference_game=False, *, experimental_eur=False):
    if (root/'user').exists() and not upgrade_existing:
        raise ValueError('This folder already has user data. Choose a fresh application folder; existing saves will not be replaced.')
    if not rom.is_file(): raise ValueError('Select your decrypted game file (.cci or .3ds).')
    region = {'experimental_eur': True} if experimental_eur else {}
    fingerprint = validate(rom, **region)
    bundled = RESOURCES/'project-restoration'
    patch = patch if patch is not None else bundled/'code.bps'
    exheader = exheader if exheader is not None else bundled/'exheader.bin'
    hud = hud if hud is not None else bundled/'romfs/layout'
    selected = {'code.bps': patch, 'exheader.bin': exheader}
    for name in ('Joker.Main.New.gar','Joker.Main.Play.gar','Joker.mfpj'):
        matches = list(hud.rglob(name)) if hud.is_dir() else []
        if len(matches) != 1:
            raise ValueError(f'HD HUD folder must contain exactly one {name}. Extract the complete Majora ReMasked package again.')
        selected['romfs/layout/'+name] = matches[0]
    expected = json.loads((RESOURCES/'setup-fingerprints.json').read_text())
    for entry in expected['files']:
        if entry['input_root'] != 'restoration': continue
        path = selected[entry['input']]
        if not path.is_file(): raise ValueError(f'Required {path.name} is missing. Extract the complete Majora ReMasked package again.')
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['input_sha256']:
            raise ValueError(f'{path.name} is not the supported version. Use the matching Restoration v110 standard-aiming patch and original HD HUD files, not already modified layouts.')
    with tempfile.TemporaryDirectory(prefix='.mm3d-inputs-',dir=root) as tmp:
        restoration=Path(tmp)
        for name,path in selected.items():
            target=restoration/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        if (root/'user').exists() and not (root/'user/setup/ready.json').exists():
            result = complete_setup(root, rom, restoration, **region)
        elif (root/'user').exists():
            if experimental_eur:
                from upgrade_game_resources import RESOURCE_VERSION
                import layout_migration
                result = root/'user'
                ready = json.loads((result/'setup/ready.json').read_bytes())
                if (result.is_symlink() or ready.get('version') != 1 or
                    ready.get('program_id') != fingerprint['program_id'] or
                    ready.get('rom_sha256') != fingerprint['main_partition_sha256'] or
                    ready.get('resource_version') != RESOURCE_VERSION or
                    ready.get('layout_version') != layout_migration.VERSION or
                    (result/'setup/resource-upgrade-pending').exists()):
                    raise ValueError('Incompatible EUR preview setup. Existing saves were preserved.')
            else:
                from upgrade_game_resources import upgrade
                result = upgrade(root,rom,restoration)
        else:
            result = setup(root,restoration,rom,copy_game=not in_place,reference_game=reference_game, **region)
        from desktop_hud import apply, apply_controls
        apply_controls(root/'user', RESOURCES)
        apply(root/'user', RESOURCES)
        return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('root','rom'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('patch','exheader','hud'):parser.add_argument('--'+name,type=Path)
    parser.add_argument("--reference-game",action="store_true",help="Use an explicitly selected external game without copying it")
    parser.add_argument("--in-place",action="store_true",help="Use the game beside the app without copying it")
    parser.add_argument("--upgrade-existing",action="store_true",help="Upgrade owned resources while preserving user data")
    parser.add_argument("--experimental-eur", action="store_true", help="Prepare the isolated EUR Rev 1 preview")
    args=parser.parse_args()
    try:
        prepare(args.root,args.rom,args.patch,args.exheader,args.hud,in_place=args.in_place,upgrade_existing=args.upgrade_existing,reference_game=args.reference_game,experimental_eur=args.experimental_eur)
        print('Setup complete. Starting the game.')
    except (ValueError,OSError) as error:
        parser.exit(1,str(error)+'\n')
