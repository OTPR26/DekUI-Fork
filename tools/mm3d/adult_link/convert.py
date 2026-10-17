"""Adult Link appearance prototype. Converts local user-owned OoT3D assets only.

Format references: xdanieldzd/N3DSCmbViewer (MIT) and magcius/noclip.website.
This generator contains no Nintendo asset payloads.
"""
from model import *
from select_hands import retarget,hand_plan
import hashlib,json
ROOT=Path(__file__).resolve().parent
MAP=list(range(13))+list(range(14,26))
ADULT_HEIGHT_SCALE=6555.463137/3986.693367

def convert(adult,mm,master_sword=False):
 assert len(adult.world)==25 and len(mm.world)==26
 # Keep original MM3D skeleton, equipment, animation material indices and hashes.
 handshapes,removed,handsreport=hand_plan(adult,mm)
 equipment_groups={16:[12,14,16],31:[5,6,7],42:[13,15,17]} if master_sword else {}
 equipment_shapes={H(mesh,0):equipment_groups[mesh[3]] for mesh in adult.meshes if mesh[3] in equipment_groups and mesh[2]!=22}
 base_selected=[mesh for mesh in adult.meshes if mesh[3] in (45,46) or H(mesh,0) in handshapes]
 selected=base_selected+[mesh for mesh in adult.meshes if H(mesh,0) in equipment_shapes]
 base_materials=sorted({mesh[2] for mesh in base_selected})
 used_materials=base_materials+sorted({mesh[2] for mesh in selected}-set(base_materials))
 material_map={old:len(mm.materials)+i for i,old in enumerate(used_materials)}
 buffers=[bytearray(x) for x in mm.buffers];adultbuf=[b'',b'',b'',*adult.buffers[2:]]
 base=[]
 for buf,src in zip(buffers,adultbuf):base.append(align(buf,4));buf.extend(src)
 index=bytearray(mm.chunks['indices']);indexbase=align(index,2);index.extend(adult.chunks['indices'])
 # Append the adult materials after all MM materials (CMAB slots stay intact).
 materials=list(mm.materials);combbase=1+max(H(ma,0x124+j*2) for ma in mm.materials for j in range(U(ma,0x120)));comb=bytearray(mm.combiners[:combbase*40])
 texdata=bytearray(mm.chunks['textures']);textures=list(mm.tex)
 base_textures=sorted({S(adult.materials[mi],16+j*24) for mi in base_materials for j in range(3) if S(adult.materials[mi],16+j*24)>=0})
 used_textures=base_textures+sorted({S(adult.materials[mi],16+j*24) for mi in used_materials for j in range(3) if S(adult.materials[mi],16+j*24)>=0}-set(base_textures))
 texture_map={old:len(mm.tex)+i for i,old in enumerate(used_textures)}
 for ti in used_textures:
  t=bytearray(adult.tex[ti]);old=U(t,16);size=U(t,0);put(t,16,align(texdata,128))
  payload=adult.chunks['textures'][old:old+size];assert len(payload)==size;texdata.extend(payload);textures.append(t)
 for i in used_materials:
  ma=bytearray(adult.materials[i])
  for j in range(3):
   at=16+j*24;ti=S(ma,at)
   if ti>=0:put(ma,at,texture_map[ti],'h')
  for j in range(U(ma,0x120)):put(ma,0x124+j*2,H(ma,0x124+j*2)+combbase,'H')
  # MM-only material fields: defaults match native skin; a unique stable hash.
  ma.extend(mm.materials[1][0x15c:0x168])
  ma.extend(struct.pack('<I',0xada10000+i));assert len(ma)==0x16c;materials.append(ma)
 comb.extend(adult.combiners)
 shapes=list(mm.shapes);meshes=[]
 # Preserve native visibility group hashes for scripted equipment operations.
 templates={m[3]:m for m in mm.meshes}
 for i,m in enumerate(mm.meshes):
  if m[3] not in (28,29,30,31,32,33) and i not in removed:
   if master_sword and m[3] in (5,6,7,12,13,14,15,16,17) and m[2] not in (2,3):continue
   meshes.append(m)
 # Counter-scale equipment around each native attachment bone. The runtime
 # raises the human draw scale, so blade/shield dimensions remain unchanged;
 # their joints move naturally with the taller body. No adult body is shrunk.
 for si in sorted({H(mesh,0) for mesh in meshes}):
  pos,norm,uv,bi,bw,pr=mm.geometry(si)
  assert all(mode in (0,1) for _,_,mode,_,_ in pr),'Unexpected smoothly skinned equipment'
  shape=bytearray(shapes[si]);start=align(buffers[0],4)
  buffers[0].extend((pos/ADULT_HEIGHT_SCALE).astype('<f4').tobytes())
  shape[0x24:0x40]=struct.pack('<IfHH4f',start,1,0x1406,0,0,0,0,0);shapes[si]=shape
 report=[]
 for srcmesh in selected:
  si,mat,vis=struct.unpack_from('<HBB',srcmesh)
  if vis not in (45,46) and si not in handshapes and si not in equipment_shapes:continue
  targetgroups=equipment_shapes[si] if si in equipment_shapes else handshapes[si] if si in handshapes else [28,29] if vis==45 else [33]
  sh=adult.shapes[si];pos,norm,uv,bi,bw,pr=adult.geometry(si);n=len(pos);dim=H(sh,0x104)
  newpos,newnorm=retarget(adult,mm,si)
  # Insert MM's tangent descriptor; shift vertex color flag by one bit.
  tangent=struct.pack('<IfHH4f',0,1,0x1406,1,0,0,0,0)
  dst=bytearray(sh[:0x5c]+tangent+sh[0x5c:])
  flags=H(dst,10);put(dst,10,(flags&3)|((flags&~3)<<1),'H')
  for attr in range(9):
   if attr==2:continue
   at=0x24+attr*28;put(dst,at,U(dst,at)+base[attr])
  for attr,values in [(0,newpos),(1,newnorm)]:
   start=align(buffers[attr],4);buffers[attr].extend(values.astype('<f4').tobytes());at=0x24+attr*28
   dst[at:at+28]=struct.pack('<IfHH4f',start,1,0x1406,0,0,0,0,0)
  # PRMS pointers are relative to their parent shape. Bone and index tables relocate.
  for j,(o,p,mode,bones,idx) in enumerate(pr):
   no=o+28;put(dst,0x124+j*2,no,'H');assert dst[no:no+4]==b'prms'
   bo=no+U(p,16)
   for k,b in enumerate(bones):put(dst,bo+k*2,MAP[b],'H')
   po=no+U(p,20);start=H(dst,po+22)+indexbase//2;assert start<=65535;put(dst,po+22,start,'H')
  put(dst,4,len(dst));newsi=len(shapes);shapes.append(dst)
  for tg in targetgroups:
   template=templates[tg];meshes.append(struct.pack('<HBB',newsi,material_map[mat],tg)+template[4:])
  report.append({'adult_shape':si,'vertices':n,'native_groups':targetgroups,'materials':material_map[mat]})
 # Native renderer expects opaque meshes before transparent meshes, sorted by material.
 # Identify transparency from each source mesh's original opaque range.
 mmopaque=H(mm.mesh_header,12);adopaque=H(adult.mesh_header,12)
 transparent={(H(m,0),m[2]) for m in mm.meshes[mmopaque:]}
 meshes.sort(key=lambda m:((H(m,0),m[2]) in transparent,m[2]))
 opaque=sum((H(m,0),m[2]) not in transparent for m in meshes)
 msh=bytearray(mm.mesh_header);put(msh,8,len(meshes));put(msh,12,opaque,'H');msh.extend(b''.join(meshes));put(msh,4,len(msh))
 shp=bytearray(mm.shape_header);put(shp,8,len(shapes));shp.extend(b'\0'*(len(shapes)*2));align(shp,4)
 for i,s in enumerate(shapes):assert len(shp)<65536;put(shp,16+i*2,len(shp),'H');shp.extend(s);align(shp,4)
 put(shp,4,len(shp))
 sklm=bytearray(chunk(b'sklm',struct.pack('<II',16,16+len(msh))+msh+shp))
 vatr=bytearray(b'vatr'+b'\0'*8+b'\0'*72);put(vatr,8,len(buffers[0])//12)
 for i,b in enumerate(buffers):align(vatr,4);put(vatr,12+i*8,len(b));put(vatr,16+i*8,len(vatr));vatr.extend(b)
 put(vatr,4,len(vatr))
 mats=bytearray(chunk(b'mats',struct.pack('<I',len(materials))+b''.join(materials)+comb))
 # Grezzo stores this legacy length separately from the actual combiner extent.
 put(mats,4,12+len(materials)*(0x16c+0x24))
 tex=chunk(b'tex ',struct.pack('<I',len(textures))+b''.join(textures))
 chunks={**mm.chunks,'sklm':sklm,'vatr':vatr,'mats':mats,'tex':tex,'indices':index,'textures':texdata}
 out=bytearray(mm.data[:0x4c]);put(out,0x20,len(index)//2)
 for i,key in enumerate(['skl','qtrs','mats','tex','sklm','luts','vatr','indices','textures']):
  align(out,128 if key=='textures' else 4);put(out,0x24+i*4,len(out));out.extend(chunks[key])
 put(out,4,len(out));return bytes(out),report

def validate_materials(model):
 # Some Grezzo files declare a short mats length; validate the complete shared
 # table through tex offset, rather than trusting that legacy length field.
 operations={0x1e01,0x2100,0x104,0x8574,0x8575,0x84e7,0x86ae,0x86af,0x6401,0x6402}
 for material in model.materials:
  for j in range(U(material,0x120)):
   offset=H(material,0x124+j*2)*40
   assert offset+40<=len(model.combiners),'Truncated material combiner table'
   assert H(model.combiners,offset) in operations,'Invalid RGB combiner operation'
   assert H(model.combiners,offset+2) in operations,'Invalid alpha combiner operation'
  for j in range(3):assert -1<=S(material,16+j*24)<len(model.tex)

def repack_gar(gar,filename,replacement):
 from unpack import entries
 es=list(entries(gar));meta=U(gar,16);offsets=U(gar,20);out=bytearray(gar[:min(e[1] for e in es)])
 for i,(name,oldoff,payload) in enumerate(es):
  align(out,4);put(out,offsets+i*4,len(out));data=replacement if name==filename else payload
  put(out,meta+i*12,len(data));out.extend(data)
 put(out,4,len(out));assert dict((n,p) for n,_,p in entries(out))[filename]==replacement
 for name,_,payload in es:
  if name!=filename:assert dict((n,p) for n,_,p in entries(out))[name]==payload
 return bytes(out)
def literals(data,header):
 out=bytearray()
 for i in range(0,len(data),8):s=data[i:i+8];out.append((1<<len(s))-1);out.extend(s)
 return header[:4]+header[4:8]+struct.pack('<II',len(data),len(out))+out
if __name__=='__main__':
 import argparse
 from unpack import decompress,entries
 ap=argparse.ArgumentParser(description="Build a local MM3D human-form appearance prototype from user-owned model assets")
 ap.add_argument('--adult-model',type=Path,required=True,help='OoT3D adult link_v2.cmb, CMB v6')
 ap.add_argument('--human-archive',type=Path,required=True,help='MM3D USA human zelda2_link_child_new.gar.lzs')
 ap.add_argument('--kokiri-sword',action='store_true',help='Use Kokiri appearance for all native sword tiers')
 ap.add_argument('--master-sword',action='store_true',help='Include OoT3D Master Sword and sheath')
 ap.add_argument('--output-dir',type=Path,required=True)
 args=ap.parse_args()
 adult=Model(args.adult_model.read_bytes());source=args.human_archive.read_bytes();gar=decompress(source)
 native=dict((n,p) for n,_,p in entries(gar));key='child/model/link_child.cmb';mm=Model(native[key])
 assert not adult.mm and mm.mm
 if args.master_sword and args.kokiri_sword:ap.error('Choose one sword appearance')
 out,report=convert(adult,mm,args.master_sword)
 if args.kokiri_sword:
  from sword_variants import kokiri_appearance
  out=kokiri_appearance(Model(out))
 test=Model(out);validate_materials(test)
 assert test.chunks['skl']==mm.chunks['skl']
 for i in range(len(test.shapes)):test.geometry(i)
 rebuilt=repack_gar(gar,key,out);packed=literals(rebuilt,source);assert decompress(packed)==rebuilt
 dst=args.output_dir;dst.mkdir(parents=True,exist_ok=True)
 (dst/'zelda2_link_child_new.gar.lzs').write_bytes(packed)
 (dst/'conversion.json').write_text(json.dumps({
  'prototype':True,'adult_model_sha256':hashlib.sha256(adult.data).hexdigest(),
  'native_archive_sha256':hashlib.sha256(source).hexdigest(),
  'output_sha256':hashlib.sha256(packed).hexdigest(),'converted':report,
  'hands':hand_plan(adult,mm)[2]},indent=2))
 print('Validated local actor archive:',len(packed),'bytes')
