#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual UIKit menu against iOS without any game/title inputs."""
from pathlib import Path
import argparse,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--core-build',type=Path,required=True);p.add_argument('--sdl',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3]
includes=['tools/renderer/pipeline_prepare','tools/oot3d/native_pica_frontend','tools/oot3d/native_game_runtime','titles/oot3d/topscreen','titles/oot3d/gameplay','titles/oot3d/environment','tools/oot3d/ui_topscreen','ports/remasked/runtime','tools/oot3d/decomp_support/include','runtime/three_ds_recomp/src','runtime/three_ds_recomp/include','runtime/three_ds_recomp/module_interface','tools/oot3d/typed_gameplay','tools/oot3d/native_a32_runtime/upstream','tools/oot3d/native_a32_runtime/upstream/recomp','tools/oot3d/native_a32_runtime','tools/oot3d/native_a32_runtime/mass_cpp','tools/three_ds/input','tools/oot3d/third_party/azahar_pica','tools/oot3d/ui_contract','tools/oot3d/ui_menu','tools/oot3d/ui_n64','titles/mm3d/adapter','titles/mm3d/dekui','titles/mm3d/restoration']
command=['xcrun','--sdk','iphoneos','clang++','-std=c++20','-fobjc-arc','-target','arm64-apple-ios17.0','-Wno-deprecated-declarations','-DREMASKED_IOS_GAME=1','-DREMASKED_APPLE_GAME=1','-DREMASKED_NATIVE_GAME=1','-DCONTROLLERBUTTONS_T=uint32_t','-fsyntax-only']
command+=['-I'+str(root/x) for x in includes]
command+=['-I'+str(a.sdl/'include'),'-I'+str(a.core_build/'_deps/remasked_json-src/include'),str(root/'ports/remasked/runtime/ios/ios_host.mm')]
subprocess.run(command,check=True)
print('Actual iOS launcher, Data-menu integration and conflict UI compile against the iOS SDK.')
