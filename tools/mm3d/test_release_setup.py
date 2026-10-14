import argparse
import json
from pathlib import Path
import tempfile
import unittest
from release_setup import capture, replay, safe


class SetupTests(unittest.TestCase):
    def test_replay_and_rejections(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('original', 'restoration', 'mods', 'config', 'recipe', 'stage'):
                (root / name).mkdir()
            source = root / 'original/test.gar'
            source.write_bytes(b'abcdef')
            (root / 'mods/test.gar').write_bytes(b'abXXefLONG')
            (root / 'config/presentation.json').write_text('{"game_camera_fov":65}')
            (root / 'config/notebook.json').write_text('{"source_archive":"/private/local"}')
            args = argparse.Namespace(**{name: root / name for name in
                ('original', 'restoration', 'mods', 'config', 'recipe')})
            capture(args, args.recipe)
            replay(args, root / 'stage')
            self.assertEqual((root / 'stage/load/mods/0004000000125500/test.gar').read_bytes(), b'abXXefLONG')
            self.assertEqual(source.read_bytes(), b'abcdef')
            self.assertNotIn('/private/local', (args.recipe / 'defaults/notebook.json').read_text())
            source.write_bytes(b'wrong!')
            with self.assertRaisesRegex(ValueError, 'Unsupported input'):
                replay(args, root / 'stage')
            source.write_bytes(b'abcdef')
            recipe = args.recipe / 'recipe.json'
            obj = json.loads(recipe.read_text())
            obj['files'][0]['output_sha256'] = 'bad'
            recipe.write_text(json.dumps(obj))
            with self.assertRaisesRegex(ValueError, 'Output mismatch'):
                replay(args, root / 'stage')

    def test_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            for path in ('../escape', '/absolute', 'C:/windows', 'a\\b'):
                with self.assertRaises(ValueError):
                    safe(Path(tmp), path)


if __name__ == '__main__':
    unittest.main()
