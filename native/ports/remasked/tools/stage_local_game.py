#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage owned inputs into an isolated local prototype sandbox; never uploads."""
import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/oot3d/native_a32_runtime'))
from build_process_manifest import build_manifest

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    inputs, output = args.inputs.resolve(), args.destination.resolve()
    if output.is_relative_to(ROOT) or output == inputs:
        parser.error('The destination must be a separate private sandbox outside the checkout')
    code, header, romfs = [inputs / name for name in ['code.bin', 'exheader.bin', 'romfs.bin']]
    if digest(code) != '6eec27b1827eab2c87744ac38066d569ffe6fe75790b1eb3300bd91f5099038d':
        parser.error('Wrong MM3D code input')
    manifest = build_manifest(header, code, romfs)
    game = output / 'PrivateGame'
    game.mkdir(parents=True, exist_ok=True, mode=0o700)
    receipts = {}
    for source in [code, header, romfs]:
        destination = game / source.name
        expected = digest(source)
        if destination.exists():
            if digest(destination) != expected:
                parser.error('Existing private input differs; refusing overwrite: ' + source.name)
        else:
            shutil.copyfile(source, destination)
            destination.chmod(0o600)
        receipts[source.name] = expected
    for field in ['code_bin_path', 'exheader_path', 'romfs_image_path']:
        manifest['source'][field] = Path(manifest['source'][field]).name
    (game / 'process-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    for name in ['resources', 'saves', 'logs', 'savestates', 'textures/load', 'textures/dump']:
        (output / name).mkdir(parents=True, exist_ok=True)
    for name in ['presentation.json', 'runtime-presentation.json']:
        destination = output / name
        if not destination.exists():
            shutil.copyfile(ROOT / 'ports/remasked/resources/presentation.json', destination)
    profile = output / 'ReMaskedNativeGame.launch.json'
    if not profile.exists():
        data = '${profile_dir}'
        arguments = ['--a32-process-manifest', data + '/PrivateGame/process-manifest.json',
            '--resource-root', data + '/resources', '--renderer', 'vulkan',
            '--config', data + '/native-game.json', '--initialize-config', 'product',
            '--save-data', data + '/saves', '--log-directory', data + '/logs',
            '--crash-directory', data + '/logs/crashes', '--savestate-directory', data + '/savestates',
            '--texture-load-directory', data + '/textures/load', '--texture-dump-directory', data + '/textures/dump',
            '--title-id', '0004000000125500', '--width', '1280', '--height', '720',
            '--gameplay-timing', 'native30_no_interpolation', '--presentation-rate', '30',
            '--serial-presentation']
        profile.write_text(json.dumps({'format': 'oot3d_native_game_launch_profile_v1', 'arguments': arguments}, indent=2) + '\n')
    (output / 'private-input-receipt.json').write_text(json.dumps({'files': receipts,
        'source_read_only': True, 'existing_saves_and_settings_preserved': True, 'uploaded': False}, indent=2) + '\n')
    print('Owned inputs staged privately; existing saves and settings preserved.')

if __name__ == '__main__':
    main()
