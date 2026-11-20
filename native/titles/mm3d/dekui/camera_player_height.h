// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "player_model_scale.h"

namespace Mm3dDekui::CameraPlayerHeight {
using U32=std::uint32_t;
constexpr U32 Pc=0x1f2c4c, Word=0x0ddf0a0c, Svc=0xfffb6f;
inline U32 Instruction(U32 pc,U32 word) {
    // Preserve EQ: only the native human-height load is displaced.
    return pc==Pc&&word==Word ? 0x0f000000|Svc : word;
}
template<class M> bool HashMatches(M& m,U32 lo,U32 hi,U32 expected) {
    U32 hash=2166136261u;
    for(U32 at=lo;at<hi;at+=4) {
        auto word=m.Read(at);if(!word)return false;
        for(unsigned i=0;i<4;++i)hash=(hash^((*word>>(i*8))&255))*16777619u;
    }
    return hash==expected;
}
template<class M> bool CodeMatches(M& m) {
    return HashMatches(m,0x1f2c0c,0x1f2c90,0x80368970)&&
           HashMatches(m,0x171d20,0x171d50,0xed747ebc);
}
template<class M> bool CameraCaller(M& m,U32 lr) {
    if(lr<0x100004||!PlayerModelScale::Pointer(lr-4,4))return false;
    auto word=m.Read(lr-4);
    if(!word||(*word&0xff000000)!=0xeb000000)return false;
    const auto displacement=static_cast<std::int32_t>((*word&0xffffff)<<8)>>6;
    const U32 target=static_cast<U32>(lr+4+displacement);
    if(target==0x171d20)return true;
    // Three native camera algorithms query their camera's +A4 target directly.
    // Keep all other direct actor-height consumers outside this correction.
    const U32 pc=lr-4;
    return target==0x1f2c0c&&((pc==0x3dedd4&&m.Read(pc-4)==0xe59400a4)||
        ((pc==0x447a60||pc==0x44b9d8)&&m.Read(pc-4)==0xe59000a4));
}
template<class M,class R,class F> void Apply(M& m,R& r,F& f,bool enabled) {
    f[1]=std::bit_cast<U32>(44.f); // Replay native VLDR; mounted offset in s0 stays intact.
    if(!enabled||r[0]!=4||r[1]<0x11000||!CameraCaller(m,r[14]))return;
    const U32 actor=r[1]-0x11000;
    auto root=m.Read(0x72f5c0);
    auto context=root&&PlayerModelScale::Pointer(*root,0x170)?m.Read(*root+0x16c):std::optional<U32>{};
    if(!context||!PlayerModelScale::HumanPlayer(m,actor,*context))return;
    auto scale=m.Read(actor+0x5c);
    if(!scale||PlayerModelScale::Scale(*scale,true)==*scale)return;
    f[1]=std::bit_cast<U32>(44.f*PlayerModelScale::AdultScale);
    static std::atomic<U32> reported{0};const U32 bit=1u<<(((r[14]>>2)^(r[14]>>7))&31);
    if(!(reported.fetch_or(bit)&bit))std::fprintf(stderr,
        "REMASKED_CAMERA_PLAYER_HEIGHT caller=%08x height=%.3f\n",r[14]-4,44.f*PlayerModelScale::AdultScale);
    // No actor/collision/animation writes. Direct non-camera height queries
    // retain native behavior; repeated calls cannot compound the correction.
}
}
