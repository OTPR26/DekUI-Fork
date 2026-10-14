// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

namespace MM3D::ScreenShrink {
using U32=std::uint32_t;
inline std::atomic<float> presentation_scale{1.f};
inline std::atomic<bool> native_normalized{false};
inline std::atomic<U32> normalized_effect{0};
constexpr U32 PC=0x15f5a0,Word=0xed800a01,SVC=0xfffe70;
constexpr U32 GuardPC=0x171a34,GuardWord=0xaa000038,GuardSVC=SVC+1;
constexpr U32 Minimum=0x3d03126f;
inline U32 Instruction(U32 pc,U32 word){
    if(pc==PC&&word==Word)return 0xef000000|SVC;
    if(pc==GuardPC&&word==GuardWord)return 0xef000000|GuardSVC;
    return word;
}
template<class M> bool CodeMatches(M& m) {
    return m.Read(GuardPC)==std::optional<U32>(GuardWord)&&
        m.Read(0x171b38)==std::optional<U32>(Minimum)&&
        m.Read(PC)==std::optional<U32>(Word)&&
        m.Read(PC+4)==std::optional<U32>(0xeb004910)&&
        m.Read(0x15f63c)==std::optional<U32>(0x7785d8)&&
        m.Read(0x15f640)==std::optional<U32>(0x3a83126f)&&
        m.Read(0x15f590)==std::optional<U32>(0xed910ab1);
}
inline float Normalize(float value) {
    return std::isfinite(value)&&value>=1.f&&value<=1000.f?value*.001f:1.f;
}
// Native Play_Draw 15F590 reads SaveContext+36C4, multiplies by .001,
// and passes it to 1719EC. That routine centers the scene using (1-scale)/2.
// Keep the game-owned amount intact; move only its rendering to the compositor.
template<class M> float Read(M& m) {
    if(m.Read(0x15f63c)!=std::optional<U32>(0x7785d8)||
       m.Read(0x15f640)!=std::optional<U32>(0x3a83126f)||
       m.Read(0x15f590)!=std::optional<U32>(0xed910ab1))return 1.f;
    const auto ui=m.Read(0x72f5c0);
    if(!ui||*ui<0x100000||*ui>0x3ffffe90)return 1.f;
    const auto game=m.Read(*ui+0x16c);
    if(!game||*game<0x100000||*game>0x3ffffec0)return 1.f;
    const auto type=m.Read(*game+0x13c);
    if(!type||((*type>>8)&255)!=4)return 1.f;
    const auto value=m.Read(0x77889c);
    return value?Normalize(std::bit_cast<float>(*value)):1.f;
}
// Replay the displaced VSTR, substituting a full-size scene only when the
// single-screen compositor can take responsibility for the native effect.
// The amount in SaveContext and every UI/model/editor matrix stay untouched.
// Captured pause backgrounds now contain an unshrunk world, so final fitting
// cannot double-shrink them.
template<class M> void Store(M& m,U32 effect,U32 original,bool supported) {
    const bool active=supported&&CodeMatches(m)&&effect>=0x100000&&effect<0x3fffffe8&&
        m.Read(effect+4).has_value();
    m.Write(effect+4,active?std::bit_cast<U32>(1.f):original);
    normalized_effect=active?effect:0;
    native_normalized=active;
    presentation_scale=active?Read(m):1.f;
}
// Native BGE skips the copy-mesh/UV update at scale1. When we own the
// full-size copy, run the normal construction path instead. R3 would only
// have been loaded by LDRLT for a sub-unit scale, so supply its native lower
// bound too. The following CMP rebuilds flags; preserve incoming flags here.
inline U32 Guard(U32 effect,U32 scale_bits,U32 cpsr,U32& r3) {
    if(native_normalized.load()&&normalized_effect.load()==effect&&
       scale_bits==std::bit_cast<U32>(1.f)) {
        r3=Minimum;return GuardPC+4;
    }
    const bool negative=(cpsr>>31)&1,overflow=(cpsr>>28)&1;
    return negative==overflow?0x171b1c:GuardPC+4;
}
template<class M> void Update(M& m) {
    presentation_scale=native_normalized.load()?Read(m):1.f;
}
// Final framebuffer and all copied UI share this rectangle. F1 is composed
// afterward by the host window and never passes through these bounds.
template<class R> R Bounds(R r,float scale) {
    if(!std::isfinite(scale)||scale<=0||scale>=1)return r;
    const auto dx=static_cast<decltype(r.left)>(std::lround((r.right-r.left)*(1-scale)*.5f));
    const auto dy=static_cast<decltype(r.top)>(std::lround((r.bottom-r.top)*(1-scale)*.5f));
    r.left+=dx;r.right-=dx;r.top+=dy;r.bottom-=dy;return r;
}
}
