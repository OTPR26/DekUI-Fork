// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace MM3D::MovementSpeed {
using U32=std::uint32_t;
inline std::atomic<float> walk{1.f},climb{1.f},swim{1.f};
constexpr U32 FirstSVC=0xfffb40;
constexpr std::array<U32,4> PCs{0x1fbee4,0x1fbf00,0x1fbf18,0x1e5398};
constexpr std::array<U32,4> Words{0xed901a19,0xed901a1a,0xed901a1b,0xed800a13};
// Actor integration, ladder/vine playback calculation, and the player's call
// into integration. Verified against the bundled v1.1 Restoration code and
// the supported alternate controls variant. Never patch ROM data or persistent velocity.
struct Signature {U32 start,size,hash;};
constexpr std::array<Signature,3> Signatures{{
    {0x1fbed8,0x58,0xed93387c},{0x1e5230,0x180,0x6cffbd6b},
    {0x205b7c,0x18,0xaea01bfa}
}};
template<class M> bool CodeMatches(M& m) {
    for(const auto& s:Signatures) {
        U32 hash=2166136261u;
        for(U32 off=0;off<s.size;off+=4) {
            const auto word=m.Read(s.start+off);if(!word)return false;
            for(unsigned i=0;i<4;++i)hash=(hash^((*word>>(8*i))&255))*16777619u;
        }
        if(hash!=s.hash)return false;
    }
    return true;
}
inline U32 Instruction(U32 pc,U32 word) {
    for(unsigned i=0;i<PCs.size();++i)if(pc==PCs[i]&&word==Words[i])return 0xef000000 | (FirstSVC + i);
    return word;
}
inline float Multiplier(float value) {return std::isfinite(value)?std::clamp(value,.5f,3.f):1.f;}
inline U32 Scale(U32 bits,float multiplier) {
    const float value=std::bit_cast<float>(bits);multiplier=Multiplier(multiplier);
    // At 1x retain the exact original bits, including signed zero/NaN.
    if(multiplier==1.f||!std::isfinite(value)||std::abs(value)>100.f)return bits;
    return std::bit_cast<U32>(value*multiplier);
}
inline bool Pointer(U32 p,U32 size) {return p>=0x08000000&&!(p&3)&&p<0x40000000&&size<=0x40000000-p;}
template<class M> bool Player(M& m,U32 actor,U32 ctx) {
    if(!Pointer(actor,0x12ccc)||ctx<0x100000||ctx>=0x3fffdf20||(ctx&3))return false;
    const auto id=m.Read(actor),type=m.Read(ctx+0x13c),first=m.Read(ctx+0x20dc);
    return id&&(*id&0xffffff)==0x020000&&type&&(*type&65535)==0x0402&&first&&*first==actor;
}
template<class M> U32 Context(M& m) {
    const auto root=m.Read(0x72f5c0);
    if(!root||*root<0x100000||*root>=0x3ffffe90||(*root&3))return 0;
    const auto ctx=m.Read(*root+0x16c);return ctx?*ctx:0;
}
// Dead, talking, riding, frozen/cutscene-controlled players stay native.
constexpr U32 Blocked=0x80|0x40|0x800000|0x10000000|0x20000000;
template<class M> float VelocityMultiplier(M& m,U32 actor,unsigned axis,float walking,float swimming) {
    if(!Player(m,actor,Context(m)))return 1.f;
    const auto flags=m.Read(actor+0x11db0),flags2=m.Read(actor+0x11db4),flags3=m.Read(actor+0x11db8),ground=m.Read(actor+0x94);
    if(!flags||!flags2||!flags3||!ground||(*flags&Blocked))return 1.f;
    if((*flags&0x08000000)&&!(*ground&1)) {
        // Surface buoyancy remains native. Underwater propulsion (including
        // Zora fast swim) scales vertically as well as horizontally.
        if(axis==1&&!(*flags2&0x400)&&!(*flags3&0x8000))return 1.f;
        return Multiplier(swimming);
    }
    // Only grounded locomotion marked Moving by the native state handler.
    // Climbing/ledge hangs, jumping, rolls, attacks and flower flight are excluded.
    if(axis==1||!(*ground&1)||!(*flags2&0x20)||
       (*flags&(0x08000000|0x40000|0x80000|0x2000|0x4000|0x200000))||
       (*flags3&(0x8|0x100|0x200|0x1000|0x2000|0x80000|0x100000)))return 1.f;
    return Multiplier(walking);
}
template<class M,class R,class F> void Apply(M& m,R& r,F& f,unsigned hook,float walking,float climbing,float swimming) {
    if(hook<3) {
        // Displaced VLDR s2: scale only this integration operand. Native
        // acceleration, state velocity, gravity and platform carry are retained.
        const auto native=m.Read(r[0]+0x64+4*hook);
        f[2]=native?Scale(*native,VelocityMultiplier(m,r[0],hook,walking,swimming)):0;
    } else if(hook==3) {
        // Climbing recomputes signed playback speed each tick. Scale that
        // fresh value, so movement/root motion and animation remain together.
        U32 value=f[0];
        const auto state=m.Read(r[4]+0xe14),flags=m.Read(r[4]+0x11db0);
        if(r[0]==r[4]+0x334&&Player(m,r[4],r[6])&&state&&*state==0x1e5230&&flags&&!(*flags&Blocked))
            value=Scale(value,climbing);
        f[0]=value;m.Write(r[0]+76,value); // displaced VSTR s0
    }
}
}
