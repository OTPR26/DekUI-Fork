#!/usr/bin/env python3
"""Build the isolated mobile host milestone; never builds/imports title code."""
import argparse, json, os, shutil, subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--platform',choices=['host','android','ios','tvos','all'],default='host')
parser.add_argument('--build-root',type=Path,default=Path.home()/'Library/Caches/ReMaskedNativePrototype')
parser.add_argument('--json-include',type=Path)
parser.add_argument('--gradle',default=os.environ.get('GRADLE_BIN','gradle'))
parser.add_argument('--device',action='store_true',help='Build unsigned Apple device apps instead of simulator apps')
args=parser.parse_args();build=args.build_root.resolve();build.mkdir(parents=True,exist_ok=True)
def run(name,command,cwd=ROOT):
    print(name,flush=True)
    with (build/(name+'.log')).open('w') as output:
        subprocess.run([str(x) for x in command],cwd=cwd,stdout=output,stderr=subprocess.STDOUT,check=True)
configure=['cmake','-S',ROOT,'-B',build/'host','-G','Ninja']
if args.json_include:configure+=['-DREMASKED_JSON_INCLUDE='+str(args.json_include.resolve())]
run('host-configure',configure)
run('host-build',['cmake','--build',build/'host','--parallel','3'])
run('host-tests',['ctest','--test-dir',build/'host','--output-on-failure'])
include=args.json_include.resolve() if args.json_include else build/'host/_deps/remasked_json-src/include'
assert (include/'nlohmann/json.hpp').is_file()
receipts={'scope':'Native settings/input and schematic HUD host only; no game renderer or AOT title module',
          'production_version':'0.4.4','production_modified':False,'platforms':{}}
targets=['android','ios','tvos'] if args.platform=='all' else [args.platform]
for platform in targets:
    if platform=='host':continue
    if platform=='android':
        run('android-build',[args.gradle,':app:assembleDebug','--no-daemon','-PremaskedJsonInclude='+str(include)],ROOT/'android')
        source=ROOT/'android/app/build/outputs/apk/debug/app-debug.apk'
        target=build/'ReMasked-Native-Prototype-Android-arm64.apk';shutil.copy2(source,target)
        receipts['platforms'][platform]={'built':True,'artifact':target.name,'application_id':'io.github.otpr26.remasked.nativeprototype'}
    else:
        run('apple-project',['xcodegen','generate'],ROOT/'apple')
        scheme='ReMaskedIOS' if platform=='ios' else 'ReMaskedTV'
        sdk=('iphoneos' if platform=='ios' else 'appletvos') if args.device else ('iphonesimulator' if platform=='ios' else 'appletvsimulator')
        run(platform+'-build',['xcodebuild','-project','ReMaskedNativePrototype.xcodeproj','-scheme',scheme,
            '-configuration','Debug','-sdk',sdk,'-derivedDataPath',build/platform,
            'REMASKED_JSON_INCLUDE='+str(include),'CODE_SIGNING_ALLOWED=NO','build'],ROOT/'apple')
        receipts['platforms'][platform]={'built':True,'signed':False,'sdk':sdk,
            'application_id':'io.github.otpr26.remasked.nativeprototype.'+platform}
(build/'build-receipt.json').write_text(json.dumps(receipts,indent=2)+'\n')
print('Built requested host milestone. Gameplay qualification remains pending.',flush=True)
