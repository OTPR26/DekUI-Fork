#!/usr/bin/env python3
"""Allowlisted Linux package; never copies local user data or installed mod directories."""
from bundle_tp_runtime import copy_tp_runtime
from bundle_restoration import copy_restoration, copy_desktop_hud
import argparse
from layout_migration import policy as verify_layout_policy
from pathlib import Path
import shutil
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ('graphical_setup.py','setup_game_folder.py','probe_rom.py','build_release_assets.py',
           'release_setup.py','restore_action_flips.py','stop_map_slides.py',
           'classic_owl_text.py','classic_localized_text.py','single_screen_text.py',
           'double_prices.py','upgrade_game_resources.py','layout_migration.py','package_setup_check.py','desktop_hud.py')


def package(appimage, output):
    verify_layout_policy(ROOT / 'resources/mm3d')
    if output.exists(): raise ValueError('Refusing to overwrite package')
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp)/'dekui';root.mkdir()
        shutil.copy2(appimage,root/'Majora-ReMasked.AppImage');(root/'Majora-ReMasked.AppImage').chmod(0o755)
        shutil.copy2(ROOT/'resources/mm3d/release-docs/README.md',root/'README.md')
        for name in ('CHANGELOG.md','CREDITS.md','KNOWN_ISSUES.md', 'INSTALL.md'):
            shutil.copy2(ROOT/'resources/mm3d/release-docs'/name, root/name)
        scripts=root/'tools/mm3d';scripts.mkdir(parents=True)
        for name in SCRIPTS: shutil.copy2(ROOT/'tools/mm3d'/name,scripts/name)
        copy_tp_runtime(ROOT, root)
        for name in ('supported-rom.json','setup-fingerprints.json','layout-migration.json'):
            target=root/'resources/mm3d'/name;target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(ROOT/'resources/mm3d'/name,target)
        shutil.copytree(ROOT/'resources/mm3d/defaults',root/'resources/mm3d/defaults')
        shutil.copytree(ROOT / "tools/mm3d/mod_import", scripts / "mod_import", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        font_license=root / "licenses/fonts/LICENSE.txt";font_license.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(ROOT / "src/android/app/src/main/assets/dekui-fonts/LICENSE.txt",font_license)
        copy_desktop_hud(ROOT, root / "resources/mm3d/desktop-hud")
        copy_restoration(ROOT, root / 'resources/mm3d/project-restoration')
        shutil.copy2(ROOT/'license.txt',root/'LICENSE.txt')
        notices=root/'licenses';notices.mkdir(exist_ok=True)
        # Include dependency license texts, never dependency source data/assets.
        for p in sorted((ROOT/'externals').rglob('*')):
            if p.is_file() and p.name.lower().startswith(('license','copying','copyright')) and p.stat().st_size<2_000_000:
                target=notices/p.relative_to(ROOT/'externals');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
        output.parent.mkdir(parents=True,exist_ok=True)
        with tarfile.open(output,'w:gz') as archive: archive.add(root,arcname=root.name)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('appimage',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();package(a.appimage,a.output)
