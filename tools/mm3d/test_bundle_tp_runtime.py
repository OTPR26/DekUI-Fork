"""Package completeness checks for the isolated TP converter."""
from pathlib import Path
import shutil
import tempfile
import unittest
from bundle_tp_runtime import copy_tp_runtime


def fixture_runtime(repo):
    source = Path(__file__).resolve().parent
    stage = repo / 'build-tp-runtime/payload/runtime'
    for name in ('tp/libnod.so', 'tp-modules/numpy/__init__.py',
                 'tp-modules/numpy/_core/_multiarray_umath.fixture.so'):
        path = stage / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'fixture')
    scripts = repo / 'tools/mm3d'
    scripts.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source / 'tp_import', scripts / 'tp_import', dirs_exist_ok=True)
    shutil.copy2(source / 'generate_tp_mod.py', scripts / 'generate_tp_mod.py')
    shutil.copytree(source / 'zobj_import', scripts / 'zobj_import', dirs_exist_ok=True)
    shutil.copy2(source / 'import_pak.py', scripts / 'import_pak.py')


class ConverterBundleTests(unittest.TestCase):
    def test_missing_native_extension_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder) / 'repo'
            fixture_runtime(repo)
            next((repo / 'build-tp-runtime').rglob('*.fixture.so')).unlink()
            with self.assertRaisesRegex(ValueError, 'native extension'):
                copy_tp_runtime(repo, Path(folder) / 'out')

    def test_missing_reader_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder) / 'repo'
            fixture_runtime(repo)
            (repo / 'build-tp-runtime/payload/runtime/tp/libnod.so').unlink()
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                copy_tp_runtime(repo, Path(folder) / 'out')

    def test_copies_only_staged_runtime_and_converter_sources(self):
        with tempfile.TemporaryDirectory() as folder:
            repo = Path(folder) / 'repo'
            fixture_runtime(repo)
            (repo / 'private.rom').write_bytes(b'private')
            output = Path(folder) / 'out'
            copy_tp_runtime(repo, output)
            self.assertTrue((output / 'runtime/tp/libnod.so').is_file())
            self.assertTrue((output / 'tools/mm3d/tp_import/generate.py').is_file())
            self.assertTrue((output / 'tools/mm3d/zobj_import/install.py').is_file())
            self.assertTrue((output / 'tools/mm3d/zobj_import/vendor/LICENSE').is_file())
            self.assertTrue((output / 'tools/mm3d/import_pak.py').is_file())
            self.assertFalse((output / 'private.rom').exists())
