#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy explicitly supplied owner files into the isolated native Android app only.

No downloads, uploads, production-app writes or generated-game publication.
"""
import argparse,json,subprocess,tempfile,tarfile,io
from pathlib import Path
PACKAGE='io.github.otpr26.remasked.nativegame'
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ['adb','serial','game','owner','current','rom']:p.add_argument('--'+key,required=True)
    args=p.parse_args();game,owner,current,rom=map(Path,[args.game,args.owner,args.current,args.rom])
    root='/sdcard/Android/data/'+PACKAGE+'/files'
    def adb(*parts):subprocess.run([args.adb,'-s',args.serial,*map(str,parts)],check=True)
    if subprocess.run([args.adb,'-s',args.serial,'shell','pidof',PACKAGE+':game'],capture_output=True).stdout.strip():
        p.error('Quit the native prototype before copying inputs')
    # This helper is first-run only. Never overwrite a native session's saves.
    existing=subprocess.run([args.adb,'-s',args.serial,'shell','test','-f',root+'/GameContent/saves/save0.bin'])
    if existing.returncode==0:p.error('Native saves already exist; use an explicit migration instead')
    for path in [game/'PrivateGame/process-manifest.json',owner/'owner-content.json',current/'presentation.json',rom]:
        if not path.is_file():p.error('Missing supplied owner input: '+path.name)
    # Stream through the debug app's own UID. Modern Android's scoped-storage
    # FUSE rules may reject adbd recursive pushes even to a newly installed app.
    # No owner data is left in a world-readable intermediate transfer folder.
    adb('shell','run-as',PACKAGE,'mkdir','-p',root)
    command=[args.adb,'-s',args.serial,'exec-in','run-as',PACKAGE,'tar','-xf','-','-C',root]
    process=subprocess.Popen(command,stdin=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=process.stdin,mode='w|') as archive:
            def owned(info):
                if not (info.isfile() or info.isdir()):raise ValueError('Owner inputs must contain only regular files/directories')
                info.uid=info.gid=0;info.uname=info.gname='';info.mode=0o700 if info.isdir() else 0o600
                return info
            def copy(source,destination):archive.add(source,arcname=destination,filter=owned)
            for name in ['PrivateGame','RestorationHud','resources']:copy(game/name,'GameContent/'+name)
            copy(owner,'OwnerContent')
            copy(current/'saves','GameContent/saves')
            for name in ['adult-link','dark-link','twilight-link']:
                if (current/'models'/name).is_dir():copy(current/'models'/name,'load/mm3d-models/'+name)
            copy(rom,'PrivateInputs/mm3d.3ds')
            profile=json.loads((game/'ReMaskedNativeGame.launch.json').read_text())
            profile['arguments']=[x.replace('${runtime_dir}/GameContent','${profile_dir}') for x in profile['arguments'] if x!='--extended-diagnostics']
            payload=(json.dumps(profile,indent=2)+'\n').encode()
            info=tarfile.TarInfo('GameContent/ReMaskedNativeGame.launch.json');info.size=len(payload);info.mode=0o600
            archive.addfile(info,io.BytesIO(payload))
            copy(current/'presentation.json','GameContent/presentation.json')
            copy(current/'presentation.json','GameContent/runtime-presentation.json')
            for name in ['logs/crashes','savestates','textures/load','textures/dump']:
                info=tarfile.TarInfo('GameContent/'+name);info.type=tarfile.DIRTYPE;info.mode=0o700;archive.addfile(info)
        process.stdin.close()
        if process.wait()!=0:raise RuntimeError('Owner transfer failed')
    finally:
        if process.poll() is None:process.kill();process.wait()
    print('Private native inputs copied; existing production app/data were not modified.')
if __name__=='__main__':main()
