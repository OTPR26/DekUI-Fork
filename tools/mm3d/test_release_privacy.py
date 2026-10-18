import io
from pathlib import Path
import tempfile
import unittest
import zipfile
from check_release_privacy import CHUNK, inspect_archive, inspect_stream, patterns


class ReleasePrivacyTests(unittest.TestCase):
    def test_paths_across_chunks_and_encodings(self):
        forbidden = patterns(['/Users/example', 'C:\\Users\\example'])
        for raw in (b'/users/Example/project', 'C:\\Users\\Example\\project'.encode('utf-16-le')):
            self.assertTrue(inspect_stream(io.BytesIO(b'x' * (CHUNK - 4) + raw), forbidden))
        self.assertFalse(inspect_stream(io.BytesIO(b'./src/main.cpp'), forbidden))

    def test_zip_members_and_metadata_are_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'app.zip'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr('app/bin', b'/Users/example/src/main.cpp')
                archive.writestr('__MACOSX/._app', b'metadata')
            reasons = [reason for _, reason in inspect_archive(path, patterns(['/Users/example']))]
            self.assertIn('private path in file contents', reasons)
            self.assertIn('macOS metadata', reasons)

    def test_vendored_runner_cache_is_public_but_other_home_paths_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'source.zip'
            name = 'externals/catch2/.github/workflows/linux-bazel-builds.yml'
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr(name, b'path: /home/runner/.cache/bazel')
            self.assertEqual(inspect_archive(path, patterns(['/home/runner'])), [])
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr(name, b'path: /home/runner/.cache/bazel\nprivate: /home/runner/private')
            self.assertTrue(inspect_archive(path, patterns(['/home/runner'])))
