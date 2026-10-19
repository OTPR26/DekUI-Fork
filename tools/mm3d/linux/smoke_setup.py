"""Load the extracted Linux converter using only its bundled Python runtime."""
import argparse
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def check(package):
    with tempfile.TemporaryDirectory(prefix='Dekui importer smoke ') as folder:
        with tarfile.open(package) as archive:
            archive.extractall(folder, filter='data')
        root = Path(folder) / 'dekui'
        python = root / 'runtime/tp-python/python/bin/python3'
        env = dict(os.environ, PATH='/usr/bin:/bin', PYTHONHOME='/nonexistent', PYTHONPATH='/nonexistent')
        subprocess.run([str(python), '-E', '-s', '-B', str(root / 'tools/mm3d/package_setup_check.py')],
                       cwd=folder, env=env, check=True, timeout=30)
        subprocess.run([str(python), '-E', '-s', '-B', str(root / 'tools/mm3d/generate_tp_mod.py'),
                        '--package', str(root), '--probe-runtime'],
                       cwd=folder, env=env, check=True, timeout=60)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    check(parser.parse_args().package)
