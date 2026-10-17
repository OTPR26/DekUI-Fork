from model import *

def kokiri_appearance(base):
 # Keep all existing hand meshes. Replace only native sword/scabbard geometry;
 # preserve every visibility-group identity used by MM3D's equipment animations.
 sword_groups={5:5,6:5,7:5,13:13,15:13,17:13,12:12,14:12,16:12}
 templates={me[3]:me for me in base.meshes}
 weapon=lambda me:me[3] in sword_groups and me[2]<42 and me[2] not in (2,3)
 meshes=[me for me in base.meshes if not weapon(me)]
 for target,source in sword_groups.items():
  for me in base.meshes:
   if me[3]==source and weapon(me):
    out=bytearray(templates[target]);out[:3]=me[:3];out[3]=target;meshes.append(bytes(out))
 # Same native opaque/transparent ordering; shields are untouched.
 oldopaque=H(base.mesh_header,12);transparent={(H(me,0),me[2]) for me in base.meshes[oldopaque:]}
 meshes.sort(key=lambda me:((H(me,0),me[2]) in transparent,me[2]))
 msh=bytearray(base.mesh_header);put(msh,8,len(meshes));put(msh,12,sum((H(me,0),me[2]) not in transparent for me in meshes),'H');msh.extend(b''.join(meshes));put(msh,4,len(msh))
 sk=base.chunks['sklm'];shp=sk[U(sk,12):];chunks={**base.chunks,'sklm':chunk(b'sklm',struct.pack('<II',16,16+len(msh))+msh+shp)}
 out=bytearray(base.data[:0x4c])
 for i,key in enumerate(['skl','qtrs','mats','tex','sklm','luts','vatr','indices','textures']):align(out,128 if key=='textures' else 4);put(out,0x24+i*4,len(out));out.extend(chunks[key])
 put(out,4,len(out));return bytes(out)
