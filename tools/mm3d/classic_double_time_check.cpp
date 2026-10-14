// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/classic_double_time.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
using namespace MM3D::ClassicDoubleTime;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at){auto i=words.find(at);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
    void Write(U32 at,U32 v){words[at]=v;}
};
constexpr U32 self=0x9000000,base=0x9100000,window=0x9200000,play=0x9300000;
unsigned language=2;
std::string_view original_text=OriginalText;
void Resource(Memory& m) {
    const U32 length=original_text.size(), total=(38+length+3)&~3u;
    m.Write(self+4,base);m.Write(self+8,base+16);m.Write(self+12,total);m.Write(self+16,20);
    m.Write(base,0x47534d47);m.Write(base+4,1);m.Write(base+8,1);m.Write(base+12,language);
    m.Write(base+16,0x1b8a);m.Write(base+20,0x3fffffff);m.Write(base+28,38);m.Write(base+32,length);
    for(U32 at=base+36;at<base+total;at+=4)m.Write(at,0xa5a5a5a5);
    for(unsigned i=0;i<length;++i)Byte(m,base+38+i,static_cast<unsigned char>(original_text[i]));
}
int main(int argc,char** argv) {
    // Every minute-code, including midnight and exact 6 AM/6 PM boundaries.
    for(unsigned day=1;day<=3;++day)for(unsigned time=0;time<=65535;++time) {
        auto d=Next(day,time);bool night=time<0x4000||time>=0xc000;
        assert(bool(d)==!(day==3&&night));
        if(d){assert(d->question==(day-1)*2+unsigned(night));assert(d->time==(night?0x4000u:0xc000u));assert(d->dawn==night);}
    }
    assert(!Next(0,0));assert(!Next(4,0));assert(!Next(1,65536));
    for(unsigned lang:{2u,4u,8u}) {
    language=lang;original_text=OriginalText;
    for(const auto& d:MM3D::ClassicLocalizedText::Dialogues)if(d.language==lang)original_text=d.song;
    Memory m;Resource(m);const auto original=m.words;
    for(int i=0;i<5;++i){assert(Text(m,self,i));assert(Text(m,self,i));assert(Text(m,self,-1));assert(m.words==original);}
    m.Write(base+40,0xdeadbeef);auto corrupt=m.words;assert(!Text(m,self,0));assert(m.words==corrupt);
    Resource(m);m.Write(base+20,0);corrupt=m.words;assert(!Text(m,self,0));assert(m.words==corrupt);
    for(bool on:{false,true})for(unsigned day=1;day<=3;++day)for(U32 time:{0u,0x3fffu,0x4000u,0xbfffu,0xc000u,0xffffu})for(bool yes:{false,true}) {
        m={};Resource(m);Hook h;h.Message(m,self,0x1b7f);enabled=on;
        m.Write(window+0x444,play);m.Write(window+0x3f4,0x1b8a);m.Write(window+0x3fc,0xffffffff);m.Write(window+0x428,13);
        m.Write(play+0x13c,0x402);m.Write(play+0x8364,0x0003abcd);
        m.Write(Common+0x14,day);m.Write(Common+0x20,0xbeef0000|time);
        m.Write(Common+0x36a0,0x5555abcd);m.Write(Common+0x140f0,0x5555abcd);
        m.Write(0x7781d8+0x6bc,0x123456);m.Write(0x7781d8+0x540,0x12345678);
        std::array<U32,16> r{};r[4]=window;r[15]=Sites[0][0]+4;
        auto before=m.words;h.Handle(m,r,0);
        auto d=Next(day,time);
        if(!on){assert(m.words==before);assert(r[1]==0x66666667);assert(!h.target);continue;}
        if(!d){assert(r[15]==0x1d82f8);assert(m.words==before);assert(!h.target);continue;}
        assert(r[15]==0x1d8114&&r[0]==play&&r[1]==0x1b8a&&r[2]==0);
        h.Message(m,self,0x1b8a);h.Handle(m,r,1);assert(r[0]==0x14);
        Half(m,play+0x8366,r[0]);m.Write(window+0x430,yes?0:1);
        // Active ID owns the question even when no next message is pending.
        assert(h.Owns(m,window));
        m.Write(window+0x3f4,0x1b8c);m.Write(window+0x3fc,0x1b8a);
        assert(!h.Owns(m,window)); // A queued ID cannot claim another dialogue.
        m.Write(window+0x3f4,0x1b8a);m.Write(window+0x3fc,0xffffffff);
        // Changing the checkbox mid-dialogue must not strand its native state.
        enabled=false;r[0]=play+0x8300;h.Handle(m,r,2);
        if(yes){assert(r[15]==0x60451c);r[5]=Common;h.Handle(m,r,3);assert(r[15]==0x6045a8);
            assert(m.Read(Common+0x36a0)==((time<<16)|0xabcd));
            assert(m.Read(Common+0x140f0)==((d->time<<16)|0xabcd));
            assert(m.Read(play+0x8364)==0x001aabcd);
            assert(m.Read(0x7781d8+0x6bc)==(d->dawn?0x01123456u:0x123456u));
        }else {assert(r[15]==0x604560);assert(m.Read(Common+0x140f0)==0x5555abcd);}
        assert(!h.target);assert(m.Read(Common+0x14)==day);assert(m.Read(Common+0x20)==(0xbeef0000|time));
        h.Message(m,self,0x1b8a); // Subsequent Song of Time restores native text.
        for(auto [at,value]:original)assert(m.Read(at)==value);
    }
    // Disabled displaced operations leave memory and unrelated registers intact.
    Hook h;std::array<U32,16> r{};r[0]=play+0x8300;r[5]=Common;
    m.Write(play+0x8364,0x0013abcd);m.Write(Common+0x10,0xffffffff);
    auto before=m.words;h.Handle(m,r,1);assert(r[0]==0x12);
    r[0]=play+0x8300;h.Handle(m,r,2);assert(r[1]==0x13);
    h.Handle(m,r,3);assert(r[0]==0xffffffff&&m.words==before);
    }
    if(argc>1) {
        Memory code;std::ifstream f(argv[1],std::ios::binary);assert(f);
        for(auto s:Signatures)for(U32 n=0;n<s[1];n+=4) {U32 w;f.seekg(s[0]+n-0x100000);f.read(reinterpret_cast<char*>(&w),4);assert(f);code.Write(s[0]+n,w);}
        assert(CodeMatches(code));
        for(unsigned i=0;i<Sites.size();++i){auto s=Sites[i];assert(code.Read(s[0])==s[1]);assert(Instruction(s[0],s[1]^1)==(s[1]^1));code.Write(s[0],Instruction(s[0],s[1]));assert(CodeMatches(code));}
        code.words[0x604514]^=1;assert(!CodeMatches(code));
    }
    std::cout<<"Classic Double Time: boundaries, guarded text restoration, Yes/No, toggle latching, native jump setup, disabled replay and executable signatures passed.\n";
}
