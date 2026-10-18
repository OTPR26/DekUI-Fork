#!/usr/bin/env python3
"""Package a clean Windows bundle with allowlisted setup files, never local user data."""
from bundle_restoration import copy_restoration
from windows_python import install as install_python
import argparse
from layout_migration import policy as verify_layout_policy
from pathlib import Path
import shutil
import tempfile
import zipfile
from package_deck import ROOT, SCRIPTS


def package(bundle, output, python_archive, architecture='x64'):
    verify_layout_policy(ROOT / 'resources/mm3d')
    if output.exists():
        raise ValueError('Refusing to overwrite package')
    if not (bundle / 'azahar.exe').is_file():
        raise ValueError('Missing bundled azahar.exe')
    if not (bundle / 'plugins/platforms/qwindows.dll').is_file():
        raise ValueError('Missing Qt Windows platform plugin')
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / 'dekui'; root.mkdir()
        shutil.copy2(bundle / 'azahar.exe', root / 'Dekui.exe')
        install_python(python_archive, root / 'runtime/python', architecture)
        # The CI bundle is clean; only deployment files are accepted here.
        for p in bundle.rglob('*'):
            rel = p.relative_to(bundle)
            allowed = (len(rel.parts) == 1 and (p.suffix.lower() == '.dll' or p.name == 'qt.conf'))
            allowed |= rel.parts[0] == 'plugins' and p.suffix.lower() == '.dll'
            if p.is_file() and allowed:
                target = root / rel; target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, target)
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
        copy_restoration(ROOT, root / 'resources/mm3d/project-restoration')
        shutil.copy2(ROOT / 'resources/mm3d/release-docs/README.md', root / 'README.md')
        for name in ('CHANGELOG.md', 'CREDITS.md', 'KNOWN_ISSUES.md', 'INSTALL.md'):
            shutil.copy2(ROOT / 'resources/mm3d/release-docs' / name, root / name)
        shutil.copy2(ROOT / 'license.txt', root / 'LICENSE.txt')
        for p in (ROOT / 'externals').rglob('*'):
            if p.is_file() and p.name.lower().startswith(('license', 'copying', 'copyright')) and p.stat().st_size < 2_000_000:
                dest = root / 'licenses' / p.relative_to(ROOT / 'externals')
                dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(p, dest)
        # System toolchain packages carry their own dependency notices.
        notices = bundle / 'licenses'
        if notices.is_dir():
            shutil.copytree(notices, root / 'licenses/toolchain')
        output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
            for p in sorted(root.rglob('*')):
                if p.is_file(): archive.write(p, p.relative_to(root.parent).as_posix())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument("--python-archive", type=Path, required=True)
    parser.add_argument('--architecture', choices=('x64', 'arm64'), default='x64')
    args = parser.parse_args()
    package(args.bundle, args.output, args.python_archive, args.architecture)
