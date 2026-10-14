import configparser
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from setup_game_folder import setup, validate, extract


class GameFolderTests(unittest.TestCase):
    def test_setup_creates_empty_texture_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);rom=root/'original.cci';rom.write_bytes(b'original')
            with patch('setup_game_folder.validate'), patch('setup_game_folder.extract',
                    side_effect=lambda rom,out:out.mkdir(parents=True)), patch('setup_game_folder.build'):
                setup(root,root,rom)
            textures=root/'user/load/textures/0004000000125500'
            self.assertTrue(textures.is_dir())
            self.assertEqual(list(textures.iterdir()),[])
            self.assertEqual(rom.read_bytes(),b'original')
            config = configparser.ConfigParser()
            config.read(root/'user/config/qt-config.ini')
            for key in ('use_hw_shader', 'async_shader_compilation'):
                self.assertFalse(config.getboolean('Renderer', key))
                self.assertFalse(config.getboolean('Renderer', key + '\\default'))
            self.assertEqual(config.getint('Renderer', 'graphics_api'), 2)


    def test_missing_rom(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaises(OSError): setup(Path(t),Path(t))
            self.assertFalse((Path(t)/'user').exists())

    def test_existing_user_preserved(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);(root/'user').mkdir();save=root/'user/save';save.write_bytes(b'keep')
            with patch('setup_game_folder.validate'), self.assertRaises(ValueError): setup(root,root)
            self.assertEqual(save.read_bytes(),b'keep')

    def test_wrong_revision(self):
        with patch('setup_game_folder.probe',return_value={'program_id':'wrong'}):
            with self.assertRaises(ValueError):validate(Path('mm3d.cci'))

    def test_truncated_extraction(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);rom=root/'mm3d.cci';rom.write_bytes(b'NCSD')
            with self.assertRaises(ValueError):extract(rom,root/'output')
            self.assertFalse((root/'output').exists())


if __name__=='__main__':unittest.main()
