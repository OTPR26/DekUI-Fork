import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import package_macos
from bundle_tp_runtime import copy_tp_runtime
from test_bundle_tp_runtime import fixture_runtime


class MacPackageTests(unittest.TestCase):
    def test_actual_setup_payload_without_local_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);app=root/'input.app'
            for name in ('MacOS/azahar','PlugIns/platforms/libqcocoa.dylib'):
                p=app/'Contents'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b'fixture')
            (app/'Contents/Info.plist').write_bytes(plistlib.dumps({}))
            fixture_repo = root / 'tp-fixture'
            fixture_runtime(fixture_repo)
            captured=root/'captured'
            def run(args,**kwargs):
                if args[0]=='ditto':
                    self.assertIn('--norsrc', args)
                    self.assertIn('--noextattr', args)
                    self.assertNotIn('--sequesterRsrc', args)
                    shutil.copytree(Path(args[-2]),captured)
                elif args[0]!='codesign':raise AssertionError(args)
            with patch.object(package_macos, 'copy_tp_runtime', side_effect=lambda repo, payload: copy_tp_runtime(fixture_repo, payload)), patch.object(package_macos,'verify_dependencies'),patch.object(package_macos,'install'),patch.object(package_macos.subprocess,'run',side_effect=run):
                package_macos.package(app,root/'out.zip',root/'python.tar.gz')
            subprocess.run([sys.executable,'-E','-s',str(captured/'Majora ReMasked.app/Contents/Resources/tools/mm3d/package_setup_check.py')],check=True,capture_output=True)
            self.assertFalse((captured/'user').exists())
            self.assertFalse(list(captured.rglob('*.rgba')))
