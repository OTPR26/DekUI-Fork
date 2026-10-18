"""Portable counterpart of the Android model converter; Python standard library only."""
import math,struct
from binary import *
def identity():return [1. if i%5==0 else 0. for i in range(16)]
def mul(a,b):return [sum(a[(i//4)*4+k]*b[k*4+i%4] for k in range(4)) for i in range(16)]
def point(a,v,w=1.):return [sum(a[i*4+j]*v[j] for j in range(3))+a[i*4+3]*w for i in range(3)]
def inverse(a):
    rows=[[a[r*4+c] if c<4 else float(c-4==r) for c in range(8)] for r in range(4)]
    for c in range(4):
        p=max(range(c,4),key=lambda r:abs(rows[r][c]));rows[c],rows[p]=rows[p],rows[c];v=rows[c][c];require(abs(v)>1e-12)
        rows[c]=[x/v for x in rows[c]]
        for r in range(4):
            if r!=c:
                v=rows[r][c];rows[r]=[x-v*y for x,y in zip(rows[r],rows[c])]
    return [rows[i//4][4+i%4] for i in range(16)]
TYPES={0x1400:('b',1),0x1401:('B',1),0x1402:('h',2),0x1403:('H',2),0x1404:('i',4),0x1405:('I',4),0x1406:('f',4)}
class Model:
    def __init__(self,data):
        self.data=data;require(data[:4]==b'cmb ' and u(data,8) in (6,10));self.mm=u(data,8)==10;self.attrs=9 if self.mm else 8
        self.keys=['skl','qtrs','mats','tex','sklm','luts','vatr','indices','textures'] if self.mm else ['skl','mats','tex','sklm','luts','vatr','indices','textures']
        self.offsets={key:u(data,0x24+4*i) for i,key in enumerate(self.keys)};self.chunks={}
        for key,o in self.offsets.items():
            end=self.offsets['tex'] if key=='mats' else self.offsets['textures'] if key=='indices' else len(data) if key=='textures' else o+u(data,o+4)
            self.chunks[key]=take(data,o,end-o)
        stride=0x16c if self.mm else 0x15c;m=self.chunks['mats'];self.materials=[take(m,12+i*stride,stride) for i in range(u(m,8))];self.combiners=m[12+len(self.materials)*stride:]
        t=self.chunks['tex'];self.tex=[take(t,12+i*36,36) for i in range(u(t,8))]
        v=self.chunks['vatr'];self.buffers=[take(v,u(v,16+i*8),u(v,12+i*8)) for i in range(self.attrs)]
        sk=self.chunks['sklm'];mo=u(sk,8);so=u(sk,12);self.mesh_header=take(sk,mo,16);self.shape_header=take(sk,so,16)
        self.meshes=[take(sk,mo+16+i*(12 if self.mm else 4),12 if self.mm else 4) for i in range(u(sk,mo+8))]
        self.shapes=[]
        for i in range(u(sk,so+8)):
            at=so+h(sk,so+16+i*2);self.shapes.append(take(sk,at,u(sk,at+4)))
        self.world=[];s=self.chunks['skl']
        for i in range(u(s,8)):
            at=16+i*(44 if self.mm else 40);parent=signed(s,at+2);sx,sy,sz,rx,ry,rz,tx,ty,tz=struct.unpack('<9f',take(s,at+4,36))
            x=identity();x[5]=math.cos(rx);x[6]=-math.sin(rx);x[9]=math.sin(rx);x[10]=math.cos(rx)
            y=identity();y[0]=math.cos(ry);y[2]=math.sin(ry);y[8]=-math.sin(ry);y[10]=math.cos(ry)
            z=identity();z[0]=math.cos(rz);z[1]=-math.sin(rz);z[4]=math.sin(rz);z[5]=math.cos(rz)
            scale=identity();scale[0]=sx;scale[5]=sy;scale[10]=sz;local=mul(mul(mul(z,y),x),scale);local[3]=tx;local[7]=ty;local[11]=tz
            self.world.append(mul(self.world[parent],local) if parent>=0 else local)
    def values(self,sh,attr,n,dim):
        o=0x24+attr*28;scale=f(sh,o+4);typ=h(sh,o+8);mode=h(sh,o+10);require(typ in TYPES)
        if mode==1:return [[f(sh,o+12+c*4) for c in range(dim)] for _ in range(n)]
        require(mode==0);fmt,size=TYPES[typ];vals=struct.unpack('<'+fmt*(n*dim),take(self.buffers[attr],u(sh,o),n*dim*size))
        return [[vals[v*dim+c]*scale for c in range(dim)] for v in range(n)]
    def primitives(self,sh):
        result=[]
        for i in range(h(sh,8)):
            at=h(sh,(0x124 if self.mm else 0x108)+i*2);p=take(sh,at,u(sh,at+4));require(p[:4]==b'prms');bones=[h(p,u(p,16)+2*j) for j in range(h(p,14))];r=u(p,20);typ=h(p,r+16);fmt,size=TYPES[typ];n=h(p,r+20)
            indices=struct.unpack('<'+fmt*n,take(self.chunks['indices'],h(p,r+22)*2,n*size));result.append((at,p,h(p,12),bones,indices))
        return result
    def geometry(self,si):
        sh=self.shapes[si];pr=self.primitives(sh);n=1+max(max(p[4]) for p in pr);dim=h(sh,0x120 if self.mm else 0x104)
        return self.values(sh,0,n,3),self.values(sh,1,n,3),self.values(sh,7 if self.mm else 6,n,dim),self.values(sh,8 if self.mm else 7,n,dim),pr
MAP=list(range(13))+list(range(14,26));HEIGHT=6555.463137/3986.693367

def retarget(a,m,si):
    pos,norm,bi,bw,pr=a.geometry(si);out=[p[:] for p in pos];normal=[p[:] for p in norm];scale=identity();scale[0]=scale[5]=scale[10]=.62
    matrices=[mul(mul(m.world[MAP[b]],scale),inverse(a.world[b])) for b in range(25)]
    for _,_,mode,bones,indices in pr:
        for vi in sorted(set(indices)):
            if mode in (0,1):out[vi]=[x*.62 for x in pos[vi]]
            else:
                total=sum(bw[vi]);require(total>0);v=[0.]*3;n=[0.]*3
                for j in range(len(bi[vi])):
                    weight=bw[vi][j]/total
                    if not weight:continue
                    matrix=matrices[bones[int(bi[vi][j])]];pp=point(matrix,pos[vi]);nn=point(matrix,norm[vi],0)
                    for k in range(3):v[k]+=pp[k]*weight;n[k]+=nn[k]*weight
                out[vi]=v;length=math.sqrt(sum(x*x for x in n));normal[vi]=[x/length if length else 0. for x in n]
    return out,normal

def world_points(m,si):
    p,_,bi,_,pr=m.geometry(si);out=[v[:] for v in p]
    for _,_,mode,bones,indices in pr:
        if mode==2:continue
        for vi in set(indices):out[vi]=point(m.world[bones[0 if mode==0 else int(bi[vi][0])]],p[vi])
    return out

def hands(a,m,skin):
    candidates={};plan={};removed=set()
    for me in a.meshes:
        if me[2]!=skin:continue
        si=h(me,0);bones={b for pr in a.primitives(a.shapes[si]) for b in pr[3]};left=bones<={15,16}
        if left or bones<={19,20}:candidates[si]=(left,retarget(a,m,si)[0])
    for group in sorted({me[3] for me in m.meshes}):
        pts=[];ids=[];bones=set()
        for i,me in enumerate(m.meshes):
            if me[3]!=group or me[2] not in (2,3):continue
            bs={b for pr in m.primitives(m.shapes[h(me,0)]) for b in pr[3]}
            if not(bs<={16,17} or bs<={20,21}):continue
            pts+=world_points(m,h(me,0));ids.append(i);bones|=bs
        if not pts:continue
        pts=list(dict.fromkeys(tuple(p) for p in pts));left=bones<={16,17}
        def dist(x,y):return sum((a-b)**2 for a,b in zip(x,y))
        def score(pair):
            cp=pair[1][1];return sum(min(dist(x,y) for y in pts) for x in cp)/len(cp)+sum(min(dist(x,y) for y in cp) for x in pts)/len(pts)
        best=min((p for p in candidates.items() if p[1][0]==left),key=score)[0]
        plan.setdefault(best,[]).append(group);removed.update(ids)
    return plan,removed

def floats(rows):return b''.join(struct.pack('<'+'f'*len(row),*row) for row in rows)
def descriptor(start):return struct.pack('<IfHH4f',start,1.,0x1406,0,0,0,0,0)
def tunic(data,body,color):
    if color==0:return data
    require(color in (1,2) and body[:4]==b'cmab');m=Model(data);t=m.tex[1];size=u(t,0);require(size==8192 and h(t,8)==128 and h(t,10)==128)
    out=bytearray(data);at=m.offsets['textures']+u(t,16);src=u(body,28)+(2 if color==1 else 1)*size;out[at:at+size]=take(body,src,size);return bytes(out)

def convert(source,native,master=False,dark=False):
    gar=decompress(native);original=next(data for name,_,data in entries(gar) if name=='child/model/link_child.cmb');m=Model(original);a=Model(source)
    require(len(a.world)==25 and len(m.world)==26 and len(a.materials)==(10 if dark else 36) and len(m.materials)==42)
    if dark:require(len(a.meshes)==15 and len(a.tex)==9)
    hand,removed=hands(a,m,7 if dark else 22);groups={0:[12,14,16]} if dark else {16:[12,14,16],31:[5,6,7],42:[13,15,17]} if master else {}
    eq={h(me,0):groups[me[3]] for me in a.meshes if me[3] in groups and me[2]!=(7 if dark else 22)}
    base=[me for me in a.meshes if me[3] in ((4,6) if dark else (45,46)) or h(me,0) in hand];selected=base+[me for me in a.meshes if h(me,0) in eq]
    base_mats=sorted({me[2] for me in base});used=base_mats+sorted({me[2] for me in selected}-set(base_mats));matmap={v:len(m.materials)+i for i,v in enumerate(used)}
    buffers=[bytearray(v) for v in m.buffers];extra=[b'',b'',b'']+a.buffers[2:];offsets=[]
    for buf,src in zip(buffers,extra):offsets.append(align(buf));buf.extend(src)
    indices=bytearray(m.chunks['indices']);indexbase=align(indices,2);indices.extend(a.chunks['indices'])
    materials=[bytearray(v) for v in m.materials];combbase=1+max(h(ma,0x124+2*j) for ma in m.materials for j in range(u(ma,0x120)));comb=bytearray(m.combiners[:combbase*40])
    def textures(mats):return {signed(a.materials[mi],16+24*j) for mi in mats for j in range(3) if signed(a.materials[mi],16+24*j)>=0}
    base_tex=sorted(textures(base_mats));used_tex=base_tex+sorted(textures(used)-set(base_tex));texmap={v:len(m.tex)+i for i,v in enumerate(used_tex)};texdata=bytearray(m.chunks['textures']);tex=[bytearray(t) for t in m.tex]
    for ti in used_tex:
        t=bytearray(a.tex[ti]);old=u(t,16);put(t,16,align(texdata,128));texdata.extend(take(a.chunks['textures'],old,u(t,0)));tex.append(t)
    for mi in used:
        ma=bytearray(a.materials[mi])
        for j in range(3):
            at=16+j*24;ti=signed(ma,at)
            if ti>=0:put(ma,at,texmap[ti],2)
        for j in range(u(ma,0x120)):put(ma,0x124+2*j,h(ma,0x124+2*j)+combbase,2)
        ma.extend(m.materials[1][0x15c:0x168]);ma.extend(struct.pack('<I',(0xdad10000 if dark else 0xada10000)+mi));materials.append(ma)
    comb.extend(a.combiners)
    if dark:
        require(len(comb)%40==0);eye=materials[matmap[4]];put(eye,0x120,1);put(eye,0x124,len(comb)//40,2);direct=bytearray(40)
        for at in (0,2):put(direct,at,0x1e01,2)
        for at in (4,6):put(direct,at,1,2)
        for at in (8,10):put(direct,at,0x8579,2)
        for at in (12,14,16,24,26,28):put(direct,at,0x84c0,2)
        for at in (18,20,22):put(direct,at,0x300,2)
        for at in (30,32,34):put(direct,at,0x302,2)
        comb.extend(direct);eye[0x130]=1;eye[0x131]=127;put(eye,0x132,0x204,2)
    shapes=[bytearray(sh) for sh in m.shapes];templates={me[3]:me for me in m.meshes};meshes=[];swords={5,6,7,12,13,14,15,16,17}
    for i,me in enumerate(m.meshes):
        if me[3] in range(28,34) or i in removed:continue
        if (master or dark) and me[3] in ({12,14,16} if dark else swords) and me[2] not in (2,3):continue
        meshes.append(bytearray(me))
    for si in sorted({h(me,0) for me in meshes}):
        pos,_,_,_,pr=m.geometry(si);require(all(p[2] in (0,1) for p in pr));at=align(buffers[0]);buffers[0].extend(floats([[v/HEIGHT for v in row] for row in pos]));shapes[si][0x24:0x40]=descriptor(at)
    for me in selected:
        si=h(me,0);mat=me[2];vis=me[3];targets=eq.get(si,hand.get(si,[28,29] if vis==(4 if dark else 45) else [33]));source_sh=a.shapes[si];pos,norm=retarget(a,m,si)
        tangent=bytearray(descriptor(0));put(tangent,10,1,2);sh=bytearray(source_sh[:0x5c]+tangent+source_sh[0x5c:]);flags=h(sh,10);put(sh,10,(flags&3)|((flags&~3)<<1),2)
        for attr in range(9):
            if attr!=2:
                at=0x24+attr*28;put(sh,at,u(sh,at)+offsets[attr])
        for attr,values in ((0,pos),(1,norm)):
            at=align(buffers[attr]);buffers[attr].extend(floats(values));sh[0x24+attr*28:0x40+attr*28]=descriptor(at)
        for j,(at,p,_,bones,_) in enumerate(a.primitives(source_sh)):
            no=at+28;put(sh,0x124+2*j,no,2);bo=no+u(p,16)
            for k,b in enumerate(bones):put(sh,bo+2*k,MAP[b],2)
            po=no+u(p,20);start=h(sh,po+22)+indexbase//2;require(start<=65535);put(sh,po+22,start,2)
        put(sh,4,len(sh));newsi=len(shapes);shapes.append(sh)
        for group in targets:
            out=bytearray(templates[group]);put(out,0,newsi,2);out[2]=matmap[mat];meshes.append(out)
    if not master and not dark:
        replacements={5:5,6:5,7:5,13:13,15:13,17:13,12:12,14:12,16:12}
        def weapon(me):return me[3] in replacements and me[2]<42 and me[2] not in (2,3)
        before=meshes[:];meshes=[me for me in meshes if not weapon(me)]
        for target,src in replacements.items():
            for me in before:
                if me[3]==src and weapon(me):
                    out=bytearray(templates[target]);out[:3]=me[:3];out[3]=target;meshes.append(out)
    transparent={(h(me,0),me[2]) for me in m.meshes[h(m.mesh_header,12):]}
    def translucent(me):return (h(me,0),me[2]) in transparent
    meshes.sort(key=lambda me:(translucent(me),me[2]));msh=bytearray(m.mesh_header);put(msh,8,len(meshes));put(msh,12,sum(not translucent(me) for me in meshes),2);msh.extend(b''.join(meshes));put(msh,4,len(msh))
    shp=bytearray(m.shape_header);put(shp,8,len(shapes));shp.extend(bytes(len(shapes)*2));align(shp)
    for i,sh in enumerate(shapes):require(len(shp)<65536);put(shp,16+2*i,len(shp),2);shp.extend(sh);align(shp)
    put(shp,4,len(shp));sklm=chunk(b'sklm',struct.pack('<II',16,16+len(msh))+msh+shp)
    vatr=bytearray(b'vatr'+bytes(80));put(vatr,8,len(buffers[0])//12)
    for i,b in enumerate(buffers):align(vatr);put(vatr,12+8*i,len(b));put(vatr,16+8*i,len(vatr));vatr.extend(b)
    put(vatr,4,len(vatr));mats=bytearray(chunk(b'mats',struct.pack('<I',len(materials))+b''.join(materials)+comb));put(mats,4,12+len(materials)*(0x16c+0x24))
    chunks=dict(m.chunks,sklm=sklm,vatr=vatr,mats=mats,tex=chunk(b'tex ',struct.pack('<I',len(tex))+b''.join(tex)),indices=indices,textures=texdata)
    out=bytearray(m.data[:0x4c]);put(out,0x20,len(indices)//2)
    for i,key in enumerate(m.keys):align(out,128 if key=='textures' else 4);put(out,0x24+4*i,len(out));out.extend(chunks[key])
    put(out,4,len(out));check=Model(bytes(out));require(check.chunks['skl']==m.chunks['skl']);rebuilt=repack(gar,out);packed=literals(rebuilt,native);require(decompress(packed)==rebuilt)
    return packed
