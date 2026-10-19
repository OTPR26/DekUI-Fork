"""Exercise helper-generated readiness metadata with the desktop frontend validator."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import layout_migration
import setup_game_folder
from test_upgrade_game_resources import ResourceUpgradeTests


class SetupFrontendTests(unittest.TestCase):
    def validate(self, user, expected=True):
        result = subprocess.run([str(VALIDATOR), '--validate-setup', str(user)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if expected else 1, result.stderr)

    def fresh_setup(self, root):
        rom = root/'game.3ds'
        rom.write_bytes(b'fixture')
        with patch.object(setup_game_folder, 'validate'), \
             patch.object(setup_game_folder, 'extract',
                          side_effect=lambda rom, out: out.mkdir(parents=True)), \
             patch.object(setup_game_folder, 'build'):
            return setup_game_folder.setup(root, root, rom, copy_game=False)

    def test_fresh_setup_is_accepted_without_rewriting_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = self.fresh_setup(Path(tmp))
            ready = user/'setup/ready.json'
            before = ready.read_bytes()
            self.assertEqual(json.loads(before)['layout_version'], layout_migration.VERSION)
            self.validate(user)
            self.assertEqual(ready.read_bytes(), before)

    def test_upgrade_is_accepted_and_preserves_user_files(self):
        fixture = ResourceUpgradeTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        with fixture.builders():
            fixture.run_upgrade()
            before = fixture.snapshot()
            self.validate(fixture.user)
            fixture.run_upgrade()
            self.validate(fixture.user)
            self.assertEqual(fixture.snapshot(), before)
        fixture.assert_kept()

    def test_incomplete_or_incompatible_setup_is_still_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            user = self.fresh_setup(Path(tmp))
            ready = user/'setup/ready.json'
            original = json.loads(ready.read_bytes())
            for key, bad_values in {
                'version': (0, 2),
                'resource_version': (0, 1, 3),
                'layout_version': (0, 1, layout_migration.VERSION + 1),
                'rom_sha256': ('wrong', ''),
            }.items():
                for value in bad_values:
                    with self.subTest(key=key, value=value):
                        ready.write_text(json.dumps(dict(original, **{key: value})))
                        self.validate(user, expected=False)
            ready.write_text(json.dumps(original))
            pending = user/'setup/resource-upgrade-pending'
            pending.write_bytes(b'pending')
            self.validate(user, expected=False)
            pending.unlink()
            self.validate(user)
            ready.write_bytes(b'invalid json')
            self.validate(user, expected=False)
            ready.unlink()
            self.validate(user, expected=False)


if __name__ == '__main__':
    VALIDATOR = Path(sys.argv.pop(1)).resolve()
    # Imported fixture tests belong to their own suite.
    unittest.main(defaultTest='SetupFrontendTests')
