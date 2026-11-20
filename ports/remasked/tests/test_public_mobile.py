# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util,io,tempfile,unittest,zipfile
from pathlib import Path
spec=importlib.util.spec_from_file_location('public_mobile_audit',Path(__file__).resolve().parents[1]/'tools/audit_public_mobile.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class PublicPackageTests(unittest.TestCase):
    def scan(self,entries):
        with tempfile.TemporaryDirectory() as folder:
            package=Path(folder)/'test.ipa'
            with zipfile.ZipFile(package,'w') as archive:
                for name,data in entries:archive.writestr(name,data)
            return module.audit(package)
    def test_clean_resources(self):
        self.assertEqual(self.scan([('Payload/ReMasked.app/icon.png',b'synthetic icon'),('Payload/ReMasked.app/ReMasked',b'synthetic executable')])['status'],'PASS')
    def test_game_and_signing_payloads(self):
        for name in ['OwnerContent/test.png','PrivateGame/code.bin','PreparedModels/model.gar.lzs','InitialSaves/save0.bin','henrikomobile/test.png','embedded.mobileprovision','signing.p12','rom.3ds','texture.bclim']:
            with self.subTest(name=name):self.assertEqual(self.scan([(name,b'synthetic fixture')])['status'],'FAIL')
    def test_home_paths(self):
        # Generic synthetic usernames; no owner's personal paths are fixtures.
        text='/Users/example/Build/source/file.cpp'
        for data in [text.encode(),text.encode('utf-16le'),text.encode('utf-16be')]:
            self.assertEqual(self.scan([('lib/libgame.so',data)])['status'],'FAIL')
    def test_parser_marker_is_not_a_private_key(self):
        self.assertEqual(self.scan([('lib/libtls.so',b'-----BEGIN PRIVATE KEY-----')])['status'],'PASS')
    def test_actual_private_key_shape(self):
        fake=b'-----BEGIN PRIVATE KEY-----\n'+b'A'*100+b'\n-----END PRIVATE KEY-----'
        self.assertEqual(self.scan([('secret.txt',fake)])['status'],'FAIL')
    def test_nested_python_archive(self):
        inner=io.BytesIO()
        with zipfile.ZipFile(inner,'w') as z:z.writestr('save0.bin',b'synthetic save')
        self.assertEqual(self.scan([('assets/python/app.imy',inner.getvalue())])['status'],'FAIL')
    def test_traversal_and_duplicate(self):
        self.assertEqual(self.scan([('../escape',b'x')])['status'],'FAIL')
        self.assertEqual(self.scan([('a',b'x'),('a',b'y')])['status'],'FAIL')
if __name__=='__main__':unittest.main()
