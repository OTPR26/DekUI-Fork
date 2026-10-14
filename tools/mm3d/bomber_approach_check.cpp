// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/bomber_approach.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <optional>
#include <unordered_map>
using namespace MM3D::BomberApproach;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 a){auto i=words.find(a);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
};
int main(int argc,char** argv) {
    Memory m;for(auto [a,w]:Guards)m.words[a]=w;assert(CodeMatches(m));
    for(auto [a,w]:Guards){m.words[a]^=1;assert(!CodeMatches(m));m.words[a]=w;}
    assert(Instruction(StatePC,StateWord)==(0xef000000|FirstSVC));
    assert(Instruction(ApproachPC,ApproachWord)==(0xef000000|(FirstSVC+1)));
    assert(Instruction(ApproachPC,ApproachWord^1)==(ApproachWord^1));
    constexpr U32 actor=0x9000000;m.words[actor]=0x4020f;m.words[actor+0xb68]=0x56dd04;
    disabled=false;assert(!Suppress(m,actor));disabled=true;assert(Suppress(m,actor));
    for(U32 state:{0u,1u,2u,3u,4u,5u,0xffffffffu}) {
        assert(State(state,false)==state);
        assert(State(state,true)==((state==2||state==3)?4:state));
    }
    // The alternate hide-and-seek/dialogue action must never qualify.
    m.words[actor+0xb68]=0x123456;assert(!Suppress(m,actor));
    m.words[actor+0xb68]=0x56dd04;m.words[actor]=0x40210;assert(!Suppress(m,actor));
    for(U32 bad:{0u,actor+1,0xfffffffcu})assert(!Suppress(m,bad));
    if(argc>1){std::ifstream f(argv[1],std::ios::binary);assert(f);for(auto [a,w]:Guards){U32 v=0;f.seekg(a-0x100000);f.read(reinterpret_cast<char*>(&v),4);assert(f&&v==w);}}
    std::cout<<"Bomber approach state filtering, other-action isolation, disabled replay and native signatures passed.\n";
}
