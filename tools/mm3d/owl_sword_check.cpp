// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/owl_sword.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <optional>
#include <unordered_map>
using namespace MM3D::OwlSword;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at){auto i=words.find(at);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
    void Write(U32 at,U32 w){words[at]=w;}
};
int main(int argc,char** argv) {
    Memory m;constexpr U32 owl=0x9000000,hit=0x9100000;
    for(auto [a,w]:Guards)m.words[a]=w;
    assert(CodeMatches(m));
    for(auto [a,w]:Guards){m.words[a]^=1;assert(!CodeMatches(m));m.words[a]=w;}
    m.words[owl]=0x701b2;m.words[owl+4]=0x109;m.words[owl+0x1c]=0;
    m.words[0x775324]=0;m.words[0x626c3c]=1;m.words[owl+0x268]=Idle;
    m.words[owl+0x264]=0;m.words[owl+0x21c]=0x20d;m.words[owl+0x248]=hit;
    const auto initial=m.words;
    for(U32 weapon:{2u,8u,32u,128u,256u,512u,0x1000000u}) {
        m.words=initial;m.words[hit]=weapon;enabled=false;
        auto before=m.words;Update(m,owl);assert(m.words==before&&!Locked(m,owl));
        enabled=true;assert(Locked(m,owl));Update(m,owl);
        assert(m.words[owl+0x268]==((weapon&0x1000200)?Activate:Idle));
        assert(m.words[owl+4]==9);assert(m.words[0x775324]==0);
    }
    for(int mode=0;mode<4;++mode) {
        m.words=initial;m.words[hit]=512;enabled=true;
        if(mode==0)m.words[0x775324]=1; // Already activated: ordinary A interaction.
        if(mode==1)m.words[owl+0x1c]=0x200; // Feather statue.
        if(mode==2)m.words[owl]=0x123; // Other actor.
        if(mode==3)m.words[owl+0x1c]=15; // Invalid index.
        const auto before=m.words;assert(!Locked(m,owl));Update(m,owl);assert(m.words==before);
    }
    m.words=initial;m.words[hit]=512;m.words[owl+0x21c]=0x0d;Update(m,owl);
    assert(m.words[owl+0x268]==Idle); // Swinging nearby without a collision is insufficient.
    for(bool classic:{false,true})for(bool sword:{false,true})for(bool feather:{false,true}) {
        m.words=initial;enabled=sword;m.words[owl+0x1c]=feather?0x200:0;
        const auto before=m.words;
        assert(FeatherBlocked(m,owl,classic)==(classic&&feather));
        BlockFeatherAcceptance(m,owl,classic);
        auto expected=before;if(classic&&feather)expected[owl+4]&=~0x100u;
        assert(m.words==expected); // Preserve saves, action state and ordinary owls.
    }
    m.words=initial;m.words[owl+0x1c]=0x200;m.words[owl]=0x123;
    assert(!FeatherBlocked(m,owl,true));
    m.words[owl]=0x1b2;m.words.erase(owl+0x1c);
    assert(!FeatherBlocked(m,owl,true));
    assert(!FeatherBlocked(m,owl+1,true));
    if(argc>1){std::ifstream f(argv[1],std::ios::binary);assert(f);for(auto [a,w]:Guards){U32 v=0;f.seekg(a-0x100000);f.read(reinterpret_cast<char*>(&v),4);assert(f&&v==w);}}
    std::cout<<"Owl sword hit gating, native activation, active/feather exemptions, disabled replay and signatures passed.\n";
}
