// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/movement_speed.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <vector>
using namespace MM3D::MovementSpeed;
struct Memory {
    std::unordered_map<U32,U32> words;unsigned writes{};
    std::optional<U32> Read(U32 at) {auto it=words.find(at);return it==words.end()?std::nullopt:std::optional<U32>(it->second);}
    void Write(U32 at,U32 value) {++writes;words[at]=value;}
};
constexpr U32 Actor=0x08010000,Root=0x08008000,ContextAddress=0x08040000;
U32 Bits(float f){return std::bit_cast<U32>(f);}
Memory Fixture(U32 flags=0,U32 flags2=0x20,U32 flags3=0,U32 ground=1) {
    Memory m;m.words={{0x72f5c0,Root},{Root+0x16c,ContextAddress},{ContextAddress+0x13c,0x0402},
        {ContextAddress+0x20dc,Actor},{Actor,0x00020000},{Actor+0x11db0,flags},
        {Actor+0x11db4,flags2},{Actor+0x11db8,flags3},{Actor+0x94,ground},
        {Actor+0xe14,0x1e5230},{Actor+0x64,Bits(2.f)},{Actor+0x68,Bits(-3.f)},
        {Actor+0x6c,Bits(4.f)},{Actor+0xa8,Bits(1.f)}};return m;
}
int main(int argc,char** argv) {
    assert(argc>=2);
    for(int n=1;n<argc;++n) {
        std::ifstream input(argv[n],std::ios::binary);assert(input);
        std::vector<unsigned char> data{std::istreambuf_iterator<char>(input),{}};
        Memory m;
        for(const auto& s:Signatures)for(U32 at=s.start;at<s.start+s.size;at+=4) {
            assert(at-0x100000+4<=data.size());U32 value=0;
            for(unsigned byte=0;byte<4;++byte)value|=U32(data[at-0x100000+byte])<<(8*byte);
            m.words[at]=value;
        }
        assert(CodeMatches(m));
        for(unsigned i=0;i<PCs.size();++i) {
            assert(m.Read(PCs[i])==Words[i]);
            assert(Instruction(PCs[i],Words[i])==(0xef000000|FirstSVC+i));
            assert(Instruction(PCs[i],Words[i]^1)==(Words[i]^1));
            m.words[PCs[i]]^=1;assert(!CodeMatches(m));m.words[PCs[i]]^=1;
        }
        m.words.erase(Signatures[0].start);assert(!CodeMatches(m));
    }
    std::array<U32,16> regs{};regs[0]=Actor;
    std::array<U32,64> floats{};
    // 1x exactly emulates all three displaced VLDRs without changing memory.
    auto native=Fixture();auto before=native.words;
    for(unsigned axis=0;axis<3;++axis) {
        Apply(native,regs,floats,axis,1.f,1.f,1.f);
        assert(floats[2]==*native.Read(Actor+0x64+4*axis));
    }
    assert(native.words==before&&native.writes==0);
    // 2x grounded travel scales horizontal velocity operands, never gravity.
    // Repeated frames cannot compound the persistent speed or platform carry.
    for(unsigned frame=0;frame<100;++frame)for(unsigned axis=0;axis<3;++axis) {
        Apply(native,regs,floats,axis,2.f,3.f,.5f);
        const float original=std::bit_cast<float>(*native.Read(Actor+0x64+4*axis));
        assert(floats[2]==Bits(original*(axis==1?1.f:2.f)));
    }
    assert(native.words==before&&native.writes==0);
    // Swimming is independent of walk and climbing, and supports signed diving.
    auto water=Fixture(0x08000000,0x400,0x8000,0);
    for(unsigned axis=0;axis<3;++axis) {
        Apply(water,regs,floats,axis,3.f,.5f,1.5f);
        assert(floats[2]==Bits(std::bit_cast<float>(*water.Read(Actor+0x64+4*axis))*1.5f));
    }
    auto surface=Fixture(0x08000000,0x20,0,0);
    Apply(surface,regs,floats,1,3.f,3.f,2.f);assert(floats[2]==Bits(-3.f));
    for(U32 flag:{0x80u,0x40u,0x800000u,0x10000000u,0x20000000u,0x40000u,0x80000u,0x2000u,0x4000u,0x200000u}) {
        auto m=Fixture(flag);Apply(m,regs,floats,0,3.f,3.f,3.f);assert(floats[2]==Bits(2.f));
    }
    for(U32 flag:{8u,0x100u,0x200u,0x1000u,0x2000u,0x80000u,0x100000u}) {
        auto m=Fixture(0,0x20,flag);Apply(m,regs,floats,0,3.f,3.f,3.f);assert(floats[2]==Bits(2.f));
    }
    for(unsigned scenario=0;scenario<6;++scenario) {
        auto m=Fixture();
        switch(scenario) {
        case 0:m.words[Actor]=0x0003000f;break; // another actor
        case 1:m.words[ContextAddress+0x13c]=0;break; // menu/game state
        case 2:m.words[ContextAddress+0x20dc]=Actor+4;break;
        case 3:m.words.erase(Actor+0x11db4);break;
        case 4:m.words[Actor+0x94]=0;break; // airborne
        case 5:m.words[Actor+0x11db4]=0;break; // no locomotion
        }
        Apply(m,regs,floats,0,3.f,3.f,3.f);assert(floats[2]==Bits(2.f));assert(m.writes==0);
    }
    // Climbing scales the newly computed signed animation rate, preserving
    // backwards climbing; no repeated multiplication of the stored rate.
    regs[0]=Actor+0x334;regs[4]=Actor;regs[6]=ContextAddress;
    auto ladder=Fixture();before=ladder.words;
    for(unsigned frame=0;frame<100;++frame) {
        floats[0]=Bits(-2.f);Apply(ladder,regs,floats,3,3.f,1.5f,.5f);
        assert(ladder.Read(Actor+0x380)==Bits(-3.f));
    }
    before[Actor+0x380]=Bits(-3.f);assert(ladder.words==before);
    ladder.words[Actor+0xe14]=0;floats[0]=Bits(-2.f);
    Apply(ladder,regs,floats,3,3.f,3.f,3.f);assert(ladder.Read(Actor+0x380)==Bits(-2.f));
    for(U32 bits:{Bits(-0.f),0x7fc12345u,0x7f800000u})assert(Scale(bits,1.f)==bits&&Scale(bits,2.f)==bits);
    assert(Scale(Bits(2.f),std::numeric_limits<float>::quiet_NaN())==Bits(2.f));
    assert(Scale(Bits(2.f),100.f)==Bits(6.f));assert(Scale(Bits(2.f),0.f)==Bits(1.f));
    std::cout<<"Movement speed: exact code guards, native 1x, independent modes, no compounding, gravity/carry exclusions, signed climbing, invalid actors/states and multiplier bounds passed\n";
}
