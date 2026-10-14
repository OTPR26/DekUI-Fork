// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/bottle_healing.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>
using namespace MM3D::BottleHealing;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at)const {
        auto it=words.find(at);return it==words.end()?std::nullopt:std::optional<U32>(it->second);
    }
};
using Regs=std::array<U32,16>;
constexpr U32 Ctx=0x10000000, Player=0x11000000, Fairy=0x12000000;
int main(int argc,char**argv) {
    assert(argc==2);
    std::ifstream f(argv[1],std::ios::binary);assert(f);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(f),{}};
    Memory code;
    for(unsigned at=0;at+4<=bytes.size();at+=4)
        code.words[0x100000+at]=U32(bytes[at])|(U32(bytes[at+1])<<8)|(U32(bytes[at+2])<<16)|(U32(bytes[at+3])<<24);
    assert(CodeMatches(code));
    for(const auto& s:Signatures)for(U32 a=s.start;a<s.start+s.size;a+=4) {
        code.words[a]^=1;assert(!CodeMatches(code));code.words[a]^=1;
    }
    for(U32 a:{0x1e71bc,0x2acae8,0x21f4fc}) {
        code.words[a]^=1;assert(!CodeMatches(code));code.words[a]^=1;
    }
    for(unsigned i=0;i<3;++i) {
        assert(code.Read(PCs[i])==Words[i]);
        assert(Instruction(PCs[i],Words[i])==(0xef000000|FirstSVC+i));
        assert(Instruction(PCs[i],Words[i]^1)==(Words[i]^1));
    }
    // Installed native potion flags: red heals, blue heals and restores magic.
    assert(bytes[0x68d7f7+0x23-0x100000]==1);
    assert(bytes[0x68d7f7+0x24-0x100000]==3);
    Memory m; m.words[Ctx+0x13c]=0xabcd0402;m.words[Player]=0xaabb0000;m.words[Fairy]=0xccdd0010;
    Regs potion{};potion[4]=Player;potion[6]=HealingBase;potion[7]=Ctx;potion[8]=Player+0x100;
    Regs fairy{};fairy[1]=HealingBase;fairy[5]=Fairy;fairy[7]=Ctx;
    Regs revive{};revive[4]=Player;revive[5]=Ctx;revive[7]=Player+0x11e00;revive[11]=HealingBase;
    auto run=[&](Regs r,unsigned hook,int red,int blue,int heal,U32 expected) {
        auto before=r;const auto words=m.words;
        Apply(m,r,hook,red,blue,heal);assert(r[0]==expected);
        for(unsigned i=1;i<16;++i)assert(r[i]==before[i]);
        assert(m.words==words); // Never mutates bottle, magic, bank or save memory.
    };
    for(int n=1;n<=20;++n) {
        m.words[Player+0x1fc]=0x00230000;run(potion,0,n,20,10,n*16);
        m.words[Player+0x1fc]=0x00240000;run(potion,0,20,n,10,n*16);
        for(U32 role:{1,5}) {m.words[Fairy+0x1c]=0xbeef0000|role;run(fairy,1,20,20,n,n*16);}
        run(revive,2,20,20,n,n*16);
    }
    // Every other potion/action and fairy role stays native.
    for(U32 action=0;action<256;++action)if(action!=0x23&&action!=0x24) {
        m.words[Player+0x1fc]=action<<16;run(potion,0,2,3,4,0x140);
    }
    for(U32 role=0;role<16;++role)if(role!=1&&role!=5) {
        m.words[Fairy+0x1c]=role;run(fairy,1,2,3,4,0xa0);
    }
    m.words[Player+0x1fc]=0x00230000;m.words[Fairy+0x1c]=1;
    run(potion,0,20,20,10,0x140);run(fairy,1,20,20,10,0xa0);run(revive,2,20,20,10,0xa0);
    run(potion,0,-9,20,10,16);run(fairy,1,20,20,99,320);
    m.words[Ctx+0x13c]=0;run(potion,0,2,3,4,0x140);run(fairy,1,2,3,4,0xa0);run(revive,2,2,3,4,0xa0);
    m.words[Ctx+0x13c]=0x0402;
    m.words[Player]=1;run(potion,0,2,3,4,0x140);run(revive,2,2,3,4,0xa0);
    m.words[Fairy]=0;run(fairy,1,2,3,4,0xa0);
    m.words[Player]=0;m.words[Fairy]=0x10;
    auto bad=potion;bad[6]=0;run(bad,0,2,3,4,0x140);
    bad=fairy;bad[1]=0;run(bad,1,2,3,4,0xa0);
    bad=revive;bad[11]=0;run(bad,2,2,3,4,0xa0);
    m.words.erase(Player+0x1fc);run(potion,0,2,3,4,0x140);
    m.words.erase(Fairy+0x1c);run(fairy,1,2,3,4,0xa0);
    std::cout<<"Bottle healing: native code guards, 1-20 hearts, both fairy paths, unrelated healing and register/memory isolation passed\n";
}
