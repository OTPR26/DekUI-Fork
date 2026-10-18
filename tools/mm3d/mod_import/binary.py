"""Bounded, read-only parsing of user-owned assets. No game payloads included."""
import struct

def require(ok,message='Unsupported or damaged asset'):
    if not ok:raise ValueError(message)
def take(d,o,n):
    require(o>=0 and n>=0 and o+n<=len(d),'Asset range is outside file')
    return bytes(d[o:o+n])
def u(d,o):return struct.unpack('<I',take(d,o,4))[0]
def h(d,o):return struct.unpack('<H',take(d,o,2))[0]
def signed(d,o):return struct.unpack('<h',take(d,o,2))[0]
def f(d,o):return struct.unpack('<f',take(d,o,4))[0]
def put(d,o,v,n=4):
    require(0<=o<=len(d)-n);d[o:o+n]=int(v&((1<<(8*n))-1)).to_bytes(n,'little')
def align(b,n=4):
    b.extend(bytes((-len(b))%n));return len(b)
def chunk(tag,payload):return tag+struct.pack('<I',8+len(payload))+payload

def entries(d):
    require(d[:4] in (b'GAR\2',b'ZAR\1') and u(d,4)==len(d))
    mm=d[:4]==b'GAR\2';meta=u(d,16);offsets=u(d,20);result=[]
    for i in range(h(d,10)):
        at=meta+i*(12 if mm else 8);np=u(d,at+(8 if mm else 4));end=d.find(b'\0',np)
        require(0<=np<end<len(d));offset=u(d,offsets+i*4)
        result.append((take(d,np,end-np).decode(),offset,take(d,offset,u(d,at))))
    return result

def decompress(d):
    require(d[:4]==b'LzS\1' and u(d,12)==len(d)-16)
    size=u(d,8);require(0<size<=64*1024*1024);out=bytearray();ring=bytearray(4096);write=0xfee;at=16
    while len(out)<size:
        flags=take(d,at,1)[0];at+=1
        for bit in range(8):
            if len(out)==size:break
            if flags&(1<<bit):values=take(d,at,1);at+=1
            else:
                a,b=take(d,at,2);at+=2;read=a|((b&240)<<4);values=None
                for _ in range((b&15)+3):
                    require(len(out)<size);v=ring[read];read=(read+1)&4095;out.append(v);ring[write]=v;write=(write+1)&4095
            if values:
                v=values[0];out.append(v);ring[write]=v;write=(write+1)&4095
    return bytes(out)

def literals(raw,header):
    out=bytearray(header[:8]+struct.pack('<II',len(raw),len(raw)+(len(raw)+7)//8))
    for at in range(0,len(raw),8):
        block=raw[at:at+8];out.append((1<<len(block))-1);out.extend(block)
    return bytes(out)

def repack(gar,model):
    es=entries(gar);out=bytearray(gar[:min(e[1] for e in es)])
    for i,(name,_,data) in enumerate(es):
        align(out);put(out,u(gar,20)+4*i,len(out));data=model if name=='child/model/link_child.cmb' else data
        put(out,u(gar,16)+12*i,len(data));out.extend(data)
    put(out,4,len(out));return bytes(out)

class Reader:
    def __init__(self,path):self.file=open(path,'rb');self.size=self.file.seek(0,2)
    def close(self):self.file.close()
    def read(self,at,size):
        require(at>=0 and size>=0 and at+size<=self.size,'ROM range is outside selected file')
        self.file.seek(at);d=self.file.read(size);require(len(d)==size);return d
    def actor(self,name,oot=False):
        header=self.read(0,512);require(header[0x100:0x104] in (b'NCSD',b'NCCH'),'Select a decrypted .3ds/.cci dump')
        ncch=u(header,0x120)*512 if header[0x100:0x104]==b'NCSD' else 0;part=self.read(ncch,512)
        require(part[0x100:0x104]==b'NCCH' and (part[0x18f]&4),'ROM must be decrypted')
        title=int.from_bytes(part[0x118:0x120],'little')
        require(title in (0x0004000000033500,0x0004000000033600,0x0004000000033400) if oot else title==0x0004000000125500,'Select the appropriate OoT3D or MM3D ROM')
        fs=ncch+u(part,0x1b0)*512;ivfc=self.read(fs,96);require(ivfc[:4]==b'IVFC')
        level=fs+0x1000;hdr=self.read(level,40);require(u(hdr,0)==40)
        # Level 3 begins at the first 4 KiB boundary after the IVFC header/master hash.
        require(u(hdr,16)<=64*1024*1024 and u(hdr,32)<=64*1024*1024,'RomFS metadata tables are too large')
        dirs=self.read(level+u(hdr,12),u(hdr,16));files=self.read(level+u(hdr,28),u(hdr,32));base=level+u(hdr,36)
        seen=set();found=[]
        def walk(off,path,depth=0):
            require(depth<40 and off not in seen,'Invalid RomFS directory tree');seen.add(off)
            nl=u(dirs,off+20);current=path+take(dirs,off+24,nl).decode('utf-16le');prefix=current+'/' if current else ''
            ent=u(dirs,off+12);file_seen=set()
            while ent!=0xffffffff:
                require(ent not in file_seen);file_seen.add(ent);length=u(files,ent+28)
                if prefix+take(files,ent+32,length).decode('utf-16le')==name:
                    offset=int.from_bytes(take(files,ent+8,8),'little');size=int.from_bytes(take(files,ent+16,8),'little')
                    require(size<=64*1024*1024);found.append(self.read(base+offset,size))
                ent=u(files,ent+4)
            child=u(dirs,off+8)
            while child!=0xffffffff:child=walk(child,prefix,depth+1)
            return u(dirs,off+4)
        walk(0,'');require(len(found)==1,'Required ROM asset is missing: '+name);return found[0]
