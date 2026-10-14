// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/tatl_reminders.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <optional>
#include <unordered_map>
using namespace MM3D::TatlReminders;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at){auto i=words.find(at);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
    void Write(U32 at,U32 v){words[at]=v;}
};
int main(int argc,char** argv) {
    Memory m;constexpr U32 ctx=0x9000000;
    for(auto [at,w]:Guards)m.words[at]=w;
    assert(CodeMatches(m));
    for(auto [at,w]:Guards){m.words[at]^=1;assert(!CodeMatches(m));m.words[at]=w;}
    assert(Instruction(PC,Word)==(0xef000000|SVC));
    assert(Instruction(PC+4,Word)==Word&&Instruction(PC,Word^1)==(Word^1));
    for(bool setting:{false,true})for(U32 current:{0u,0x2au,0x2bu,0x34u})for(U32 request:{0u,0x2au,0x2bu,0x2cu,0x34u,0x35u}) {
        disabled=setting;m.words[ctx+0x8658]=0xbeef0000|current;
        const auto before=m.words;const bool suppress=setting&&Regular(request);
        assert(Request(m,ctx,request)==(suppress?0:request));
        auto expected=before;
        if(suppress&&Regular(current))expected[ctx+0x8658]=0xbeef0000;
        assert(m.words==expected); // Alarm and adjacent memory always preserved.
    }
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);assert(f);
        for(auto [at,w]:Guards){U32 v=0;f.seekg(at-0x100000);f.read(reinterpret_cast<char*>(&v),4);assert(f&&v==w);}
    }
    std::cout<<"Tatl regular-request suppression, alarm preservation, disabled replay and native guards passed.\n";
}
