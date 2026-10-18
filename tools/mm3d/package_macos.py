#!/usr/bin/env python3
"""Package a deployed Apple Silicon app, portable setup and clean defaults."""
import argparse
from layout_migration import policy as verify_layout_policy
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
from bundle_restoration import copy_restoration, copy_desktop_hud
from macos_python import install
from package_deck import ROOT, SCRIPTS

MACH_MAGICS = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'}


def macho_files(root):
    for path in root.rglob('*'):
        if path.is_file() and not path.is_symlink():
            with path.open('rb') as stream:
                if stream.read(4) in MACH_MAGICS:
                    yield path


def verify_dependencies(root):
    # Fail closed if deployment leaves a Homebrew/runner-only dependency.
    for path in macho_files(root):
        output = subprocess.check_output(['otool', '-arch', 'arm64', '-L', str(path)], text=True)
        identities = subprocess.check_output(['otool', '-arch', 'arm64', '-D', str(path)], text=True).splitlines()[1:]
        for line in output.splitlines()[1:]:
            dep = line.strip().split(' (', 1)[0]
            if dep in identities:  # A dylib's install ID is not a loaded dependency.
                continue
            if not dep.startswith(('@rpath/', '@loader_path/', '@executable_path/',
                                   '/usr/lib/', '/System/Library/')):
                raise ValueError(f'Nonportable dependency in {path}: {dep}')
        archs = subprocess.check_output(['lipo', '-archs', str(path)], text=True).split()
        if 'arm64' not in archs:
            raise ValueError(f'Missing Apple Silicon architecture: {path}')


def package(bundle, output, python_archive):
    verify_layout_policy(ROOT / 'resources/mm3d')
    if output.exists():
        raise ValueError('Refusing to overwrite package')
    if not (bundle / 'Contents/MacOS/azahar').is_file():
        raise ValueError('Missing deployed azahar.app')
    if not (bundle / 'Contents/PlugIns/platforms/libqcocoa.dylib').is_file():
        raise ValueError('Missing deployed Qt Cocoa plugin')
    with tempfile.TemporaryDirectory(prefix='dekui-macos-package-') as tmp:
        root = Path(tmp) / 'dekui'; root.mkdir()
        app = root / 'DekUI Fork.app'
        shutil.copytree(bundle, app, symlinks=True)
        info = app / 'Contents/Info.plist'
        metadata = plistlib.loads(info.read_bytes())
        metadata.update(CFBundleName='DekUI Fork', CFBundleDisplayName='DekUI Fork', CFBundleIdentifier='io.github.otpr26.dekui')
        info.write_bytes(plistlib.dumps(metadata))
        runtime = app / 'Contents/Resources/runtime'
        install(python_archive, runtime)
        scripts = root / 'tools/mm3d'; scripts.mkdir(parents=True)
        for name in SCRIPTS:
            shutil.copy2(ROOT / 'tools/mm3d' / name, scripts / name)
        resources = root / 'resources/mm3d'; resources.mkdir(parents=True)
        for name in ('supported-rom.json', 'setup-fingerprints.json', 'layout-migration.json'):
            shutil.copy2(ROOT / 'resources/mm3d' / name, resources / name)
        shutil.copytree(ROOT / 'resources/mm3d/defaults', resources / 'defaults')
        shutil.copytree(ROOT / "tools/mm3d/mod_import", scripts / "mod_import", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        font_license=root / "licenses/fonts/LICENSE.txt";font_license.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(ROOT / "src/android/app/src/main/assets/dekui-fonts/LICENSE.txt",font_license)
        copy_desktop_hud(ROOT, root / "resources/mm3d/desktop-hud")
        copy_restoration(ROOT, resources / 'project-restoration')
        for name in ('README.md', 'CHANGELOG.md', 'CREDITS.md', 'KNOWN_ISSUES.md', 'INSTALL.md'):
            shutil.copy2(ROOT / 'resources/mm3d/release-docs' / name, root / name)
        shutil.copy2(ROOT / 'tools/mm3d/macos/INSTALL-MACOS.txt', root / 'INSTALL-MACOS.txt')
        shutil.copy2(ROOT / 'license.txt', root / 'LICENSE.txt')
        for p in (ROOT / 'externals').rglob('*'):
            if p.is_file() and p.name.lower().startswith(('license', 'copying', 'copyright')) and p.stat().st_size < 2_000_000:
                dest = root / 'licenses' / p.relative_to(ROOT / 'externals')
                dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(p, dest)
        molten = ROOT / 'build-macos/externals/MoltenVK'
        for p in molten.glob('*LICENSE*'):
            if p.is_file():
                dest = root / 'licenses/MoltenVK' / p.name
                dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(p, dest)
        verify_dependencies(root)
        # Ad-hoc signatures support Apple Silicon without paid Apple credentials.
        # This does not notarize the app or remove Gatekeeper's first-launch prompt.
        for path in macho_files(runtime):
            subprocess.run(['codesign', '--force', '--sign', '-', str(path)], check=True)
        subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(app)], check=True)
        subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent',
                        str(root), str(output.resolve())], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--python-archive', type=Path, required=True)
    args = parser.parse_args()
    package(args.bundle, args.output, args.python_archive)
