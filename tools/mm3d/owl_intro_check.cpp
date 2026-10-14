// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/owl_sword.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <optional>
#include <unordered_map>
using namespace MM3D;
using U32 = std::uint32_t;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at) {
        auto it=words.find(at);
        return it==words.end()?std::nullopt:std::optional<U32>(it->second);
    }
    void Write(U32 at,U32 value) { words[at]=value; }
};
int main(int argc,char** argv) {
    Memory m;
    for(auto [at,w]:OwlIntro::Guards)m.words[at]=w;
    assert(OwlIntro::CodeMatches(m));
    for(auto [at,w]:OwlIntro::Guards) {
        m.words[at]^=1;assert(!OwlIntro::CodeMatches(m));m.words[at]=w;
    }
    assert(OwlIntro::Instruction(OwlIntro::IntroPC,OwlIntro::IntroWord)==(0xef000000|OwlIntro::FirstSVC));
    assert(OwlIntro::Instruction(OwlIntro::TatlPC,OwlIntro::TatlWord)==(0xef000000|(OwlIntro::FirstSVC+1)));
    assert(OwlIntro::Instruction(OwlIntro::IntroPC,OwlIntro::IntroWord^1)==(OwlIntro::IntroWord^1));
    constexpr U32 owl=0x9000000, hit=0x9100000;
    for(bool skip:{false,true}) {
        OwlIntro::disabled=skip;
        std::array<U32,16> r{};
        for(unsigned i=0;i<r.size();++i)r[i]=0x9000000+i*4;
        const auto before=r;
        OwlIntro::Enter(r);
        auto expected=before;expected[4]=before[0];
        if(skip){expected[14]=OwlIntro::IntroReturn;expected[15]=OwlIntro::Idle;}
        assert(r==expected); // SP and saved caller frame remain intact for the native pop.
        for(U32 flags=0;flags<256;++flags)
            assert(OwlIntro::TatlFlags(flags)==(skip?flags|0x10:flags));
        // Sword setting remains independent. The intro state is eligible only
        // while skipped; an activation cutscene already running is never replaced.
        for(bool sword:{false,true})for(U32 action:{OwlIntro::Intro,OwlSword::Idle,0x30e950u}) {
            m.words[owl]=0x1b2;m.words[owl+4]=0x19;m.words[owl+0x1c]=0;
            m.words[0x775324]=0;m.words[0x626c3c]=1;
            m.words[owl+0x264]=0;m.words[owl+0x268]=action;
            m.words[owl+0x21c]=0x200;m.words[owl+0x248]=hit;m.words[hit]=512;
            const auto memory_before=m.words;
            OwlSword::enabled=sword;OwlSword::Update(m,owl);
            bool activate=sword&&(action==OwlSword::Idle||(skip&&action==OwlIntro::Intro));
            assert(m.words[owl+0x268]==(activate?OwlSword::Activate:action));
            assert(m.words[0x775324]==0); // No automatic activation/save-bit write.
            if(!sword)assert(m.words==memory_before);
        }
    }
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);assert(f);
        for(auto [at,w]:OwlIntro::Guards) {
            U32 value=0;f.seekg(at-0x100000);f.read(reinterpret_cast<char*>(&value),4);
            assert(f&&value==w);
        }
    }
    std::cout<<"Owl intro: disabled replay, native return, specific Tatl gate, sword combinations and binary guards passed.\n";
}
