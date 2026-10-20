"""Run the actual packaged helper on Windows without relying on installed Python."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


def check(package):
    if os.name != 'nt':
        raise RuntimeError('Packaged Windows runtime smoke check must run on Windows')
    with tempfile.TemporaryDirectory(prefix='Dekui setup smoke ') as tmp:
        with zipfile.ZipFile(package) as archive:
            archive.extractall(tmp)
        root = Path(tmp) / 'dekui'
        subprocess.run([str(root / 'Majora-ReMasked.exe'), '--version'],
                       cwd=tmp, check=True, timeout=30)
        python = root / 'runtime/python/python.exe'
        env = os.environ.copy()
        env['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32')
        env['PYTHONHOME'] = str(root / 'invalid-system-python')
        env['PYTHONPATH'] = str(root / 'invalid-system-modules')
        payload = root
        subprocess.run([str(python), '-E', '-s', '-B', str(payload / 'tools/mm3d/generate_tp_mod.py'),
                        '--package', str(payload), '--probe-runtime'], cwd=tmp, env=env, check=True, timeout=60)
        subprocess.run([str(python), '-E', '-s', '-B', str(payload / 'tools/mm3d/import_pak.py'),
                        '--package', str(payload), '--probe-runtime'], cwd=tmp, env=env, check=True, timeout=60)
        subprocess.run([str(python), '-B', '-c',
                        'import sys, graphical_setup, hashlib, tempfile, json, shutil, zipfile; '
                        'assert sys.flags.isolated and sys.flags.ignore_environment; '
                        'assert graphical_setup.RESOURCES.is_dir()'],
                       cwd=tmp, env=env, check=True, timeout=30)
        subprocess.run([str(python), '-E', '-s', '-B', str(root / 'tools/mm3d/package_setup_check.py')], cwd=tmp, check=True, timeout=30)
        helper = root / 'tools/mm3d/graphical_setup.py'
        subprocess.run([str(python), '-B', str(helper), '--help'],
                       cwd=tmp, env=env, check=True, timeout=30)
        subprocess.run([str(python), '-B', str(root / 'tools/mm3d/mod_import/generate.py'), '--help'],
                       cwd=tmp, env=env, check=True, timeout=30)
        result = subprocess.run([str(python), '-B', str(helper), '--root', str(root),
                                 '--rom', str(root / 'missing.cci'), '--in-place'],
                                cwd=tmp, env=env, capture_output=True, text=True, timeout=30)
        if result.returncode != 1 or 'Select your decrypted game file' not in result.stderr:
            raise RuntimeError(f'Packaged helper failed input validation smoke check: {result.stderr}')
        if (root / 'user').exists():
            raise RuntimeError('Invalid input unexpectedly created user data')
        print('Packaged setup works without system Python; isolation and error handling passed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    check(parser.parse_args().package)
