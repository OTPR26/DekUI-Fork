// SPDX-License-Identifier: GPL-2.0-or-later
// clang++ -std=c++20 -Isrc tools/mm3d/actor_draw_distance_check.cpp -o /tmp/actor-distance-check
#include "mm3d/actor_draw_distance.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <vector>
using namespace MM3D::ActorDrawDistance;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at) const {
        auto it=words.find(at);
        return it==words.end()?std::nullopt:std::optional<U32>(it->second);
    }
};
int main(int argc,char** argv) {
    assert(argc==3); // actual Restoration executable and stock USA Rev1
    for(int file=1;file<argc;++file) {
        std::ifstream in(argv[file],std::ios::binary);
        assert(in);
        std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in),{}};
        const U32 pc=file==1?PC:OriginalPC, offset=PC-pc;
        Memory code;
        for(auto [start,count]:{std::pair{0x1755f8u,Caller.size()},std::pair{0x190ea4u,Cull.size()}})
            for(unsigned i=0;i<count;++i) {
                U32 at=start-offset+4*i,index=at-0x100000;
                assert(index+4<=bytes.size());
                code.words[at]=U32(bytes[index])|(U32(bytes[index+1])<<8)|(U32(bytes[index+2])<<16)|(U32(bytes[index+3])<<24);
            }
        assert(CodeMatches(code,pc));
        assert(!CodeMatches(code,pc+4));
        assert(Instruction(pc,Word)==(0xef000000|SVC));
        assert(Instruction(pc,Word^1)==(Word^1));
        assert(Instruction(pc+4,Word)==Word);
        for(auto& [address,word]:code.words) {
            word^=1;assert(!CodeMatches(code,pc));word^=1;
        }
    }
    const U32 native=std::bit_cast<U32>(1000.f);
    for(unsigned category=0;category<256;++category) {
        Memory actor;actor.words[0x8000000]=0x12000000|(category<<16)|0x42;
        const auto before=actor.words;
        for(int frame=0;frame<100;++frame) {
            assert(Apply(actor,0x8000000,native,1.f)==native);
            assert(Apply(actor,0x8000000,native,2.f)==std::bit_cast<U32>((category==4||category==5)?2000.f:1000.f));
        }
        assert(Apply(actor,0x8000000,native,3.f)==std::bit_cast<U32>((category==4||category==5)?3000.f:1000.f));
        assert(actor.words==before); // no actor edits or accumulated range
    }
    for(U32 bits:{0u,0x80000000u,0xbf800000u,0x7fc01234u,0x7f800000u,0xff800000u,0x7f7fffffu}) {
        assert(Distance(bits,4,1.f)==bits);
        assert(Distance(bits,4,2.f)==bits); // malformed/overflow values unchanged
    }
    assert(Distance(native,5,1.5f)==std::bit_cast<U32>(1500.f));
    assert(Distance(native,5,100.f)==std::bit_cast<U32>(3000.f));
    assert(Distance(native,5,-1.f)==native);
    assert(Distance(native,5,std::numeric_limits<float>::quiet_NaN())==native);
    Memory missing;
    assert(Apply(missing,0x8000000,native,2.f)==native);
    assert(Apply(missing,0,native,2.f)==native);
    // Native far test is projected Z >= distance + extent. Side/vertical
    // extents stay native, so only the forward cutoff increases.
    constexpr float extent=100.f,z=1600.f;
    assert(z>=std::bit_cast<float>(Distance(native,4,1.f))+extent);
    assert(z<std::bit_cast<float>(Distance(native,4,2.f))+extent);
    std::cout<<"Actor distance checks passed (stock + Restoration).\n";
}
