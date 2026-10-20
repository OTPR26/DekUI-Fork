#!/usr/bin/env python3
"""Compile reviewed English wording into guarded in-memory replacements.
Reads the user's GMSG; never changes game assets. Control operands are aligned
relative to the message, as in the game's byte-text reader.
"""
from dialogue_recipe import recipe
import argparse, json, re, struct
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
SPEC = ROOT / 'resources/mm3d/text/single-screen.json'
# Commands present in the reviewed messages. Unknown commands fail closed.
ARGS = {0:0, 1:0, 2:0, 0x25:1, 0x27:0, 0x28:0, 0x29:1,
        0x2b:1, 0x31:1, 0x3a:1, 0x1e:0, 0x26:1, 0x2c:0,
        0x2f:1, 0x33:0, 0x34:0, 0x35:0, 0x36:0, 0x37:0}
ICONS = {'A':1, 'B':2, 'X':3, 'Y':4, 'L':5, 'R':6, 'Dpad':0x0a, 'Stick':0x11}

def decode(raw):
    result=[];i=0
    while i<len(raw):
        if raw[i]!=127:
            start=i
            while i<len(raw) and raw[i]!=127:i+=1
            result.append(raw[start:i].decode('utf-8'));continue
        i+=1
        if i&1:
            if raw[i]!=0:raise ValueError('Bad control alignment')
            i+=1
        command=struct.unpack_from('<H',raw,i)[0];i+=2
        if command==0x20:
            arg=struct.unpack_from('<H',raw,i)[0];i+=2
            end=raw.index(0,i);one=raw[i:end];i=end+1
            end=raw.index(0,i);two=raw[i:end];i=end+1
            if i&1:
                if raw[i]!=0:raise ValueError('Bad plural alignment')
                i+=1
            result.append((command,arg,one,two));continue
        if command==0x32:
            if i!=4:
                if raw[i:i+2]!=b'\0\0':raise ValueError('Bad sound alignment')
                i+=2
            result.append((command,struct.unpack_from('<I',raw,i)[0]));i+=4
            continue
        if command not in ARGS:raise ValueError(f'Unsupported command {command:x}')
        n=ARGS[command];values=struct.unpack_from('<'+'H'*n,raw,i);i+=2*n
        result.append((command,*values))
        if command==0:
            if any(raw[i:]):raise ValueError('Data beyond terminator')
            break
    if not result or result[-1]!=(0,):raise ValueError('Missing end command')
    return result

def encode(tokens):
    result=bytearray()
    for token in tokens:
        if isinstance(token,str):result.extend(token.encode('utf-8'));continue
        result.append(127)
        if len(result)&1:result.append(0)
        result.extend(struct.pack('<H',token[0]))
        if token[0]==0x20:
            result.extend(struct.pack('<H',token[1])+token[2]+b'\0'+token[3]+b'\0')
            if len(result)&1:result.append(0)
        elif token[0]==0x32:
            if len(result)!=4:result.extend(b'\0\0')
            result.extend(struct.pack('<I',token[1]))
        else:result.extend(struct.pack('<'+'H'*(len(token)-1),*token[1:]))
    return bytes(result)

def markup(text):
    tokens=[]
    for part in re.split(r'(\{[^}]+\}|\n)',text):
        if not part:continue
        if part=='\n':tokens.append((1,))
        elif part.startswith('{Color:'):tokens.append((0x3a,int(part[7:-1])))
        elif part.startswith('{Icon:'):tokens.append((0x25,int(part[6:-1])))
        elif part.startswith('{'):tokens.append((0x25,ICONS[part[1:-1]]))
        else:tokens.append(part)
    return tokens

def messages(data, language=2):
    if data[:8]!=b'GMSG\x01\0\0\0':raise ValueError('Unsupported GMSG')
    count,languages=struct.unpack_from('<II',data,8)
    if languages!=language or count>20000:raise ValueError('Unexpected GMSG language')
    result={}
    for i in range(count):
        at=16+20*i;mid=struct.unpack_from('<H',data,at)[0];offset,size=struct.unpack_from('<II',data,at+12)
        if offset<16+20*count or offset+size>len(data) or offset&1 or mid in result:raise ValueError('Invalid entry')
        result[mid]=data[offset:offset+size]
    return result

def fnv(data):
    value=0xcbf29ce484222325
    for b in data:value=((value^b)*0x100000001b3)&0xffffffffffffffff
    return value

def visual_runs(tokens):
    """Never join text across timing, choice, dynamic-value or layout commands."""
    start=None
    for i,t in enumerate(tokens+[(0,)]):
        visual=isinstance(t,str) or t[0] in (1,0x25,0x3a)
        if visual and start is None:start=i
        if not visual and start is not None:
            yield start,i,tokens[start:i]
            start=None

def show_run(tokens):
    return ''.join(t if isinstance(t,str) else '\n' if t[0]==1 else
                   '{Color:'+str(t[1])+'}' if t[0]==0x3a else
                   '{Icon:'+str(t[1])+'}' for t in tokens)

def compile_messages(data,spec):
    originals=messages(data,spec.get('language',2) if isinstance(spec.get('language',2),int) else 2);edited={}
    for edit in spec['edits']:
        mid=int(edit['id'],16);tokens=edited.setdefault(mid,decode(originals[mid]))
        start=edit['token_index']
        if start>=len(tokens) or not isinstance(tokens[start],str) or fnv(tokens[start].encode())!=int(edit['token_hash'],16):
            raise ValueError(f'{mid:04x}: reviewed source changed')
        prefix=tokens[start][:edit['prefix_length']]
        end=start+1
        while end<len(tokens) and not (isinstance(tokens[end],tuple) and tokens[end][0] in (0,2,0x2b,0x31)):end+=1
        tokens[start:end]=([prefix] if prefix else [])+markup(edit['text'])
    grouped={}
    for edit in spec.get('neutral_runs',[]):grouped.setdefault(int(edit['id'],16),[]).append(edit)
    for mid,edits in grouped.items():
        tokens=edited.setdefault(mid,decode(originals[mid]))
        runs=list(visual_runs(tokens))
        if len({e['run'] for e in edits})!=len(edits):raise ValueError('Duplicate run')
        for edit in sorted(edits,key=lambda e:e['run'],reverse=True):
            start,end,old=runs[edit['run']]
            if fnv(show_run(old).encode())!=int(edit['source_hash'],16):raise ValueError(f'{mid:04x}: reviewed source changed')
            new=markup(edit['text'])
            if sum(t==(1,) for t in new)>sum(t==(1,) for t in old):
                raise ValueError(f'{mid:04x}: extra line breaks')
            if edit['text'].startswith('\n\n') or '\n\n' in edit['text']:
                raise ValueError(f'{mid:04x}: extra paragraph')
            tokens[start:end]=new
    result=[]
    for mid,tokens in sorted(edited.items()):
        raw=originals[mid];out=encode(tokens)
        if len(out)>len(raw):raise ValueError(f'{mid:04x}: replacement exceeds original allocation')
        # Dialogue flow, item fanfare/delay and page boundaries must stay native.
        flow=lambda ts:[t for t in ts if isinstance(t,tuple) and t[0] not in (1,0x25,0x3a)]
        if flow(tokens)!=flow(decode(raw)):raise ValueError(f'{mid:04x}: dialogue flow changed')
        if decode(out)!=tokens:raise ValueError('Control round trip failed')
        result.append((mid,len(raw),fnv(raw),out,tokens,raw))
    return result

def header(rows):
    lines=['// SPDX-License-Identifier: GPL-2.0-or-later', '// Generated by tools/mm3d/single_screen_text.py. Do not edit.', '#pragma once', '#include <array>', '#include <cstdint>', '#include "mm3d/dialogue_recipe.h"', 'namespace MM3D::SingleScreenText {', 'struct Replacement {std::uint16_t id;std::uint32_t size;std::uint64_t hash;DialogueText::Recipe text;DialogueText::Recipe original;std::uint64_t edited_hash;};', f'inline constexpr std::array<Replacement,{len(rows)}> Replacements{{{{']
    for mid,size,h,data,_,original in rows:
        edited_hash=fnv(data+bytes(size-len(data)))
        lines.append(f'    {{0x{mid:04x},{size},0x{h:016x}ULL,{recipe(original,data)},{recipe(original)},0x{edited_hash:016x}ULL}},')
    return '\n'.join(lines+['}};', '}',''])

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('gmsg',type=Path);p.add_argument('--header',type=Path);p.add_argument('--review',type=Path);a=p.parse_args()
    spec=json.loads(SPEC.read_text())
    rows=compile_messages(a.gmsg.read_bytes(),spec)
    if a.header:
        basic=compile_messages(a.gmsg.read_bytes(),dict(spec,neutral_runs=[]))
        extra=header(basic)
        extra=extra[extra.index('inline constexpr'):].replace(' Replacements',' SingleScreenReplacements')
        a.header.write_text(header(rows)+'\nnamespace MM3D::SingleScreenText {\n'+extra)
    if a.review:
        def show(tokens):
            return ''.join(t if isinstance(t,str) else '\n' if t[0]==1 else '\n\n' if t[0]==2 else '{'+next((k for k,v in ICONS.items() if v==t[1]),str(t[1]))+'}' if t[0]==0x25 else '' for t in tokens)
        a.review.write_text('\n\n'.join(f'{mid:04x} ({len(data)}/{size} bytes)\n{show(tokens)}' for mid,size,_,data,tokens,_ in rows)+'\n')
    print(f'Compiled {len(rows)} guarded replacements')
