"""Keep vendor build reports portable without modifying runtime files."""
from pathlib import Path
import tempfile
import unittest
from build_desktop_tp_runtime import sanitize_numpy_metadata


class NumpyMetadataTests(unittest.TestCase):
    def test_vendor_paths_removed_and_other_configuration_retained(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            config = root / 'tp-modules/numpy/__config__.py'
            config.parent.mkdir(parents=True)
            config.write_text('report = {"python": r"C:\\Users\\Builder\\Temp\\python.exe", '
                              '"include": r"C:/Users/Builder/Temp/include", '
                              '"version": "3.13", "compiler": "msvc"}\n')
            runtime = config.parent / 'keep.bin'
            runtime.write_bytes(b'unchanged runtime')
            sanitize_numpy_metadata(root)
            source = config.read_text()
            self.assertNotIn('Builder', source)
            values = {}
            exec(compile(source, '__config__.py', 'exec'), values)
            self.assertEqual(values['report']['version'], '3.13')
            self.assertEqual(values['report']['compiler'], 'msvc')
            self.assertEqual(runtime.read_bytes(), b'unchanged runtime')
