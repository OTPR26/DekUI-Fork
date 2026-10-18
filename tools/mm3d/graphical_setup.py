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


def prepare(root, rom, patch=None, exheader=None, hud=None, in_place=False, upgrade_existing=False, reference_game=False):
    if (root/'user').exists() and not upgrade_existing:
        raise ValueError('This folder already has user data. Choose a fresh application folder; existing saves will not be replaced.')
    if not rom.is_file(): raise ValueError('Select your decrypted game file (.cci or .3ds).')
    validate(rom)
    bundled = RESOURCES/'project-restoration'
    patch = patch if patch is not None else bundled/'code.bps'
    exheader = exheader if exheader is not None else bundled/'exheader.bin'
    hud = hud if hud is not None else bundled/'romfs/layout'
    selected = {'code.bps': patch, 'exheader.bin': exheader}
    for name in ('Joker.Main.New.gar','Joker.Main.Play.gar','Joker.mfpj'):
        matches = list(hud.rglob(name)) if hud.is_dir() else []
        if len(matches) != 1:
            raise ValueError(f'HD HUD folder must contain exactly one {name}. Extract the complete Dekui package again.')
        selected['romfs/layout/'+name] = matches[0]
    expected = json.loads((RESOURCES/'setup-fingerprints.json').read_text())
    for entry in expected['files']:
        if entry['input_root'] != 'restoration': continue
        path = selected[entry['input']]
        if not path.is_file(): raise ValueError(f'Required {path.name} is missing. Extract the complete Dekui package again.')
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['input_sha256']:
            raise ValueError(f'{path.name} is not the supported version. Use the matching Restoration v110 standard-aiming patch and original HD HUD files, not already modified layouts.')
    with tempfile.TemporaryDirectory(prefix='.mm3d-inputs-',dir=root) as tmp:
        restoration=Path(tmp)
        for name,path in selected.items():
            target=restoration/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
        if (root/'user').exists():
            from upgrade_game_resources import upgrade
            result = upgrade(root,rom,restoration)
        else:
            result = setup(root,restoration,rom,copy_game=not in_place,reference_game=reference_game)
        from desktop_hud import apply
        apply(root/'user', RESOURCES)
        return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('root','rom'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('patch','exheader','hud'):parser.add_argument('--'+name,type=Path)
    parser.add_argument("--reference-game",action="store_true",help="Use an explicitly selected external game without copying it")
    parser.add_argument("--in-place",action="store_true",help="Use the game beside the app without copying it")
    parser.add_argument("--upgrade-existing",action="store_true",help="Upgrade owned resources while preserving user data")
    args=parser.parse_args()
    try:
        prepare(args.root,args.rom,args.patch,args.exheader,args.hud,in_place=args.in_place,upgrade_existing=args.upgrade_existing,reference_game=args.reference_game)
        print('Setup complete. Starting the game.')
    except (ValueError,OSError) as error:
        parser.exit(1,str(error)+'\n')
