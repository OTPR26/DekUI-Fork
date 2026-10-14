// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/camera_live.h"
#include "mm3d/widescreen.h"
namespace MM3D::SongEffect {
using U32=std::uint32_t;
constexpr U32 FirstSVC=0xffffa8;
struct Load {U32 pc,word,literal,next;unsigned reg;};
// Final billboard matrices for Oceff profiles91,96,A5,1D8,1DA,1DD.
// MOV r1,sp immediately before native model matrix upload.
constexpr std::array<Load,6> Loads{{
    {0x467f88,0xe1a0100d,0x467f84,0xebf65ac5,0},
    {0x489ba8,0xe1a0100d,0x489ba4,0xebf5d3bd,0},
    {0x489e10,0xe1a0100d,0x489e0c,0xebf5d323,0},
    {0x48a06c,0xe1a0100d,0x48a068,0xebf5d28c,0},
    {0x48a290,0xe1a0100d,0x48a28c,0xebf5d203,0},
    {0x48a4a8,0xe1a0100d,0x48a4a4,0xebf5d17d,0},
}};
inline U32 Instruction(U32 pc,U32 word){
    for(unsigned i=0;i<Loads.size();++i)if(pc==Loads[i].pc&&word==Loads[i].word)return 0xef000000|(FirstSVC+i);
    return word;
}
template<class M> bool CodeMatches(M& m){
    for(auto l:Loads)if(m.Read(l.pc)!=std::optional<U32>(l.word)||
        m.Read(l.pc+4)!=std::optional<U32>(l.next)||m.Read(l.literal)!=std::optional<U32>(0xe59501fc))return false;
    return true;
}
inline U32 Scale(float fov,bool wide,float zoom){
    if(!std::isfinite(fov)||fov<45||fov>70||!std::isfinite(zoom)||zoom<1||zoom>1.5f)return 0x3dcccccd;
    constexpr float rad=3.14159265358979323846f/360.f;
    // Uniform growth preserves the circular silhouette. Its horizontal extent
    // covers the wider frustum; vertical excess naturally clips to the viewport.
    const float factor=std::tan(fov*rad)/std::tan(45.f*rad)*zoom*(wide?16.f/15.f:1.f);
    return std::bit_cast<U32>(.1f*factor);
}
template<class M> bool Expand(M& m,U32 matrix,float fov,bool wide,float zoom) {
    if(matrix<0x100000||matrix>0x3fffffc0||(matrix&3))return false;
    const float factor=std::bit_cast<float>(Scale(fov,wide,zoom))/.1f;
    if(factor==1)return true;
    std::array<U32,6> values{};unsigned i=0;
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<2;++col) {
        auto bits=m.Read(matrix+16*row+4*col);if(!bits)return false;
        const float value=std::bit_cast<float>(*bits)*factor;
        if(!std::isfinite(value))return false;
        values[i++]=std::bit_cast<U32>(value);
    }
    i=0;for(unsigned row=0;row<3;++row)for(unsigned col=0;col<2;++col)
        m.Write(matrix+16*row+4*col,values[i++]);
    return true;
}

}
