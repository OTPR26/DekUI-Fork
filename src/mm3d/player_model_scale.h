// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace MM3D::PlayerModelScale {
using U32=std::uint32_t;
// Native Zora model: sole-to-crown 6555.463 model units. Converted adult:
// 3986.693 units. Both use the normal 0.01 actor draw scale.
inline constexpr float AdultScale=6555.463137f/3986.693367f;
constexpr U32 FirstSVC=0xfffb70;
constexpr std::array<U32,4> PCs{0x182a34,0x182a94,0x182ab0,0x182ac8};
constexpr std::array<U32,4> Words{0xed942a17,0xe5942058,0xe594205c,0xe5940060};
inline U32 Instruction(U32 pc,U32 word) {
    for(unsigned i=0;i<PCs.size();++i)
        if(pc==PCs[i]&&word==Words[i])return 0xef000000 | (FirstSVC + i);
    return word;
}
template<class M> bool CodeMatches(M& m) {
    U32 hash=2166136261u;
    for(U32 at=0x182a2c;at<0x182ae4;at+=4) {
        const auto word=m.Read(at);if(!word)return false;
        for(unsigned i=0;i<4;++i)hash=(hash^((*word>>(8*i))&255))*16777619u;
    }
    return hash==0x00adbca2u; // USA Rev1 and the installed Restoration controls variant.
}
inline bool Pointer(U32 at,U32 size) {
    return at>=0x100000 && !(at&3) && at<0x40000000 && size<=0x40000000-at;
}
template<class M> bool HumanPlayer(M& m,U32 actor,U32 context) {
    if(!Pointer(actor,0x200)||!Pointer(context,0x20e0))return false;
    const auto id=m.Read(actor),form=m.Read(actor+0x1fc);
    const auto type=m.Read(context+0x13c),player=m.Read(context+0x20dc);
    return id && (*id&0xffffff)==0x020000 && form && (*form>>24)==4 &&
        type && (*type&65535)==0x0402 && player && *player==actor;
}
inline U32 Scale(U32 bits,bool adult) {
    if(!adult)return bits;
    const float value=std::bit_cast<float>(bits);
    // Keep native giant-mask scaling and malformed values unchanged.
    if(!std::isfinite(value)||value==0.f||std::abs(value)>.02f)return bits;
    return std::bit_cast<U32>(value*AdultScale);
}
template<class M,class R,class F> void Apply(M& m,R& r,F& f,unsigned hook,bool enabled) {
    if(hook>=PCs.size())return;
    const unsigned offset=hook==1?0x58:hook==3?0x60:0x5c;
    const auto word=m.Read(r[4]+offset);
    const U32 value=word?Scale(*word,enabled&&HumanPlayer(m,r[4],r[6])):0;
    // Replay the displaced operand load only. Actor scale, collision, velocity,
    // positions and animation data are never rewritten or compounded.
    if(hook==0)f[4]=value;
    else r[hook==3?0:2]=value;
}
}
