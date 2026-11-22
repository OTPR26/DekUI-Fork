"""Check the extracted package without relying on a system Python installation."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def check(package):
    with tempfile.TemporaryDirectory(prefix='Dekui setup smoke ') as tmp:
        subprocess.run(['ditto', '-x', '-k', str(package.resolve()), tmp], check=True)
        root = Path(tmp) / 'dekui'
        python = root / 'Majora ReMasked.app/Contents/Resources/runtime/python/bin/python3'
        subprocess.run([str(python), '-E', '-s', '-B', str(root / 'Majora ReMasked.app/Contents/Resources/tools/mm3d/package_setup_check.py')], cwd=tmp, check=True, timeout=30)
        helper = root / 'Majora ReMasked.app/Contents/Resources/tools/mm3d/graphical_setup.py'
        env = os.environ.copy()
        env.update(PATH='/usr/bin:/bin', PYTHONHOME='/nonexistent-python',
                   PYTHONPATH='/nonexistent-modules')
        payload = root / 'Majora ReMasked.app/Contents/Resources'
        subprocess.run([str(python), '-E', '-s', '-B', str(payload / 'tools/mm3d/generate_tp_mod.py'),
                        '--package', str(payload), '--probe-runtime'], cwd=tmp, env=env, check=True, timeout=60)
        subprocess.run([str(python), '-E', '-s', '-B', str(payload / 'tools/mm3d/import_pak.py'),
                        '--package', str(payload), '--probe-runtime'], cwd=tmp, env=env, check=True, timeout=60)
        subprocess.run([str(python), '-E', '-s', '-B', str(helper), '--help'],
                       cwd=tmp, env=env, check=True, timeout=30)
        result = subprocess.run([str(python), '-E', '-s', '-B', str(helper),
                                 '--root', str(root), '--rom', str(root / 'missing.cci'), '--in-place'],
                                cwd=tmp, env=env, capture_output=True, text=True, timeout=30)
        if result.returncode != 1 or 'Select your decrypted game file' not in result.stderr:
            raise RuntimeError(f'Packaged helper failed validation smoke check: {result.stderr}')
        if (root / 'user').exists():
            raise RuntimeError('Invalid input unexpectedly created user data')
        # Load the actual deployed binary and Qt platform plugin, without a ROM.
        app = root / 'Majora ReMasked.app'
        env.update(QT_QPA_PLATFORM='cocoa', HOME=str(root / 'smoke-home'))
        result = subprocess.run([str(app / 'Contents/MacOS/azahar'), '-v'],
                                cwd=tmp, env=env, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            raise RuntimeError(f'Packaged app did not load: {result.stderr}')
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
        print('Extracted app loads; setup runs without system Python and rejects invalid input.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    check(parser.parse_args().package)
