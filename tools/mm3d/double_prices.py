#!/usr/bin/env python3
"""Build reviewed US-English price edits without modifying game assets."""
from dialogue_recipe import recipe, replace_reviewed
import argparse,json,re,struct
from pathlib import Path
from single_screen_text import messages,fnv
ROOT=Path(__file__).resolve().parents[2]
ARGS={0:0,1:0,2:0,0x34:0,0x1e:0,0x25:1,0x27:0,0x28:0,0x29:1,0x2b:1,0x2c:0,0x2f:1,0x31:1,0x3a:1}
def decode(raw):
    ts=[];i=0
    while i<len(raw):
        if raw[i]!=127:
            start=i
            while i<len(raw) and raw[i]!=127:i+=1
            ts.append(raw[start:i].decode('utf-8'));continue
        i+=1
        if i&1:
            assert raw[i]==0;i+=1
        cmd=struct.unpack_from('<H',raw,i)[0];i+=2
        if cmd==0x20:
            arg=struct.unpack_from('<H',raw,i)[0];i+=2
            end=raw.index(0,i);one=raw[i:end];i=end+1
            end=raw.index(0,i);two=raw[i:end];i=end+1
            if i&1:assert raw[i]==0;i+=1
            ts.append((cmd,arg,one,two))
        elif cmd==0x32:
            if i!=4:assert raw[i:i+2]==b'\0\0';i+=2
            ts.append((cmd,struct.unpack_from('<I',raw,i)[0]));i+=4
        else:
            n=ARGS[cmd];ts.append((cmd,*struct.unpack_from('<'+'H'*n,raw,i)));i+=2*n
        if cmd==0:
            assert not any(raw[i:]);return ts
    raise ValueError('Missing terminator')
def encode(ts):
    out=bytearray()
    for t in ts:
        if isinstance(t,str):out.extend(t.encode());continue
        out.append(127)
        if len(out)&1:out.append(0)
        out.extend(struct.pack('<H',t[0]))
        if t[0]==0x20:
            out.extend(struct.pack('<H',t[1])+t[2]+b'\0'+t[3]+b'\0')
            if len(out)&1:out.append(0)
        elif t[0]==0x32:
            if len(out)!=4:out.extend(b'\0\0')
            out.extend(struct.pack('<I',t[1]))
        else:out.extend(struct.pack('<'+'H'*(len(t)-1),*t[1:]))
    return bytes(out)
def compile_data(data,spec,multiplier=2):
    assert multiplier in (2,3)
    capped={0x1600,0x1606,0x2afa,0x2b0b,0x06ba,0x06bb,0x06d7,0x06d8}
    def scale(mid,n):
        value=n*multiplier
        return min(value,500) if mid in capped and n<=500 else value
    originals=messages(data);entries={}
    for i in range(struct.unpack_from('<I',data,8)[0]):
        at=16+20*i;mid=struct.unpack_from('<H',data,at)[0]
        entries[mid]=struct.unpack_from('<I',data,at+4)[0]
    metadata={int(x,16) for x in spec['metadata_ids']};literal={int(x,16) for x in spec['literal_ids']}
    specials={int(k,16):v for k,v in spec['special_literals'].items()}
    rows=[]
    for mid in sorted(metadata|literal|specials.keys()):
        raw=originals[mid];ts=decode(raw);assert encode(ts)==raw,(hex(mid),'round trip')
        edited=[]
        for t in ts:
            if isinstance(t,str):
                def price(m):
                    v=m[1];n=int(v.replace(',',''))
                    value=scale(mid,n)
                    if mid in specials:
                        value=(100-10*multiplier) if n==90 else (10*multiplier if n==10 else n)
                    replacement=f'{value:,}' if ',' in v else str(value)
                    return replacement+' Rupee'+m[2]
                for rule in spec.get('rewrites',{}).get(f'{mid:04x}',[]):
                    new=rule['text']
                    t,_=replace_reviewed(t,rule,new.replace('twice','three times') if multiplier==3 else new)
                t=re.sub(r'\b([0-9][0-9,]*) Rupee(s?)\b',price,t)
            edited.append(t)
        if len(encode(edited))>len(raw):
            swaps=spec.get('shorten',{}).get(f'{mid:04x}',[])
            for rule in swaps:
                matches=0
                for i,t in enumerate(edited):
                    if isinstance(t,str):
                        edited[i],count=replace_reviewed(t,rule,rule['text']);matches+=count
                assert matches,hex(mid)
        new=encode(edited)
        # Collapse adjacent colour changes with no text between them.
        if len(new)>len(raw):
            compact=[]
            for t in edited:
                if isinstance(t,tuple) and t[0]==0x3a and compact and isinstance(compact[-1],tuple) and compact[-1][0]==0x3a:compact.pop()
                compact.append(t)
            edited=compact;new=encode(edited)
        if len(new)>len(raw):
            print(f'OVERFLOW {mid:04x}: {len(new)}/{len(raw)}',repr(edited));continue
        assert decode(new)==edited,(hex(mid),'edited round trip')
        flow=lambda tokens:[t for t in tokens if not isinstance(t,str) and t[0]!=0x3a]
        assert flow(ts)==flow(edited),(hex(mid),'dialogue flow changed')
        value=entries[mid];updated=value
        if mid in metadata:
            for shift in (0,10,20):
                n=(value>>shift)&1023
                if n!=1023:
                    assert scale(mid,n)<1023
                    updated=(updated&~(1023<<shift))|(scale(mid,n)<<shift)
        rows.append((mid,raw,new,value,updated))
    if len(rows)!=len(metadata|literal|specials.keys()):raise ValueError('Text needs reviewed shortening')
    assert not ({mid for mid,*_ in rows}&{int(x,16) for x in spec['exempt_ids']})
    return rows
def hooks_header(spec):
    lines=['// SPDX-License-Identifier: GPL-2.0-or-later','// Generated by tools/mm3d/double_prices.py; do not edit.','#pragma once','#include <array>','#include <cstdint>','namespace MM3D::Prices {','enum class Kind {Funds,Charge,NetReward,Fallback};','struct Site {std::uint32_t pc,word;Kind kind;std::array<std::uint32_t,5> guards;};',f'inline constexpr std::array<Site,{len(spec)}> Sites{{{{']
    for r in spec:
        lines.append('    {0x'+r['pc']+',0x'+r['word']+',Kind::'+r['kind']+',{'+','.join('0x'+x for x in r['guards'])+'}}, // '+r['name'])
    return '\n'.join(lines+['}};','}',''])

def audit_code(data,spec):
    expected={int(r['pc'],16) for r in spec}
    actual=set()
    for at in range(0,len(data)-3,4):
        word=struct.unpack_from('<I',data,at)[0]
        if word&0x0e000000!=0x0a000000:continue
        offset=word&0xffffff
        if offset&0x800000:offset-=0x1000000
        if 0x100000+at+8+offset*4==0x2c1634:actual.add(0x100000+at)
    assert actual==expected,('Unclassified/changed rupee calls',actual^expected)
    hooks=json.loads((ROOT/'resources/mm3d/text/price-hooks.json').read_text())
    for site in hooks:
        pc=int(site['pc'],16)
        for j,value in enumerate(site['guards']):
            assert struct.unpack_from('<I',data,pc-0x100000-8+j*4)[0]==int(value,16),(site['name'],j)
    charge_sites={int(r['pc'],16) for r in hooks if r['kind'] in ('Charge','NetReward')}
    assert charge_sites=={int(r['pc'],16) for r in spec if r['handling'] in ('Charge','NetReward')}
    print('All',len(actual),'native rupee call sites classified and hook signatures verified')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('gmsg',type=Path);p.add_argument('--header',type=Path);p.add_argument('--hooks-header',type=Path);p.add_argument('--code',type=Path);a=p.parse_args()
    spec=json.loads((ROOT/'resources/mm3d/text/double-prices.json').read_text());rows=compile_data(a.gmsg.read_bytes(),spec);triples=compile_data(a.gmsg.read_bytes(),spec,3)
    if a.header:
        lines=['// SPDX-License-Identifier: GPL-2.0-or-later','// Generated by tools/mm3d/double_prices.py; do not edit.','#pragma once','#include <array>','#include <cstdint>','#include "mm3d/dialogue_recipe.h"','namespace MM3D::Prices {','struct MessageEdit {std::uint16_t id;DialogueText::Recipe original,text;std::uint32_t metadata,updated;std::uint64_t hash,edited_hash;};',f'inline constexpr std::array<MessageEdit,{len(rows)}> Messages{{{{']
        for mid,raw,new,oldmeta,newmeta in rows:
            lines.append(f'{{0x{mid:04x},{recipe(raw)},{recipe(raw,new)},0x{oldmeta:x},0x{newmeta:x},0x{fnv(raw):x}ULL,0x{fnv(new+bytes(len(raw)-len(new))):x}ULL}},')
        lines+=['}};',f'inline constexpr std::array<MessageEdit,{len(triples)}> TripleMessages{{{{']
        for mid,raw,new,oldmeta,newmeta in triples:
            lines.append(f'{{0x{mid:04x},{recipe(raw)},{recipe(raw,new)},0x{oldmeta:x},0x{newmeta:x},0x{fnv(raw):x}ULL,0x{fnv(new+bytes(len(raw)-len(new))):x}ULL}},')
        a.header.write_text('\n'.join(lines+['}};','}','']))
    if a.hooks_header:a.hooks_header.write_text(hooks_header(json.loads((ROOT/'resources/mm3d/text/price-hooks.json').read_text())))
    if a.code:audit_code(a.code.read_bytes(),json.loads((ROOT/'resources/mm3d/text/price-transactions.json').read_text()))
    print('Compiled',len(rows),'price messages')
