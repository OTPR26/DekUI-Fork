// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/screen_shrink.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
using namespace MM3D::ScreenShrink;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 p)const{auto i=words.find(p);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
    void Write(U32 p,U32 v){words[p]=v;}
    void F(U32 p,float v){Write(p,std::bit_cast<U32>(v));}
};
struct Rect {unsigned left,top,right,bottom;bool operator==(const Rect&)const=default;};
constexpr U32 UI=0x200000,G=0x210000,E=0x220000;
Memory Fixture(){Memory m;
    m.Write(GuardPC,GuardWord);m.Write(0x171b38,Minimum);m.Write(PC,Word);m.Write(PC+4,0xeb004910);m.Write(0x15f63c,0x7785d8);
    m.Write(0x15f640,0x3a83126f);m.Write(0x15f590,0xed910ab1);
    m.Write(0x72f5c0,UI);m.Write(UI+0x16c,G);m.Write(G+0x13c,0x402);
    m.F(0x77889c,800);m.F(E+4,.8f);return m;
}
int main(int argc,char** argv){
    assert(Instruction(PC,Word)==(0xef000000|SVC));assert(Instruction(PC+4,Word)==Word);
    auto m=Fixture();assert(CodeMatches(m));auto before=m.words;
    Store(m,E,std::bit_cast<U32>(.8f),true);assert(native_normalized&&std::abs(presentation_scale-.8f)<1e-6);
    before[E+4]=std::bit_cast<U32>(1.f);assert(m.words==before); // Only native render scale changes.
    U32 r3=0xdeadbeef;
    assert(Guard(E,std::bit_cast<U32>(1.f),0,r3)==GuardPC+4&&r3==Minimum);
    // The native fast path is replayed for other effects and unsupported paths.
    for(U32 flags:{0u,0x80000000u,0x10000000u,0x90000000u}) {
        r3=0xdeadbeef;const bool ge=((flags>>31)&1)==((flags>>28)&1);
        assert(Guard(E+0x100,std::bit_cast<U32>(1.f),flags,r3)==(ge?0x171b1c:GuardPC+4));
        assert(r3==0xdeadbeef);
    }
    assert(Instruction(GuardPC,GuardWord)==(0xef000000|GuardSVC));
    const Rect full{0,0,2000,1200};const auto fitted=Bounds(full,presentation_scale.load());
    assert((fitted==Rect{200,120,1800,1080}));
    // Every pixel in the completed scene/UI gets the same transform, including
    // panes clipped at the original edge. No element-specific centers exist.
    assert((Bounds(Rect{100,50,1100,650},.8f)==Rect{200,110,1000,590}));
    assert(Bounds(full,1.f)==full);assert(Bounds(full,0.f)==full);
    m.F(0x77889c,900);Update(m);assert(std::abs(presentation_scale-.9f)<1e-6);
    m.F(0x77889c,1000);Update(m);assert(presentation_scale==1.f);
    m.Write(G+0x13c,0x302);m.F(0x77889c,800);Update(m);assert(presentation_scale==1.f);
    // Unsupported compositor/code replays the exact native store and never
    // asks final composition to apply a second shrink.
    for(bool unsupported_code:{false,true}){m=Fixture();if(unsupported_code)m.Write(PC+4,0);
        before=m.words;Store(m,E,std::bit_cast<U32>(.8f),unsupported_code);
        assert(m.words==before&&!native_normalized&&presentation_scale==1.f);}
    for(int arg=2;arg<argc;++arg){Memory live;std::ifstream f(argv[arg],std::ios::binary);assert(f);U32 page;
        while(f.read(reinterpret_cast<char*>(&page),4))for(U32 off=0;off<4096;off+=4){U32 v;f.read(reinterpret_cast<char*>(&v),4);assert(f);live.Write(page+off,v);}
        std::ifstream code(argv[1],std::ios::binary);assert(code);
        for(U32 at:{GuardPC,0x171b38u,PC,PC+4,0x15f63cu,0x15f640u,0x15f590u}){U32 v;code.seekg(at-0x100000);code.read(reinterpret_cast<char*>(&v),4);assert(code);live.Write(at,v);}
        assert(CodeMatches(live));const auto amount=live.Read(0x77889c);assert(amount);
        assert(std::abs(Read(live)-std::bit_cast<float>(*amount)*.001f)<1e-6);
        std::cout<<argv[arg]<<": scale="<<Read(live)<<'\n';
    }
    std::cout<<"Screen composition shrink checks passed\n";
}
