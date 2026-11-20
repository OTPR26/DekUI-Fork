#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build private, immutable Apple app resources from verified owner content. Never uploads."""
import argparse
import hashlib
import json
import shutil
import tarfile
from pathlib import Path

TITLE = '0004000000125500'
SAVE_NAMES = ('save0.bin', 'save1.bin', 'save2.bin')
CHECKOUT = Path(__file__).resolve().parents[3]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inventory(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError('Expected a real content directory')
    files = {}
    for path in root.rglob('*'):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ValueError('Links and special files are forbidden')
        if path.is_file():
            files[path.relative_to(root).as_posix()] = sha(path)
    return files


def build(owner, output):
    owner, output = owner.resolve(), output.resolve()
    if output.exists() or output.is_relative_to(CHECKOUT) or output.is_relative_to(owner) or owner.is_relative_to(output):
        raise ValueError('Choose a fresh private destination outside the source checkout and owner inputs')
    expected = json.loads((owner / 'henriko-file-hashes.json').read_text())
    pack = owner / 'henriko-mm3d-mobile'
    if inventory(pack) != expected:
        raise ValueError('Texture content differs from the verified AYN copy')
    saves = owner / 'ayn-current-saves'
    save_hashes = json.loads((owner / 'ayn-save-hashes.json').read_text())
    if set(save_hashes) != set(SAVE_NAMES) or inventory(saves) != save_hashes:
        raise ValueError('Current save content differs from the verified AYN copy')
    if any((saves / name).stat().st_size != 79384 for name in SAVE_NAMES):
        raise ValueError('Unexpected MM3D save size')
    # Validate the entire archive before extracting any entry.
    with tarfile.open(owner / 'current-hud-overlay.tar') as archive:
        entries = archive.getmembers()
        for entry in entries:
            path = Path(entry.name)
            if path.is_absolute() or '..' in path.parts or not (entry.isfile() or entry.isdir()):
                raise ValueError('Unsafe HUD overlay archive')
        output.mkdir(parents=True, mode=0o700)
        overlay = output / 'HudOverlay'
        overlay.mkdir()
        archive.extractall(overlay, filter='data')
    shutil.copytree(pack, output / 'HenrikoMobile')
    shutil.copytree(saves, output / 'InitialSaves')
    hashes = inventory(output)
    manifest = {'schema': 1, 'title_id': TITLE, 'publishable': False,
                'texture_precedence': ['HudOverlay', 'HenrikoMobile'],
                'initial_saves': save_hashes, 'files': hashes}
    (output / 'owner-content.json').write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--owner-content', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    result = build(args.owner_content, args.destination)
    print(f"Private app resources prepared: {len(result['files'])} verified files; HUD takes precedence; 3 initial saves.")


if __name__ == '__main__':
    main()
