// SPDX-License-Identifier: GPL-2.0-or-later
// clang++ -std=c++20 -Isrc tools/mm3d/death_penalties_check.cpp -o /tmp/death-check
#include "mm3d/death_penalties.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
using namespace MM3D::DeathPenalties;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at) const {
        auto it=words.find(at);return it==words.end()?std::nullopt:std::optional<U32>(it->second);
    }
    void Write(U32 at,U32 value) { words[at]=value; }
};
constexpr U32 Ctx=0x10000000, BankSentinel=0x775ad0;
Memory Live(U32 scene,U32 state,U32 rupees) {
    Memory m;
    m.words[Ctx+0x13c]=0xbeef0402;
    m.words[Ctx+0x148]=0xcafe0000|scene;
    m.words[Ctx+GameOverOffset]=0xfeed0000|state;
    m.words[Wallet]=0x12340000|rupees;
    m.words[Accumulator]=0x56780063;
    m.words[Health]=0x9abc0000;
    m.words[BankSentinel]=0x00001387; // unrelated save bytes must never change
    return m;
}
std::array<U32,16> Registers() {
    std::array<U32,16> r;
    for(unsigned i=0;i<r.size();++i)r[i]=0x20000000+i*4;
    return r;
}
int main(int argc,char** argv) {
    assert(Instruction(DeathPC,DeathWord)==(0xef000000|FirstSVC));
    assert(Instruction(ContinuePC,ContinueWord)==(0xef000000|(FirstSVC+1)));
    assert(Instruction(DeathPC,DeathWord^1)==(DeathWord^1));
    assert(Instruction(ContinuePC+4,ContinueWord)==ContinueWord);
    // Wallet edges, all contexts, both settings. Preserve every neighboring byte,
    // bank sentinel and register, replaying the original game-over store exactly.
    for(U32 scene=0;scene<=0x74;++scene)for(bool enabled:{false,true})
      for(U32 rupees:{0u,1u,99u,200u,500u,999u}) {
        auto m=Live(scene,0,rupees), expected=m;
        auto r=Registers();r[0]=Ctx+0x8600;r[1]=1;r[6]=Ctx;
        const auto original=r;
        Death(m,r,enabled);
        expected.words[Ctx+GameOverOffset]=0xfeed0001;
        if(enabled){expected.words[Wallet]=0x12340000;expected.words[Accumulator]=0x56780000;}
        assert(m.words==expected.words && r==original);
      }
    // A changed/malformed context must not affect the wallet; native replay remains.
    for(int invalid=0;invalid<5;++invalid) {
        auto m=Live(0x6f,0,200);auto r=Registers();r[0]=Ctx+0x8600;r[1]=1;r[6]=Ctx;
        if(invalid==0)m.words[Ctx+0x13c]=0x0602;
        if(invalid==1)m.words.erase(Ctx+0x13c);
        if(invalid==2)m.words[Health]|=1;
        if(invalid==3)m.words.erase(Accumulator);
        if(invalid==4)r[6]+=4;
        const auto before=m.words[Wallet];Death(m,r,true);assert(m.words[Wallet]==before);
    }
    // Every scene: all six Moon destinations keep native respawn. Rupee penalties
    // remain independent, including on the Moon. Disabled mode is bit-exact BL.
    for(U32 scene=0;scene<=0x100;++scene)for(bool end:{false,true}) {
        auto m=Live(scene,3,200),expected=m;
        auto r=Registers();r[0]=Ctx;r[9]=Ctx;r[6]=Ctx+GameOverOffset;
        const auto before=r;
        const bool moon=scene==11||scene==42||scene==63||scene==71||scene==102||scene==103;
        Continue(m,r,end);
        const bool crash=end && scene<=0x74 && !moon;
        if(crash){expected.words[Health]=0x9abc0030;expected.words[Ctx+GameOverOffset]=0xfeed0004;}
        assert(m.words==expected.words);
        for(unsigned i=0;i<14;++i)assert(r[i]==before[i]);
        assert(r[14]==(crash?ContinueReturn:ContinuePC+4));
        assert(r[15]==(crash?NativeMoonCrash:NativeRespawn));
    }
    for(int invalid=0;invalid<6;++invalid) {
        auto m=Live(0x6f,3,200);auto r=Registers();r[0]=Ctx;r[9]=Ctx;r[6]=Ctx+GameOverOffset;
        if(invalid==0)m.words[Ctx+0x13c]=0x0400;
        if(invalid==1)m.words.erase(Ctx+0x148);
        if(invalid==2)m.words[Health]|=1;
        if(invalid==3)m.words[Ctx+GameOverOffset]=20; // fairy revival
        if(invalid==4)r[9]+=4;
        if(invalid==5)r[6]+=4;
        const auto before=m.words;Continue(m,r,true);
        assert(m.words==before && r[15]==NativeRespawn && r[14]==ContinuePC+4);
    }
    assert(argc==2);std::ifstream f(argv[1],std::ios::binary);assert(f);
    Memory code;
    for(auto sig:Signatures) {
        f.seekg(sig.start-0x100000);
        for(U32 off=0;off<sig.size;off+=4){U32 word;f.read(reinterpret_cast<char*>(&word),4);assert(f);code.words[sig.start+off]=word;}
    }
    assert(CodeMatches(code));
    for(auto sig:Signatures)for(U32 off=0;off<sig.size;off+=4){code.words[sig.start+off]^=1;assert(!CodeMatches(code));code.words[sig.start+off]^=1;}
    // Verify no-fairy/fairy split, native crash time/entrance, and return epilogue.
    assert(code.Read(0x2062c0)==0x0a00001eu); // BEQ to death-only store
    assert(code.Read(0x2062c4)==0xe3a02014u); // fairy state20, bypasses death hook
    assert(code.Read(ContinueReturn)==0xe8bd9ff0u);
    assert(code.Read(0x1bff80)==0x400au && code.Read(0x1bff88)==0x54c0u);
    std::cout<<"Death penalty checks passed against native binary\n";
}
