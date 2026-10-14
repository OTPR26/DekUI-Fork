import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from bundle_restoration import copy_restoration, FILES
from package_deck import ROOT, package


class RestorationBundleTests(unittest.TestCase):
    def test_official_checksums_and_source_dependency(self):
        with tempfile.TemporaryDirectory() as tmp:
            dest=Path(tmp)/'restoration';copy_restoration(ROOT,dest)
            self.assertEqual(set(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file()),set(FILES))
            with tarfile.open(dest/'SOURCE.tar.gz') as source:
                names=source.getnames()
                self.assertIn('project-restoration/make_release.sh',names)
                self.assertIn('project-restoration/source/lib/eigen/COPYING.MPL2',names)

    def test_windows_checkout_preserves_pinned_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);repo=root/'repo';repo.mkdir()
            shutil.copy2(ROOT/'.gitattributes',repo/'.gitattributes')
            resource=repo/'resources/mm3d'
            shutil.copytree(ROOT/'resources/mm3d/project-restoration',resource/'project-restoration')
            shutil.copy2(ROOT/'resources/mm3d/setup-fingerprints.json',resource/'setup-fingerprints.json')
            def git(*args):
                subprocess.run(['git',*args],cwd=repo,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            git('init');git('config','core.autocrlf','false')
            git('add','.');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','-c','commit.gpgsign=false','commit','-m','fixture')
            checkout=root/'windows-checkout'
            git('clone','--config','core.autocrlf=true',str(repo),str(checkout))
            copy_restoration(checkout,root/'output')
            for name in FILES:
                self.assertEqual((root/'output'/name).read_bytes(),(resource/'project-restoration'/name).read_bytes())

    def test_corrupt_bundle_rejected_before_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);resource=root/'resources/mm3d'
            shutil.copytree(ROOT/'resources/mm3d/project-restoration',resource/'project-restoration')
            shutil.copy2(ROOT/'resources/mm3d/setup-fingerprints.json',resource/'setup-fingerprints.json')
            (resource/'project-restoration/exheader.bin').write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError,'checksum mismatch'):copy_restoration(root,root/'output')
            self.assertFalse((root/'output').exists())

    def test_deck_package_includes_only_bundled_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);app=root/'test.AppImage';app.write_bytes(b'fixture')
            package(app,root/'deck.tar.gz')
            with tarfile.open(root/'deck.tar.gz') as archive:
                # Locally generated archive, not an untrusted download.
                for member in archive.getmembers():
                    if member.isfile():
                        target=root/'unpacked'/member.name;target.parent.mkdir(parents=True,exist_ok=True)
                        target.write_bytes(archive.extractfile(member).read())
                import sys
                subprocess.run([sys.executable,'-E','-s',str(root/'unpacked/dekui/tools/mm3d/package_setup_check.py')],check=True,capture_output=True)
                self.assertEqual(archive.extractfile('dekui/resources/mm3d/defaults/presentation.json').read(),
                                 (ROOT/'resources/mm3d/defaults/presentation.json').read_bytes())
                for name in FILES:
                    self.assertEqual(archive.extractfile('dekui/resources/mm3d/project-restoration/'+name).read(),
                                     (ROOT/'resources/mm3d/project-restoration'/name).read_bytes())
                self.assertFalse(any(n.endswith(('.cci','.3ds')) for n in archive.getnames()))
