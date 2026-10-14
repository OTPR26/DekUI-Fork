// SPDX-License-Identifier: GPL-2.0-or-later
// clang++ -std=c++20 -Isrc tools/mm3d/prop_draw_distance_check.cpp -o /tmp/prop-distance-check
#include "mm3d/prop_draw_distance.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>
using namespace MM3D::PropDrawDistance;
struct Memory {
    std::vector<unsigned char> bytes;
    std::optional<U32> Read(U32 at) const {
        if(at<0x100000||at-0x100000+std::uint64_t(4)>bytes.size())return {};
        at-=0x100000;
        return U32(bytes[at])|(U32(bytes[at+1])<<8)|(U32(bytes[at+2])<<16)|(U32(bytes[at+3])<<24);
    }
};
int main(int argc,char** argv) {
    assert(argc==3);
    for(int file=1;file<argc;++file) {
        std::ifstream in(argv[file],std::ios::binary);assert(in);
        Memory m{{std::istreambuf_iterator<char>(in),{}}};
        for(unsigned i=0;i<Hooks.size();++i) {
            const auto& h=Hooks[i];const U32 pc=file==1?h.pc:h.stock_pc;
            assert(m.Read(pc)==h.word);
            assert(CodeMatches(m,pc));
            assert(Instruction(pc,h.word)==(0xef000000|FirstSVC+i));
            assert(Instruction(pc,h.word^1)==(h.word^1));
            m.bytes[pc-0x100000]^=1;assert(!CodeMatches(m,pc));m.bytes[pc-0x100000]^=1;
            // Literal corruption must also disable the corresponding family.
            const U32 delta=file==1?0:h.stock_pc-h.pc;
            const U32 constant=(i<3?0x4cb294:0x50970c)+delta;
            m.bytes[constant-0x100000]^=1;assert(!CodeMatches(m,pc));m.bytes[constant-0x100000]^=1;
            for(float factor:{1.f,1.5f,2.f,3.f}) {
                std::array<U32,16> regs;regs.fill(0xdeadbeef);
                std::array<U32,64> floats;floats.fill(0x12345678);
                const auto old_regs=regs;const auto old_floats=floats;
                Load(i,regs,floats,factor);
                const U32 expected=std::bit_cast<U32>(h.value+1200.f*(factor-1.f));
                for(unsigned r=0;r<regs.size();++r)assert(regs[r]==(!h.floating&&r==h.reg?expected:old_regs[r]));
                for(unsigned r=0;r<floats.size();++r)assert(floats[r]==(h.floating&&r==h.reg?expected:old_floats[r]));
            }
        }
        assert(!CodeMatches(m,0));
    }
    // Only the named props, never unrelated props or NPC/enemy ranges.
    const U32 original=std::bit_cast<U32>(1200.f);
    for(unsigned category=0;category<12;++category)for(unsigned id=0;id<0x300;++id) {
        const U32 header=id|(category<<16)|0xff000000;
        const bool expected=(category==6&&(id==0x65||id==0x10a||id==0x78||id==0x7c))||(category==1&&id==0x11b);
        assert(Eligible(header)==expected);
        struct Actor {U32 header;std::optional<U32> Read(U32 at){return at==0x8000000?std::optional<U32>(header):std::nullopt;}} actor{header};
        assert(Apply(actor,0x8000000,original,1.f)==original);
        for(int frame=0;frame<4;++frame)
            assert(Apply(actor,0x8000000,original,2.f)==std::bit_cast<U32>(expected?2400.f:1200.f));
        assert(Apply(actor,0x8000000,original,3.f)==std::bit_cast<U32>(expected?3600.f:1200.f));
        assert(actor.header==header);
        assert(Apply(actor,0,original,2.f)==original);
    }
    // Opaque-to-transparent boundary shifts outward without changing alpha
    // or length of the native fade band, including its final zero endpoint.
    for(float factor:{1.f,1.25f,1.5f,2.f,3.f}) {
        const float start=std::bit_cast<float>(Threshold(1200.f,factor));
        const float end=std::bit_cast<float>(Threshold(1300.f,factor));
        assert(end-start==100.f);
        for(int step=0;step<=100;++step) {
            const float z=start+step;
            assert((end-z)*.01f==(100.f-step)*.01f);
        }
    }
    assert(Threshold(1200.f,std::numeric_limits<float>::quiet_NaN())==original);
    assert(Threshold(1200.f,-5.f)==original);
    assert(Threshold(1200.f,99.f)==std::bit_cast<U32>(3600.f));
    std::cout<<"Bush checks passed (stock + Restoration, scope and fades).\n";
}
