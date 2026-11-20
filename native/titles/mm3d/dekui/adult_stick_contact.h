// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "player_model_scale.h"
#include <algorithm>

namespace Mm3dDekui::AdultStickContact {
using PlayerModelScale::U32;
constexpr U32 PC=0x4aab64,Word=0xee700ae1,FirstSVC=0xfffb74;
// Native v1.1 relocation: r11 remains the player, r8 the play context.
// Exact owned-code fingerprint covers the held-item check, hand-joint query,
// distance arithmetic and original radius comparison.
// Obj_Syokudai checks the left-hand joint against a sphere 67 units above
// the torch base. Adult draw scaling raises that joint beyond the sphere.
// Extend only the upward reach by the height ratio; retain its 20-unit radius,
// native ignition/timer/arrow logic, and the lower edge of the original sphere.
inline U32 Instruction(U32 pc,U32 word) {
    return pc==PC&&word==Word ? 0xef000000|FirstSVC : word;
}
template<class M> bool CodeMatches(M& m) {
    U32 hash=2166136261u;
    for(U32 at=0x4aab00;at<0x4aab84;at+=4) {
        auto word=m.Read(at);if(!word)return false;
        for(unsigned i=0;i<4;++i)hash=(hash^((*word>>(8*i))&255))*16777619u;
    }
    const auto height=m.Read(0x4aad88),radiusSquared=m.Read(0x4aad8c);
    return hash==0xa3dd99c3u && height && *height==0x42860000u &&
        radiusSquared && *radiusSquared==0x43c80000u;
}
inline float VerticalDistance(float separation,float height,bool adult) {
    const float native=separation-height;
    if(!adult||height!=67.f||!std::isfinite(native))return native;
    return native-std::clamp(native,0.f,height*(PlayerModelScale::AdultScale-1.f));
}
template<class M,class R,class F> bool Apply(M& m,R& r,F& f,bool enabled) {
    const auto action=enabled ? m.Read(r[11]+0x1f8) : std::optional<U32>{};
    const bool adult=action&&(*action>>24)==7&&PlayerModelScale::HumanPlayer(m,r[11],r[8]);
    const float before=std::bit_cast<float>(f[1]),height=std::bit_cast<float>(f[3]);
    const float after=VerticalDistance(before,height,adult);
    f[1]=std::bit_cast<U32>(after); // Displaced VSUB.F32 s1,s1,s3 only.
    return adult&&after!=before-height;
}
}
