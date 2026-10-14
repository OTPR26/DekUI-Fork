// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/minimap_shortcut.h"
#include "mm3d/menu_shortcuts.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
using namespace MM3D::MinimapShortcut;
namespace H=MM3D::HudAdjustments;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at)const{auto i=words.find(at);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
};
constexpr U32 Game=0x10000000,UI=0x10020000,Main=0x10030000;
Memory Live(U32 held=0x8004,U32 pressed=4) {
    return {{{0x72f5c0,UI},{UI+0x16c,Game},{0x656b04,Main},{0x72f5c4,Main},{0x72f5c8,0},
             {Game+0x13c,0x0402},{Game+0x30,held},{Game+0x34,pressed}}};
}
int main(int argc,char**argv) {
    assert(Instruction(PC,Word)==(0xef000000|SVC));
    assert(Instruction(PC,Word^1)==(Word^1)&&Instruction(PC+4,Word)==Word);
    for(bool hidden:{false,true}) {
        H::hide_minimap=hidden;H::minimap_visibility_request=-1;
        auto m=Live();const auto before=m.words;
        std::array<U32,16> r;for(U32 i=0;i<16;++i)r[i]=0x20000000+i*4;r[4]=Game;
        auto saved=r;
        assert(Handle(m,r));assert(H::hide_minimap==!hidden);
        assert(H::minimap_visibility_request.exchange(-1)==int(!hidden));
        assert(r[0]==Sound&&r[14]==Resume&&r[15]==PlaySound&&m.words==before);
        for(unsigned i=1;i<14;++i)assert(r[i]==saved[i]);
        // Native continuation clears SELECT only; ZR and other inputs survive.
        m.words[Game+0x30]&=~4u;m.words[Game+0x34]&=~4u;
        r=saved;assert(!Handle(m,r));assert(H::hide_minimap==!hidden);
        assert(H::minimap_visibility_request==-1); // no repeat while held
        // Another physical SELECT press restores the preceding visibility.
        m.words[Game+0x30]|=4;m.words[Game+0x34]|=4;
        assert(Handle(m,r));assert(H::hide_minimap==hidden);
        assert(H::minimap_visibility_request.exchange(-1)==int(hidden));
    }
    for(int bad=0;bad<8;++bad) {
        H::hide_minimap=false;H::minimap_visibility_request=-1;
        auto m=Live();std::array<U32,16> r{};r[4]=Game;r[14]=0x123;r[15]=PC+4;
        if(bad==0)m.words[Game+0x30]=4; // SELECT alone still opens inventory
        if(bad==1)m.words[Game+0x34]=0; // held only
        if(bad==2)m.words[Game+0x13c]=0x0602; // file select
        if(bad==3)m.words[0x72f5c4]=Main+4; // inventory/notebook/song screen
        if(bad==4)m.words[0x72f5c8]=Main+4; // transition pending
        if(bad==5)m.words[UI+0x16c]=Game+4;
        if(bad==6)m.words.erase(0x72f5c0);
        if(bad==7)m.words.erase(Game+0x34);
        const auto before=m.words;assert(!Handle(m,r));
        assert(r[0]==5&&r[14]==0x123&&r[15]==PC+4&&m.words==before);
        assert(!H::hide_minimap&&H::minimap_visibility_request==-1);
    }
    assert(argc==2);std::ifstream f(argv[1],std::ios::binary);assert(f);Memory code;
    const auto read=[&](U32 at){U32 v;f.seekg(at-0x100000);f.read(reinterpret_cast<char*>(&v),4);assert(f);code.words[at]=v;};
    for(auto s:Signatures)for(U32 off=0;off<s.size;off+=4)read(s.start+off);
    for(U32 at:{0x59ba10u,0x1ee324u,0x1ee328u,0x1ee3e8u})read(at);
    assert(CodeMatches(code));
    for(auto& [at,value]:code.words){value^=1;assert(!CodeMatches(code));value^=1;}
    // Exercise the actual translation precedence with the existing shortcut
    // suppression enabled. Previously its POP at7d06c4 made our hook unreachable.
    namespace Old=MM3D::MenuShortcuts;
    assert(Old::CodeMatches(code));
    assert(Old::Instruction(RoutePC,RouteWord)==Old::Return);
    const auto translated=[&](U32 pc) {
        const U32 original=*code.Read(pc);
        if(Instruction(pc,original)!=original && CodeMatches(code))return Instruction(pc,original);
        if(Old::CodeMatches(code))return Old::Instruction(pc,original);
        return original;
    };
    const auto branch_target=[](U32 pc,U32 w){return U32(pc+8+(std::int32_t(w<<8)>>6));};
    assert(translated(RoutePC)==SelectBranch);
    assert(branch_target(RoutePC,translated(RoutePC))==0x7d06d4);
    assert(code.Read(0x7d06d4)==0xe1160005u); // TST new-SELECT, held-ZR
    assert(code.Read(0x7d06d8)==0x08bd81f0u); // POPEQ: no SELECT => disabled ZR+START
    assert(translated(PC)==(0xef000000|SVC));
    // SELECT/START without ZR still branch around this hook to inventory/notebook.
    assert(branch_target(0x7d06b8,translated(0x7d06b8))==0x7d070c);
    assert(branch_target(0x7d06c0,translated(0x7d06c0))==0x7d0718);
    // A changed sound routine disables BOTH new sites; original early return wins.
    code.words[PlaySound]^=1;
    assert(translated(RoutePC)==Old::Return && translated(PC)==Word);
    code.words[PlaySound]^=1;
    assert(code.Read(Resume)==0xe1c423d0u); // LDRD r2,r3,[r4,#0x30]
    assert(code.Read(Resume+4)==0xe3c22004u&&code.Read(Resume+8)==0xe3c33004u);
    assert(code.Read(Resume+12)==0xe1c423f0u); // STRD back
    assert(code.Read(Resume+16)==0xe8bd81f0u); // original epilogue
    std::cout<<"Minimap shortcut checks passed against native binary\n";
}
