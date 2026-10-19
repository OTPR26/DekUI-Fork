import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from tp_import.hashes import xxh64
from tp_import.disc import yaz


class TextureHashTests(unittest.TestCase):
    def test_dolphin_hash_vectors(self):
        self.assertEqual(xxh64(b''), 'EF46DB3751D8E999')
        self.assertEqual(xxh64(b'abc'), '44BC2CF5AD770999')

    def test_invalid_archive_back_reference(self):
        data = b'Yaz0' + (3).to_bytes(4, 'big') + bytes(8) + bytes([0, 0x10, 0])
        with self.assertRaises(ValueError):
            yaz(data)


class TwilightInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            from tp_import import generate
        except ImportError as error:
            raise unittest.SkipTest(str(error))
        cls.module = generate

    def test_incomplete_generation_preserves_other_models(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            parent = root/'load/mm3d-models'
            previous = parent/'twilight-link/keep'
            adult = parent/'adult-link/keep'
            for path in (previous, adult):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'previous')
            stage = parent/'.twilight-link-test'
            stage.mkdir()
            with self.assertRaises(ValueError):
                self.module.install(root, stage)
            self.assertEqual(previous.read_bytes(), b'previous')
            self.assertEqual(adult.read_bytes(), b'previous')

    def test_successful_install_keeps_previous_generation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            parent = root/'load/mm3d-models'
            previous = parent/'twilight-link/keep'
            previous.parent.mkdir(parents=True)
            previous.write_bytes(b'previous')
            stage = parent/'.twilight-link-test'
            for variant in ('kokiri', 'master'):
                path = stage/'variants'/variant/'romfs/actors/zelda2_link_child_new.gar.lzs'
                path.parent.mkdir(parents=True)
                path.write_bytes(b'test fixture')
            path = stage/'voices/romfs/sound/sequence/JokerSound.bcsar'
            path.parent.mkdir(parents=True)
            path.write_bytes(b'test fixture')
            (stage/'build').mkdir()
            (stage/'texture-manifest.json').write_text('[]')
            self.module.install(root, stage)
            self.assertEqual((parent/'.twilight-link-previous/keep').read_bytes(), b'previous')
            self.assertTrue((parent/'twilight-link/variants/master/romfs/actors/zelda2_link_child_new.gar.lzs').is_file())

    def test_failed_disc_open_removes_stage_and_keeps_sources(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with patch.object(self.module, 'Disc', side_effect=ValueError('invalid disc')):
                with self.assertRaises(ValueError):
                    self.module.generate(root, 'mm', 'tp', 'library')
            self.assertEqual(list((root/'load/mm3d-models').iterdir()), [])


class ReplacementTests(unittest.TestCase):
    def test_duplicates_choose_largest_matching_texture(self):
        try:
            from PIL import Image
            from tp_import.replacements import prepare
        except ImportError as error:
            self.skipTest(str(error))
        def png(size, color):
            buffer = io.BytesIO()
            Image.new('RGBA', size, color).save(buffer, format='PNG')
            return buffer.getvalue()
        reader = Mock()
        reader.read.side_effect = lambda uri: png((32, 32), 'blue') if uri == 'large' else png((16, 16), 'red')
        library = Mock()
        library.dekui_tp_texture_hash.return_value = 1
        library.dekui_tp_legacy_texture_hash.return_value = 2
        with tempfile.TemporaryDirectory() as folder:
            stage = Path(folder)
            (stage/'raw.bin').write_bytes(b'raw fixture')
            (stage/'texture-manifest.json').write_text(json.dumps([dict(file='raw.bin', source_hash='ABC', width=8, height=8)]))
            with patch('tp_import.replacements.c.CDLL', return_value=library):
                result = json.loads(prepare(stage, json.dumps({'ABC':['small', 'large']}), 'library', reader))
            self.assertEqual(result, {'matched':1, 'duplicate_hashes':1})
            path = stage/'texture-overrides/tex1_8x8_0000000000000001_13_mip0.png'
            with Image.open(path) as image:
                self.assertEqual(image.size, (32, 32))
                self.assertEqual(image.getpixel((0, 0)), (0, 0, 255, 255))
