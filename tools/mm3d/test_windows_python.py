import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile
import windows_python as runtime


def fixture_archive(path):
    with zipfile.ZipFile(path, 'w') as archive:
        for name in runtime.REQUIRED:
            archive.writestr(name, 'official fixture ' + name)
    return hashlib.sha256(path.read_bytes()).hexdigest()


class WindowsPythonTests(unittest.TestCase):
    def test_arm64_runtime_selection_rejects_x64_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); archive = root / 'python.zip'
            digest = fixture_archive(archive)
            with patch.object(runtime, 'ARM64_SHA256', digest):
                runtime.install(archive, root / 'runtime', 'arm64')
                notice = (root / 'runtime/DEKUI-NOTICE.txt').read_text()
                self.assertIn('Windows arm64', notice)
                self.assertIn('embed-arm64.zip', notice)
            with patch.object(runtime, 'SHA256', digest):
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    runtime.verify(archive, 'arm64')
        with self.assertRaisesRegex(ValueError, 'Unsupported Windows architecture'):
            runtime.distribution('arm32')

    def test_installs_isolated_runtime_with_license(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); archive = root / 'python.zip'
            digest = fixture_archive(archive)
            with patch.object(runtime, 'SHA256', digest):
                runtime.install(archive, root / 'runtime/python')
                with self.assertRaisesRegex(ValueError, 'existing Python'):
                    runtime.install(archive, root / 'runtime/python')
            self.assertEqual((root / 'runtime/python' / runtime.PTH).read_text(),
                             'python313.zip\n.\n../../tools/mm3d\n')
            self.assertEqual((root / 'runtime/python/LICENSE.txt').read_text(),
                             'official fixture LICENSE.txt')
            self.assertIn(digest, (root / 'runtime/python/DEKUI-NOTICE.txt').read_text())

    def test_bad_hash_and_incomplete_archive_fail_before_extraction(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); archive = root / 'python.zip'
            fixture_archive(archive)
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                runtime.install(archive, root / 'runtime')
            self.assertFalse((root / 'runtime').exists())
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('python.exe', 'incomplete')
            with patch.object(runtime, 'SHA256', hashlib.sha256(archive.read_bytes()).hexdigest()):
                with self.assertRaisesRegex(ValueError, 'incomplete'):
                    runtime.install(archive, root / 'runtime')
            self.assertFalse((root / 'runtime').exists())
