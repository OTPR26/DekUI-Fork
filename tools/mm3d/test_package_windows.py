import tempfile
import shutil
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch
from test_bundle_tp_runtime import fixture_runtime
import package_windows
import windows_python
from test_windows_python import fixture_archive


class WindowsPackageTest(unittest.TestCase):
    def test_only_deployment_and_setup_files_are_packaged(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); bundle = root / 'bundle'; repo = root / 'repo'
            for name in ('azahar.exe', 'Qt6Core.dll', 'qt.conf', 'plugins/platforms/qwindows.dll',
                         'user/save.bin', 'game/mm3d.cci', 'restoration/code.bps', 'debug.exe'):
                p = bundle / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'test')
            for name in package_windows.SCRIPTS:
                p = repo / 'tools/mm3d' / name; p.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(package_windows.ROOT/'tools/mm3d'/name,p)
            shutil.copytree(package_windows.ROOT / 'src/android/app/src/main/assets/dekui-pc-controls', repo / 'src/android/app/src/main/assets/dekui-pc-controls')
            shutil.copytree(package_windows.ROOT / 'resources/mm3d/desktop-hud', repo / 'resources/mm3d/desktop-hud')
            shutil.copytree(package_windows.ROOT / 'src/android/app/src/main/assets/dekui-hud8x', repo / 'src/android/app/src/main/assets/dekui-hud8x')
            shutil.copytree(package_windows.ROOT / 'tools/mm3d/mod_import' , repo / 'tools/mm3d/mod_import')
            font=repo / 'src/android/app/src/main/assets/dekui-fonts/LICENSE.txt';font.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(package_windows.ROOT / 'src/android/app/src/main/assets/dekui-fonts/LICENSE.txt',font)
            for name in ('tools/mm3d/windows/README.txt', 'license.txt',
                         'resources/mm3d/release-docs/README.md',
                         'resources/mm3d/release-docs/CHANGELOG.md',
                         'resources/mm3d/release-docs/CREDITS.md',
                         'resources/mm3d/release-docs/KNOWN_ISSUES.md',
                         'resources/mm3d/release-docs/INSTALL.md',
                         'resources/mm3d/supported-rom.json', 'resources/mm3d/setup-fingerprints.json',
                         'resources/mm3d/defaults/presentation.json', 'resources/mm3d/layout-migration.json'):
                p = repo / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('{}')
            shutil.copytree(package_windows.ROOT / 'resources/mm3d/project-restoration',
                            repo / 'resources/mm3d/project-restoration')
            shutil.copy2(package_windows.ROOT / 'resources/mm3d/setup-fingerprints.json',
                         repo / 'resources/mm3d/setup-fingerprints.json')
            shutil.copytree(package_windows.ROOT / 'resources/mm3d/defaults', repo / 'resources/mm3d/defaults', dirs_exist_ok=True)
            shutil.copy2(package_windows.ROOT / 'resources/mm3d/layout-migration.json', repo / 'resources/mm3d/layout-migration.json')
            presentation = (package_windows.ROOT / 'resources/mm3d/defaults/presentation.json').read_bytes()
            (repo / 'resources/mm3d/defaults/presentation.json').write_bytes(presentation)
            fixture_runtime(repo)
            output = root / 'dekui.zip'
            python_archive = root / 'python.zip'
            digest = fixture_archive(python_archive)
            with patch.object(package_windows, 'ROOT', repo), patch.object(windows_python, 'SHA256', digest):
                package_windows.package(bundle, output, python_archive)
                with self.assertRaises(ValueError): package_windows.package(bundle, output, python_archive)
            with zipfile.ZipFile(output) as archive:
                names = set(archive.namelist())
                archive.extractall(root/'unpacked')
                import subprocess, sys
                subprocess.run([sys.executable,'-E','-s',str(root/'unpacked/dekui/tools/mm3d/package_setup_check.py')], check=True, capture_output=True)
                self.assertEqual(archive.read('dekui/resources/mm3d/defaults/presentation.json'), presentation)
                self.assertIn('dekui/Dekui.exe', names)
                self.assertEqual(archive.read('dekui/README.md'),
                                 (repo / 'resources/mm3d/release-docs/README.md').read_bytes())
                self.assertNotIn('dekui/INSTALL.txt', names)
                self.assertNotIn('dekui/SOURCE.md', names)
                for name in ('CHANGELOG.md', 'CREDITS.md', 'KNOWN_ISSUES.md', 'INSTALL.md'):
                    self.assertEqual(archive.read('dekui/' + name), (repo / 'resources/mm3d/release-docs' / name).read_bytes())
                self.assertNotIn('dekui/README.txt', names)
                self.assertIn('dekui/runtime/python/python.exe', names)
                self.assertIn('dekui/runtime/python/LICENSE.txt', names)
                self.assertEqual(archive.read('dekui/runtime/python/python313._pth'),
                                 b'python313.zip\n.\n../../tools/mm3d\n')
                self.assertNotIn('dekui/SHA256SUMS', names)
                self.assertIn('dekui/plugins/platforms/qwindows.dll', names)
                self.assertIn('dekui/tools/mm3d/graphical_setup.py', names)
                for name in ('code.bps', 'exheader.bin', 'LICENSE.md', 'NOTICE.txt', 'SOURCE.tar.gz'):
                    self.assertIn('dekui/resources/mm3d/project-restoration/' + name, names)
                self.assertFalse(any('/user/' in n or '/game/' in n or '/restoration/' in n or n.endswith('debug.exe') for n in names))

    def test_rejects_missing_runtime(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                package_windows.package(Path(tmp), Path(tmp) / 'out.zip', Path(tmp) / 'python.zip')
