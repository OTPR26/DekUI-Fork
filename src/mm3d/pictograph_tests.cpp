// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
#include "mm3d/pictograph.h"
namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Memory {
    std::map<std::uint32_t, std::uint32_t> words;
    std::optional<std::uint32_t> Read(std::uint32_t at) {
        auto i=words.find(at);
        return i==words.end()?std::nullopt:std::optional(i->second);
    }
    void Write(std::uint32_t at,std::uint32_t value) { words[at]=value; }
};
}
int main() {
    using namespace MM3D::Pictograph;
    try {
        Memory m;
        std::array<U32,16> r{};
        r[4]=0x72a000; r[5]=0x202; r[6]=0x800;
        Cancel(); Update(m,r,1000);
        Check(r[5]==0x202 && m.words[0x72a008]==0x800, "unbound input or original store changed");
        requested=900; Update(m,r,1000);
        Check(r[5]==0x606 && r[6]==0x800 && m.words.size()==1, "Pictograph changed item buttons");
        Check(r[15]==PC+4, "incorrect return address");
        r[5]=0; Update(m,r,1001);
        Check(r[5]==0, "one press repeated across frames");
        requested=700; Update(m,r,1000);
        Check(r[5]==0 && requested==0, "stale request survived pause");
        requested=1100; Update(m,r,1000);
        Check(r[5]==0, "future request accepted");
        r[4]=0x1000; requested=900; Update(m,r,1000);
        Check(r[5]==0 && requested==0 && m.words[0x1008]==r[6], "unexpected pad state modified");
        Check(Instruction(PC,Word)==(0xef000000u|SVC), "hook missing");
        Check(Instruction(PC,0)==0 && Instruction(PC+4,Word)==Word, "unrelated instruction intercepted");
        Check(!CodeMatches(m), "unsupported code accepted");
        std::cout<<"Pictograph input checks passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
