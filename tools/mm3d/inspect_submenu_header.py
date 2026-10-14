#!/usr/bin/env python3
"""Inspect the verified MM3D SubMenuTitle MFL without modifying game files."""
import argparse,json,struct
from pathlib import Path

def inspect(b):
 assert b[:4]==b'MFL '
 count,panes=struct.unpack_from('<H',b,10)[0],struct.unpack_from('<H',b,14)[0]
 geo,anim,strings=struct.unpack_from('<III',b,20)
 names=b[strings:].split(b'\0'); names=[x.decode('ascii') for x in names if x]
 assert geo==32+count*68 and anim==geo+panes*48
 objects=[];stack=[]
 for i in range(count):
  a=32+i*68;kind=b[a]>>4;lo,hi=struct.unpack_from('<HH',b,a+4)
  while stack and stack[-1][1]==0:stack.pop()
  parent=stack[-1][0] if stack else None
  if stack:stack[-1][1]-=1
  obj={'name':names[1+panes+i],'index':i,'offset':hex(a),'parent':parent,'translate':struct.unpack_from('<3f',b,a+8),'scale':struct.unpack_from('<3f',b,a+20)}
  if kind==3:
   g=geo+lo*48;assert names[1+lo]==obj['name'];obj['geometry']={'offset':hex(g),'xyzwh':struct.unpack_from('<5f',b,g+4)}
  objects.append(obj)
  children=lo if kind==0 else hi if kind==1 else 0
  if children:stack.append([obj['name'],children])
 while stack and stack[-1][1]==0:stack.pop()
 assert not stack
 tracks=[];a=anim
 anim_names=names[-struct.unpack_from('<H',b,18)[0]:]
 for name in anim_names:
  nt,fps,duration=struct.unpack_from('<HHI',b,a);record={'name':name,'offset':hex(a),'fps':fps,'duration':duration,'tracks':[]};a+=8
  for _ in range(nt):
   obj,prop,n,kind,dur=struct.unpack_from('<HHHHI',b,a);at=a;a+=12
   assert kind in (0,2)
   offsets=range(a,a+(dur+1)*4,4) if kind==2 else range(a+8,a+n*12,12)
   values=[{'offset':hex(v),'float':struct.unpack_from('<f',b,v)[0],'raw':struct.unpack_from('<I',b,v)[0]} for v in offsets]
   record['tracks'].append({'offset':hex(at),'object':objects[obj]['name'],'property':prop,'encoding':kind,'values':values})
   a+=(dur+1)*4 if kind==2 else n*12
  tracks.append(record)
 assert a==strings
 return {'layout':names[0],'objects':objects,'animations':tracks}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('mfl',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.write_text(json.dumps(inspect(a.mfl.read_bytes()),indent=2)+'\n')
