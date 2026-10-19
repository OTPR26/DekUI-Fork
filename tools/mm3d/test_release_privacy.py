import io
from pathlib import Path
import tempfile
import unittest
import zipfile
from check_release_privacy import (PUBLIC_RUNNER_HOME, PUBLIC_VENDOR_FILENAMES, CHUNK,
                                   inspect_archive, inspect_stream, patterns)


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
                archive.writestr(name, b'path: ' + PUBLIC_RUNNER_HOME + b'/.cache/bazel')
            self.assertEqual(inspect_archive(path, patterns([PUBLIC_RUNNER_HOME.decode()])), [])
            with zipfile.ZipFile(path, 'w') as archive:
                archive.writestr(name, b'path: ' + PUBLIC_RUNNER_HOME + b'/.cache/bazel\nprivate: ' + PUBLIC_RUNNER_HOME + b'/private')
            self.assertTrue(inspect_archive(path, patterns([PUBLIC_RUNNER_HOME.decode()])))

    def test_checker_sources_do_not_match_runner_home(self):
        forbidden = patterns([PUBLIC_RUNNER_HOME.decode()])
        for name in ('check_release_privacy.py', 'test_release_privacy.py'):
            with Path(__file__).with_name(name).open('rb') as stream:
                self.assertFalse(inspect_stream(stream, forbidden))

    def test_vendor_source_exception_is_scoped_and_preserves_private_checks(self):
        forbidden = patterns([PUBLIC_RUNNER_HOME.decode(), '/Users/example'])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'app.apk'
            for name, public_path in PUBLIC_VENDOR_FILENAMES.items():
                for member, contents, expected in (
                    (name, public_path, False),
                    ('other/file', public_path, True),
                    (name, public_path + b'/Users/example/private', True),
                    (name, public_path + PUBLIC_RUNNER_HOME + b'/private', True),
                    (name, public_path[:-1] + b'/private\0', True),
                ):
                    with self.subTest(member=member, expected=expected):
                        with zipfile.ZipFile(path, 'w') as archive:
                            archive.writestr(member, contents)
                        self.assertEqual(bool(inspect_archive(path, forbidden)), expected)
