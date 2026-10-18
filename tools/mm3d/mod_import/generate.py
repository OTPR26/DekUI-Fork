#!/usr/bin/env python3
"""Stage a complete desktop mod from user-selected ROMs; native frontend finalizes textures."""
import argparse,hashlib,json,shutil,sys,tempfile,struct
from pathlib import Path
from binary import Reader,entries,decompress,u,h,require
from model import Model,convert,tunic
from voice import convert as convert_voices

def progress(message):print(json.dumps({'progress':message}),flush=True)
def generate(root,mm_rom,oot_rom,dark=False):
    model_id='dark-link' if dark else 'adult-link';parent=root/'user/load/mm3d-models';parent.mkdir(parents=True,exist_ok=True)
    stage=Path(tempfile.mkdtemp(prefix='.'+model_id+'-',dir=parent))
    mm=oot=None
    def write(name,data):
        p=stage/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
    try:
        mm=Reader(mm_rom);oot=Reader(oot_rom)
        progress('Reading your OoT3D model and original colors…')
        archive=oot.actor('actor/zelda_torch2.zar' if dark else 'actor/zelda_link_boy_new.zar',True);assets={n:d for n,_,d in entries(archive)}
        source=assets['Model/darklink.cmb' if dark else 'boy/model/link_v2.cmb'];body=b'' if dark else assets['boy/misc/link_body.cmab'];native=mm.actor('actors/zelda2_link_child_new.gar.lzs')
        progress('Importing Adult Link’s voices…')
        sound,clips,keys=convert_voices(oot.actor('sound/QueenSound.bcsar',True),mm.actor('sound/sequence/JokerSound.bcsar'))
        write('voices/romfs/sound/sequence/JokerSound.bcsar',sound)
        progress('Reading the sword icon…');icon=None
        for language in ('01_US_ENGLISH','02_EU_ENGLISH','00_JP_JAPANESE'):
            try:icon=oot.actor('menu/'+language+'/icon_item_menu00.ctxb',True);break
            except ValueError as e:
                if 'Required ROM asset is missing' not in str(e):raise
        require(icon is not None,'OoT3D item icon atlas is missing');require(len(icon)==262216 and icon[:4]==b'ctxb' and h(icon,0x2c)==512 and h(icon,0x30)==0x675b,'Unsupported icon atlas')
        write('texture-sources/icon.bin',icon[72:]);texture_meta=[{'file':'texture-sources/icon.bin','width':512,'height':512,'format':13,'icon':True}];report={'schema':1,'voices':{'clips':clips,'human_keys':keys},'model':model_id};seen=set();count=0
        for color in ([0] if dark else range(3)):
            for master in ([True] if dark else (False,True)):
                name=('master' if master else 'kokiri')+('' if color==0 else '-blue' if color==1 else '-red')
                progress('Creating '+('Dark Link' if dark else name)+'…');data=convert(source if dark else tunic(source,body,color),native,master,dark)
                dest='romfs/actors/zelda2_link_child_new.gar.lzs' if dark else 'variants/'+name+'/romfs/actors/zelda2_link_child_new.gar.lzs';write(dest,data)
                if not dark and color==0 and not master:write('romfs/actors/zelda2_link_child_new.gar.lzs',data)
                report[name]=hashlib.sha256(data).hexdigest();count+=1
                m=Model(next(d for n,_,d in entries(decompress(data)) if n=='child/model/link_child.cmb'))
                for t in m.tex[33:]:
                    width=h(t,8);height=h(t,10);fmt={0x675a:12,0x675b:13}.get(h(t,12),3);size=width*height*({3:16,12:4,13:8}[fmt])//8;raw=m.chunks['textures'][u(t,16):u(t,16)+size];digest=hashlib.sha256(raw).hexdigest()
                    if digest in seen:continue
                    seen.add(digest);file='texture-sources/'+digest+'.bin';write(file,raw);texture_meta.append(dict(file=file,width=width,height=height,format=fmt,icon=False))
        write('texture-sources/manifest.json',json.dumps(texture_meta).encode());report['variants']=count;report['source_rom']=str(oot_rom);write('generated.json',json.dumps(report,indent=2).encode())
        print(json.dumps({'stage':str(stage.resolve()),'model':model_id,'clips':clips,'variants':count}),flush=True)
        return stage
    except BaseException:
        shutil.rmtree(stage);raise
    finally:
        if mm:mm.close()
        if oot:oot.close()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--mm-rom',type=Path,required=True);p.add_argument('--oot-rom',type=Path,required=True);p.add_argument('--dark',action='store_true');args=p.parse_args()
    try:generate(args.root,args.mm_rom,args.oot_rom,args.dark)
    except (ValueError,OSError,KeyError,struct.error) as e:
        print(str(e),file=sys.stderr);sys.exit(1)
