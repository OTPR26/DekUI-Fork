#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package/sign a private owner iOS or tvOS prototype, preserving its identity."""
import argparse
import hashlib
import json
import plistlib
import shutil
import subprocess
from pathlib import Path


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['app','profile','entitlements','receipt']:
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--icons', type=Path)
    parser.add_argument('--platform', choices=['tvos','ios'], default='tvos')
    parser.add_argument('--identity', required=True)
    parser.add_argument('--device', required=True)
    parser.add_argument('--team', required=True)
    args = parser.parse_args()
    info = plistlib.loads((args.app/'Info.plist').read_bytes())
    expected = 'io.github.otpr26.remasked.nativeprototype.game.'+args.platform
    platform = 'AppleTVOS' if args.platform == 'tvos' else 'iPhoneOS'
    if info.get('CFBundleIdentifier') != expected or info.get('CFBundleSupportedPlatforms') != [platform]:
        raise ValueError('Wrong app identity or platform')
    profile = plistlib.loads(subprocess.check_output(['security','cms','-D','-i',str(args.profile)]))
    if profile['TeamIdentifier'] != [args.team] or args.device not in profile.get('ProvisionedDevices',[]):
        raise ValueError('Wrong signing team or device authorization')
    entitlements = plistlib.loads(args.entitlements.read_bytes())
    if entitlements.get('application-identifier') != args.team+'.'+expected or profile['Entitlements'].get('application-identifier') != args.team+'.'+expected:
        raise ValueError('Signing entitlement identity mismatch')
    if entitlements.get('com.apple.developer.icloud-container-identifiers') != ['iCloud.io.github.otpr26.remasked']:
        raise ValueError('Dedicated ReMasked iCloud container required')
    identities = subprocess.check_output(['security','find-identity','-v','-p','codesigning'],text=True)
    if args.identity not in identities:
        raise ValueError('Requested signing identity is unavailable')
    manifest = json.loads((args.app/'OwnerContent/owner-content.json').read_text())
    for name, expected_hash in manifest['files'].items():
        if sha(args.app/'OwnerContent'/name) != expected_hash:
            raise ValueError('Owner content failed verification: '+name)
    verified_models=0
    models=args.app/'PreparedModels'
    if models.is_dir():
        model_manifest=json.loads((models/'manifest.json').read_text())
        for name, expected_hash in model_manifest.items():
            relative=Path(name)
            if relative.is_absolute() or '..' in relative.parts or sha(models/relative)!=expected_hash:
                raise ValueError('Prepared model failed verification')
            verified_models+=1
    if args.platform == 'tvos':
        if args.icons is None:
            raise ValueError('Compiled tvOS icons are required')
        icons = plistlib.loads((args.icons/'icon-info.plist').read_bytes())
        if 'CFBundleIcons' not in icons or not (args.icons/'Assets.car').is_file():
            raise ValueError('Compiled tvOS app icon is required')
        shutil.copyfile(args.icons/'Assets.car',args.app/'Assets.car')
        info.update(icons)
    elif (info.get('CFBundleDisplayName') != 'Majora ReMasked' or
          info.get('CFBundleIcons',{}).get('CFBundlePrimaryIcon',{}).get('CFBundleIconName') != 'AppIcon' or
          not (args.app/'Assets.car').is_file()):
        raise ValueError('Compiled ReMasked iOS icon and branding are required')
    (args.app/'Info.plist').write_bytes(plistlib.dumps(info))
    shutil.copyfile(args.profile,args.app/'embedded.mobileprovision')
    # Static build libraries are linked into the executable, not runtime assets.
    (args.app/'libshaderc.a').unlink(missing_ok=True)
    subprocess.run(['codesign','--force','--sign',args.identity,'--entitlements',str(args.entitlements),str(args.app)],check=True)
    subprocess.run(['codesign','--verify','--deep','--strict',str(args.app)],check=True)
    receipt = {'publishable':False,'bundle_id':info['CFBundleIdentifier'],
        'display_name':info['CFBundleDisplayName'],'version':info['CFBundleVersion'],
        'icon_sha256':sha(args.app/'Assets.car'),'verified_owner_files':len(manifest['files']),'verified_prepared_model_files':verified_models,
        'executable_sha256':sha(args.app/info['CFBundleExecutable']),
        'device':args.device,'team':args.team,'signed':True}
    args.receipt.write_text(json.dumps(receipt,indent=2)+'\n')
    print('Verified and signed private Apple package:',receipt['display_name'])


if __name__ == '__main__':
    main()
