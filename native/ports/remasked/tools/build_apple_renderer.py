#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an unsigned Apple prototype with the real static NRI/MoltenVK probe."""
import argparse
import shlex
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform', choices=['ios', 'tvos'], required=True)
parser.add_argument('--device', action='store_true')
parser.add_argument('--build-root', type=Path, required=True)
parser.add_argument('--molten-root', type=Path, required=True)
parser.add_argument('--json-include', type=Path, required=True)
parser.add_argument('--title-build', type=Path, help='Optional private static title build for this exact SDK/architecture')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = args.build_root.resolve()
build.mkdir(parents=True, exist_ok=True)
system, scheme, device_sdk, simulator_sdk, slice_name = (
    ('iOS', 'ReMaskedIOS', 'iphoneos', 'iphonesimulator', 'ios') if args.platform == 'ios'
    else ('tvOS', 'ReMaskedTV', 'appletvos', 'appletvsimulator', 'tvos'))
sdk = device_sdk if args.device else simulator_sdk
slice_name += ('-arm64' if args.platform == 'ios' else '-arm64_arm64e') if args.device else '-arm64_x86_64-simulator'
nri = build / ('nri-' + sdk)
def run(name, command, cwd=root):
    with (build / (name + '.log')).open('w') as log:
        subprocess.run([str(arg) for arg in command], cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
run('nri-configure', ['cmake', '-S', root / 'renderer', '-B', nri, '-G', 'Ninja',
    '-DCMAKE_SYSTEM_NAME=' + system, '-DCMAKE_OSX_SYSROOT=' + sdk,
    '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=17.0',
    '-DCMAKE_BUILD_TYPE=Release', '-DREMASKED_MOLTENVK_ROOT=' + str(args.molten_root.resolve()),
    '-DREMASKED_MOLTENVK_SLICE=' + slice_name])
run('nri-build', ['cmake', '--build', nri, '--target', 'remasked_renderer_probe', '-j', '3'])
libraries = [nri / 'libremasked_renderer_probe.a']
libraries += [nri / '_deps/nri-build' / ('lib' + name + '.a') for name in ['NRI', 'NRI_VK', 'NRI_Validation', 'NRI_Shared']]
libraries += [args.molten_root.resolve() / 'MoltenVK/static/MoltenVK.xcframework' / slice_name / 'libMoltenVK.a']
native_flags = ['REMASKED_NATIVE_NRI']
if args.title_build:
    title_build = args.title_build.resolve()
    for name in ['remasked_static_title_host', 'remasked_static_title', 'triaevum_title_whole_aot_support']:
        path = title_build / ('lib' + name + '.a')
        if not path.is_file():
            parser.error('Missing static title library: ' + str(path))
        libraries.append(path)
    native_flags.append('REMASKED_NATIVE_TITLE')
flags = shlex.join([str(path) for path in libraries] + ['-framework', 'Metal', '-framework', 'Foundation',
    '-framework', 'QuartzCore', '-framework', 'IOSurface'])
run('project', ['xcodegen', 'generate'], root / 'apple')
run('app-build', ['xcodebuild', '-project', 'ReMaskedNativePrototype.xcodeproj', '-scheme', scheme,
    '-sdk', sdk, '-configuration', 'Debug', '-derivedDataPath', build / 'app',
    'ARCHS=arm64', 'ONLY_ACTIVE_ARCH=YES',
    'REMASKED_JSON_INCLUDE=' + str(args.json_include.resolve()),
    'REMASKED_NATIVE_FLAGS=' + ' '.join(native_flags),
    'REMASKED_NATIVE_DEFINITIONS=' + ' '.join(flag + '=1' for flag in native_flags),
    'REMASKED_NRI_LIBS=' + flags, 'CODE_SIGNING_ALLOWED=NO', 'build'], root / 'apple')
print('Built real NRI device diagnostic host. Game submission remains unconnected.')
