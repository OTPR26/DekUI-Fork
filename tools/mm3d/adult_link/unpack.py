from pathlib import Path
import struct,json
# Grezzo LzS: LSB-first flags, 4 KiB sliding dictionary, 12-bit dictionary
# address plus 4-bit length. Literal chunks can also be packed losslessly.
def decompress(data):
 assert data[:4]==b'LzS\1';_,size,compressed=struct.unpack_from('<III',data,4);assert compressed+16==len(data)
 ring=bytearray(4096);write=0xfee;out=bytearray();i=16
 while i<len(data):
  flags=data[i];i+=1
  for bit in range(8):
   if i>=len(data):break
   if flags&(1<<bit):chunk=[data[i]];i+=1
   else:
    a,b=data[i:i+2];i+=2;pos=a|((b&240)<<4);chunk=[]
    for j in range((b&15)+3):
     value=ring[pos];pos=(pos+1)&4095;ring[write]=value;write=(write+1)&4095;out.append(value)
    continue
   for value in chunk:ring[write]=value;write=(write+1)&4095;out.append(value)
 assert len(out)==size,(len(out),size)
 return bytes(out)
def entries(data):
 assert data[:4] in (b'GAR\2',b'ZAR\1');mm=data[:4]==b'GAR\2'
 size,types,n,typ,meta,offsets=struct.unpack_from('<IHHIII',data,4);assert size==len(data)
 for i in range(n):
  at=meta+i*(12 if mm else 8);length=struct.unpack_from('<I',data,at)[0];nameoff=struct.unpack_from('<I',data,at+(8 if mm else 4))[0];name=data[nameoff:data.index(0,nameoff)].decode();off=struct.unpack_from('<I',data,offsets+i*4)[0]
  yield name,off,data[off:off+length]
