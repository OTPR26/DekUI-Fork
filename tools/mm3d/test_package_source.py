"""Exercise actual gzip streaming before starting the expensive CI build."""
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from package_source import package


class SourcePackageTests(unittest.TestCase):
    def test_archive_producer_finishes_with_trailing_padding(self):
        # A tar reader stops at the archive terminator, before pipe EOF.
        # Oversized padding models git archive blocked on a small Windows pipe.
        producer = '''
import io, sys, tarfile
with tarfile.open(fileobj=sys.stdout.buffer, mode='w|') as archive:
    entry = tarfile.TarInfo('source.txt')
    entry.size = 7
    archive.addfile(entry, io.BytesIO(b'fixture'))
sys.stdout.buffer.write(bytes(2 * 1024 * 1024))
sys.stdout.buffer.flush()
'''
        runner = '''
import subprocess, sys
from pathlib import Path
from unittest.mock import patch
from package_source import package
real_popen = subprocess.Popen
def padded_archive(*args, **kwargs):
    return real_popen([sys.executable, '-c', sys.argv[1]], **kwargs)
with patch('package_source.subprocess.check_output', return_value=''), \\
     patch('package_source.subprocess.Popen', side_effect=padded_archive):
    package(Path(sys.argv[2]))
'''
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'source.tar.gz'
            env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parent))
            subprocess.run([sys.executable, '-c', runner, producer, str(output)],
                           cwd=tmp, env=env, check=True, timeout=15,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            with tarfile.open(output) as archive:
                self.assertEqual(archive.extractfile('source.txt').read(), b'fixture')

    def test_committed_source_and_submodule(self):
        def git(root, *args):
            subprocess.run(['git', *args], cwd=root, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo, dep, nested = root / 'repo', root / 'dep', root / 'nested'
            for path in (repo, dep, nested):
                path.mkdir()
                git(path, 'init')
                git(path, 'config', 'user.name', 'Fixture')
                git(path, 'config', 'user.email', 'fixture@example.invalid')
                (path / 'source.txt').write_text(path.name)
                (path / 'HANDOFF.md').write_text('internal notes' if path==repo else 'dependency documentation')
                (path / 'DEVELOPMENT.md').write_text('working notes' if path==repo else 'dependency build instructions')
                (path / 'LICENSE.txt').write_text('license fixture')
                git(path, 'add', '.')
                git(path, 'commit', '-m', 'Fixture')
            git(dep, '-c', 'protocol.file.allow=always', 'submodule', 'add',
                str(nested), 'nested module')
            git(dep, 'commit', '-am', 'Add nested dependency')
            git(repo, '-c', 'protocol.file.allow=always', 'submodule', 'add', str(dep), 'externals/dep')
            git(repo, '-c', 'protocol.file.allow=always', 'submodule', 'update', '--init', '--recursive')
            git(repo, 'commit', '-am', 'Add dependency')
            (repo / 'private.cci').write_text('not for distribution')
            previous = Path.cwd()
            archive_path = root / 'source.tar.gz'
            try:
                os.chdir(repo)
                package(archive_path)
                with self.assertRaises(ValueError):
                    package(archive_path)
            finally:
                os.chdir(previous)
            with tarfile.open(archive_path) as archive:
                self.assertEqual(archive.extractfile('source.txt').read(), b'repo')
                self.assertEqual(archive.extractfile('externals/dep/source.txt').read(), b'dep')
                self.assertEqual(archive.extractfile('externals/dep/nested module/source.txt').read(), b'nested')
                self.assertNotIn('private.cci', archive.getnames())
                self.assertNotIn('HANDOFF.md', archive.getnames())
                self.assertNotIn('DEVELOPMENT.md', archive.getnames())
                self.assertIn('externals/dep/HANDOFF.md', archive.getnames())
                self.assertIn('externals/dep/nested module/DEVELOPMENT.md', archive.getnames())
                self.assertIn('LICENSE.txt', archive.getnames())
                self.assertIn('externals/dep/LICENSE.txt', archive.getnames())
                self.assertFalse(any('.git' in Path(n).parts for n in archive.getnames()))


if __name__ == '__main__':
    unittest.main()
