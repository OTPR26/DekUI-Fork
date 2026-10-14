// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include "mm3d/hud_adjustments.h"

namespace MM3D::MinimapShortcut {
using U32=std::uint32_t;
constexpr U32 PC=0x7d06dc,Word=0xe3a00005,SVC=0xfffc82;
constexpr U32 RoutePC=0x7d06c4,RouteWord=0xe1110005,SelectBranch=0xea000002;
constexpr U32 Sound=0x0100077f,PlaySound=0x2006e4,Resume=0x7d06f8;
struct Signature {U32 start,size,hash;};
constexpr std::array<Signature,3> Signatures{{
    {0x7d0660,0xd4,0x8771ec7d}, // Restoration UiScheduleTriggerHook
    {0x2006e4,0x64,0x39a01581}, // native PlayEffect, including its tail call
    {0x87846c,0x14,0x6a7cceeb}, // gameplay trigger trampoline
}};
inline U32 Instruction(U32 pc,U32 word) {
    // MenuShortcuts otherwise translates this site into an early return.
    // Skip only the disabled ZR+START block and reach the native ZR+SELECT test.
    // The adapter tries these fully guarded instructions before MenuShortcuts;
    // a mismatched binary retains its existing disabled-shortcut behavior.
    if(pc==RoutePC&&word==RouteWord)return SelectBranch;
    return pc==PC&&word==Word?0xef000000|SVC:word;
}
template<class M> bool CodeMatches(M& m) {
    for(const auto& s:Signatures) {
        U32 hash=2166136261u;
        for(U32 off=0;off<s.size;off+=4) {
            auto word=m.Read(s.start+off);if(!word)return false;
            for(unsigned i=0;i<4;++i)hash=(hash^((*word>>(8*i))&255))*16777619u;
        }
        if(hash!=s.hash)return false;
    }
    // FIRSTPERSON's native camera-zoom cue and effect call.
    return m.Read(0x59ba10)==0xea0b7295u && m.Read(0x1ee324)==0xe59f00bcu && m.Read(0x1ee328)==0xeb0048edu &&
           m.Read(0x1ee3e8)==Sound;
}
template<class M,class R> bool Handle(M& m,R& r) {
    r[0]=5; // displaced MOV; unsupported context follows the native map shortcut
    const U32 game=r[4];
    const auto ui=m.Read(0x72f5c0),main=m.Read(0x656b04),current=m.Read(0x72f5c4);
    const auto state=m.Read(game+0x13c),held=m.Read(game+0x30),pressed=m.Read(game+0x34);
    if(!game||(game&3)||!ui||!*ui||m.Read(*ui+0x16c)!=game||
       !main||!*main||current!=main||m.Read(0x72f5c8)!=0||
       !state||(*state&0xffff)!=0x0402||!held||!pressed||
       !(*held&0x8000)||!(*pressed&4))return false;
    HudAdjustments::ToggleMinimap();
    // Already inside the native ZR+new-SELECT branch. Bypass its map-mode
    // mutation/open request, play the first-person cue, then rejoin the native
    // SELECT held/pressed clearing and register-restoring epilogue. Holding the
    // chord cannot repeat; a new SELECT press is needed for the next toggle.
    r[0]=Sound;r[14]=Resume;r[15]=PlaySound;
    return true;
}
} // namespace MM3D::MinimapShortcut
