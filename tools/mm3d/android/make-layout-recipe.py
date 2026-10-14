#!/usr/bin/env python3
"""Generate bounded copy/write operations from fingerprint-verified local assets.
The recipe carries offsets and changed scalar bytes, never whole game layouts.
Run after prepare-layouts.py; the resulting outputs must match setup-fingerprints.json.
"""
import argparse, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('stage',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[3]
m=json.loads((root/'resources/mm3d/setup-fingerprints.json').read_text())
inputs={}
for e in m['files']:
 key=e['input_root']+'/'+e['input']
 source=(a.stage/'original' if e['input_root']=='original' else root/'resources/mm3d/project-restoration')/e['input']
 data=source.read_bytes()
 assert hashlib.sha256(data).hexdigest()==e['input_sha256']
 inputs[key]=data
inputs['original/romfs/layout/Joker.Main.Play.gar']=(a.stage/'original/romfs/layout/Joker.Main.Play.gar').read_bytes()
assert hashlib.sha256(inputs['original/romfs/layout/Joker.Main.Play.gar']).hexdigest()==m['original_action_sha256']
result={'version':1,'inputs':{k:hashlib.sha256(v).hexdigest() for k,v in inputs.items()},'files':[]}
for e in m['files']:
 key=e['input_root']+'/'+e['input']; base=inputs[key]
 output=(a.stage/'mods/0004000000125500'/e['output']).read_bytes()
 assert hashlib.sha256(output).hexdigest()==e['output_sha256']
 ops=[];at=0
 while at<len(output):
  if at<len(base) and output[at]==base[at]:
   at+=1;continue
  # Appended animation data is copied from supplied assets, not embedded.
  if at>=len(base):
   match=None
   for source,data in inputs.items():
    pos=data.find(output[at:at+16]) if len(output)-at>=16 else -1
    if pos>=0:
     count=16
     while at+count<len(output) and pos+count<len(data) and output[at+count]==data[pos+count]:count+=1
     if match is None or count>match[2]:match=(source,pos,count)
   if match:
    ops.append({'at':at,'copy':match[0],'offset':match[1],'length':match[2]});at+=match[2];continue
  ops.append({'at':at,'byte':output[at]});at+=1
 result['files'].append({'output':e['output'],'base':key,'size':len(output),'sha256':e['output_sha256'],'ops':ops})
a.output.write_text(json.dumps(result,separators=(',',':'))+'\n')
print('Recipe:',a.output,'bytes:',a.output.stat().st_size)
