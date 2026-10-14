// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/vegetation_draw_distance.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>
#include <vector>
using namespace MM3D::VegetationDrawDistance;
struct Memory {
    std::vector<unsigned char> bytes;
    std::unordered_map<U32,U32> runtime;
    std::optional<U32> Read(U32 at) const {
        if(auto it=runtime.find(at);it!=runtime.end())return it->second;
        if(at<0x100000||at-0x100000+std::uint64_t(4)>bytes.size())return {};
        at-=0x100000;
        return U32(bytes[at])|(U32(bytes[at+1])<<8)|(U32(bytes[at+2])<<16)|(U32(bytes[at+3])<<24);
    }
};
int main(int argc,char** argv) {
    assert(argc==3);
    for(int file=1;file<argc;++file) {
        std::ifstream in(argv[file],std::ios::binary);assert(in);
        Memory m{{std::istreambuf_iterator<char>(in),{}},{}};
        for(unsigned i=0;i<Hooks.size();++i) {
            const auto& h=Hooks[i];const U32 pc=file==1?h.pc:h.stock_pc;
            const U32 word=*m.Read(pc);
            assert(CodeMatches(m,pc));
            assert(Instruction(pc,word)==(0xef000000|(FirstSVC+i)));
            assert(Instruction(pc,word^1)==(word^1));
            for(unsigned j=0;j<16;++j) {
                const auto at=pc-8+j*4-0x100000;
                m.bytes[at]^=1;assert(!CodeMatches(m,pc));m.bytes[at]^=1;
            }
            U32 native=std::bit_cast<U32>(2560000.f);
            if(h.distance!=0.f) {
                const U32 displacement=h.floating?(word&255)*4:word&4095;
                const U32 literal=pc+8+((word&(1u<<23))?displacement:0u-displacement);
                native=*m.Read(literal);
                assert(Threshold(h,1.f)==native);
                m.bytes[literal-0x100000]^=1;assert(!CodeMatches(m,pc));m.bytes[literal-0x100000]^=1;
            }
            for(float factor:{1.f,1.5f,2.f,3.f}) {
                std::array<U32,16> regs;regs.fill(0xdeadbeef);
                std::array<U32,64> floats;floats.fill(0x12345678);
                regs[4]=0x8000000;regs[1]=0x8100000;
                m.runtime={{regs[4],0xff06007f},{regs[4]+0x1c,0},{regs[1],native}};
                const auto old_regs=regs;const auto old_floats=floats;const auto old_memory=m.runtime;
                Load(m,i,regs,floats,factor);
                U32 expected;
                if(h.distance!=0.f) {
                    float d=h.distance+h.shift_base*(factor-1.f);
                    expected=std::bit_cast<U32>(h.squared?d*d:d);
                    if(h.negate)expected=0u-expected;
                } else expected=std::bit_cast<U32>(2560000.f*factor*factor);
                for(unsigned r=0;r<regs.size();++r)assert(regs[r]==(!h.floating&&r==h.reg?expected:old_regs[r]));
                for(unsigned r=0;r<floats.size();++r)assert(floats[r]==(h.floating&&r==h.reg?expected:old_floats[r]));
                assert(m.runtime==old_memory);
            }
        }
        // Cluster gating: bushes and rocks extend; unknown type and actor don't.
        for(unsigned type=0;type<4;++type)for(U32 id:{0x6007fu,0x60065u}) {
            std::array<U32,16> regs{};std::array<U32,64> floats{};
            regs[4]=0x8000000;regs[1]=0x8100000;
            m.runtime={{regs[4],id},{regs[4]+0x1c,type},{regs[1],std::bit_cast<U32>(2560000.f)}};
            Load(m,7,regs,floats,3.f);
            assert(floats[1]==std::bit_cast<U32>(id==0x6007f&&type<3?23040000.f:2560000.f));
        }
    }
    // A patch at 2000 units is rejected at 1x but remains inside every 3x
    // visibility gate. The opaque-to-transparent fade remains 100 units.
    for(float factor:{1.f,2.f,3.f}) {
        const float shift=980.f*(factor-1.f);
        const float start=1080.f+shift,end=1180.f+shift;
        assert(end-start==100.f);
        assert((2000.f<1280.f+shift)==(factor>1.f));
        assert(std::bit_cast<float>(Threshold(Hooks[2],factor))==end);
        assert(std::bit_cast<float>(Threshold(Hooks[4],factor))==start*start);
        for(int offset=0;offset<=100;++offset)
            assert(end-(start+offset)==100.f-offset);
        for(unsigned first:{9u,12u}) {
            float opaque=std::bit_cast<float>(Threshold(Hooks[first],factor));
            float gone=std::bit_cast<float>(Threshold(Hooks[first+1],factor));
            assert(gone-opaque==100.f);
            assert(Threshold(Hooks[first+1],factor)==Threshold(Hooks[first+2],factor));
        }
    }
    Memory empty;assert(!CodeMatches(empty,0));
    std::cout<<"Vegetation and rock checks passed (both executables, exact replay, scope, fades, register preservation).\n";
}
