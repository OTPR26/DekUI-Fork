// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "player_model_scale.h"

namespace MM3D::CameraPlayerHeight {
using U32=std::uint32_t;
constexpr U32 Pc=0x1f2c4c, RestorationPc=0x1f2e5c, Word=0x0ddf0a0c, Svc=0xfffb6f;
constexpr U32 NativeHeight = std::bit_cast<U32>(44.f);
inline U32 Instruction(U32 pc,U32 word) {
    // Preserve EQ: only the native human-height load is displaced.
    return (pc==Pc||pc==RestorationPc)&&word==Word ? 0x0f000000|Svc : word;
}
template<class M> bool HashMatches(M& m,U32 lo,U32 hi,U32 expected) {
    U32 hash=2166136261u;
    for(U32 at=lo;at<hi;at+=4) {
        auto word=m.Read(at);if(!word)return false;
        for(unsigned i=0;i<4;++i)hash=(hash^((*word>>(i*8))&255))*16777619u;
    }
    return hash==expected;
}
template<class M> bool CodeMatches(M& m,U32 pc) {
    if(pc==RestorationPc)
        return HashMatches(m,0x1f2e1c,0x1f2ea0,0x80368970)&&
               HashMatches(m,0x171e7c,0x171eac,0xb935a93d);
    return pc==Pc&&HashMatches(m,0x1f2c0c,0x1f2c90,0x80368970)&&
           HashMatches(m,0x171d20,0x171d50,0xed747ebc);
}
template<class M> bool CameraCaller(M& m,U32 lr) {
    if(lr<0x100004||!PlayerModelScale::Pointer(lr-4,4))return false;
    auto word=m.Read(lr-4);
    if(!word||(*word&0xff000000)!=0xeb000000)return false;
    const auto displacement=static_cast<std::int32_t>((*word&0xffffff)<<8)>>6;
    const U32 target=static_cast<U32>(lr+4+displacement);
    if(target==0x171d20||target==0x171e7c)return true;
    // Three native camera algorithms query their camera's +A4 target directly.
    // Keep all other direct actor-height consumers outside this correction.
    const U32 pc=lr-4;
    const bool restoration=target==0x1f2e1c;
    if(!restoration&&target!=0x1f2c0c)return false;
    const U32 offset=restoration?0x158:0;
    const U32 original=pc+offset;
    return (original==0x3dedd4&&m.Read(pc-4)==0xe59400a4)||
           ((original==0x447a60||original==0x44b9d8)&&m.Read(pc-4)==0xe59000a4);
}
template<class M,class R,class F> void Apply(M& m,R& r,F& f,bool enabled) {
    f[1] = NativeHeight; // Replay native VLDR; mounted offset in s0 stays intact.
    if(!enabled||r[0]!=4||r[1]<0x11000||!CameraCaller(m,r[14]))return;
    const U32 actor=r[1]-0x11000;
    auto root=m.Read(0x72f5c0);
    auto context=root&&PlayerModelScale::Pointer(*root,0x170)?m.Read(*root+0x16c):std::optional<U32>{};
    if(!context||!PlayerModelScale::HumanPlayer(m,actor,*context))return;
    auto scale=m.Read(actor+0x5c);
    if(!scale||PlayerModelScale::Scale(*scale,true)==*scale)return;
    f[1]=std::bit_cast<U32>(44.f*PlayerModelScale::AdultScale);
    // No actor/collision/animation writes. Direct non-camera height queries
    // retain native behavior; repeated calls cannot compound the correction.
}
}
