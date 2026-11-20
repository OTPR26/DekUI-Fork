# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import io
import json
import tarfile
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('owner_content', Path(__file__).parents[1] / 'tools/package_owner_content.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class OwnerContentTests(unittest.TestCase):
    def fixture(self, root):
        owner = root / 'owner'
        pack = owner / 'henriko-mm3d-mobile'
        saves = owner / 'ayn-current-saves'
        pack.mkdir(parents=True)
        saves.mkdir()
        (pack / 'pack.json').write_text('{"options":{"use_new_hash":true}}')
        (pack / 'texture.png').write_bytes(b'verified texture fixture')
        for name in module.SAVE_NAMES:
            (saves / name).write_bytes(b's' * 79384)
        (owner / 'henriko-file-hashes.json').write_text(json.dumps(module.inventory(pack)))
        (owner / 'ayn-save-hashes.json').write_text(json.dumps(module.inventory(saves)))
        with tarfile.open(owner / 'current-hud-overlay.tar', 'w') as archive:
            item = tarfile.TarInfo('HUD/texture.png')
            item.size = 3
            archive.addfile(item, io.BytesIO(b'hud'))
        return owner

    def test_private_bundle_preserves_all_inputs_and_hud_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            owner = self.fixture(root)
            before = module.inventory(owner)
            result = module.build(owner, root / 'bundle')
            self.assertFalse(result['publishable'])
            self.assertEqual(result['texture_precedence'], ['HudOverlay', 'HenrikoMobile'])
            self.assertEqual(before, module.inventory(owner))
            for name in module.SAVE_NAMES:
                self.assertEqual((root / 'bundle/InitialSaves' / name).read_bytes(), (owner / 'ayn-current-saves' / name).read_bytes())
            with self.assertRaises(ValueError):
                module.build(owner, root / 'bundle')

    def test_changed_save_refused_before_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            owner = self.fixture(root)
            (owner / 'ayn-current-saves/save0.bin').write_bytes(b'changed')
            with self.assertRaises(ValueError):
                module.build(owner, root / 'bundle')
            self.assertFalse((root / 'bundle').exists())

    def test_archive_traversal_refused_before_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            owner = self.fixture(root)
            with tarfile.open(owner / 'current-hud-overlay.tar', 'w') as archive:
                item = tarfile.TarInfo('../escaped')
                item.size = 1
                archive.addfile(item, io.BytesIO(b'x'))
            with self.assertRaises(ValueError):
                module.build(owner, root / 'bundle')
            self.assertFalse((root / 'bundle').exists())


if __name__ == '__main__':
    unittest.main()
