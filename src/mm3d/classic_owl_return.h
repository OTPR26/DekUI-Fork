// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/single_screen_text.h"
#include <array>
#include <bit>
#include <cmath>
namespace MM3D::ClassicSaving {
// Player's first normal standing update after the entrance animation, before
// that update can process walking. Use native conversation setup/cleanup; do
// not pause the world or borrow the statue's save/choice actor state machine.
struct OwlReturn {
    using U32=std::uint32_t;
    U32 player=0,play=0,old_message=0,target=0;
    bool focus_sound_pending=false;
    template<class M> static U32 FindOwl(M& m,U32 context,U32 link) {
        U32 best=0;float distance=400.f*400.f;
        auto x=m.Read(link+0x24),z=m.Read(link+0x2c);
        if(!x||!z)return 0;
        U32 actor=m.Read(context+0x2118).value_or(0); // Item actor list (type 7).
        for(unsigned n=0;actor&&n<256;++n) {
            if(!SingleScreenText::Pointer(actor,0x290)||(actor&3))break;
            const auto id=m.Read(actor),params=m.Read(actor+0x1c);
            if(id&&(*id&0xffff)==0x1b2&&params&&
               m.Read(actor+0x268)==0x58d8d4) {
                auto ax=m.Read(actor+0x24),az=m.Read(actor+0x2c);
                if(ax&&az) {
                    const float dx=std::bit_cast<float>(*ax)-std::bit_cast<float>(*x);
                    const float dz=std::bit_cast<float>(*az)-std::bit_cast<float>(*z);
                    const float d=dx*dx+dz*dz;
                    if(std::isfinite(d)&&d<distance){best=actor;distance=d;}
                }
            }
            actor=m.Read(actor+0x134).value_or(0);
        }
        return best;
    }
    template<class M,class R> bool Enter(M& m,R& r) {
        r[4]=r[0]; // Displaced MOV after PUSH {r4-r11,lr}.
        if(!ResumeNoticeArmed())return false;
        const U32 link=r[0],context=r[1];
        if(!SingleScreenText::Pointer(link,0x13000)||!SingleScreenText::Pointer(context,0x10000)||
           (link&3)||(context&3)||m.Read(context+0x20dc)!=link||
           m.Read(link+0xe14)!=0x1fcb34)return false;
        const auto state=m.Read(context+0x13c),message=m.Read(link+0x118);
        const auto window=m.Read(0x6b3724);
        if(!state||(*state&0xffff)!=0x402||!message||!window||
           !SingleScreenText::Pointer(*window,0xa34)||m.Read(*window+0x3dc)!=0||
           !m.Read(link+0x11dd0))return false;
        const U32 owl=FindOwl(m,context,link);if(!owl)return false;
        std::array<U32,9> saved{};
        for(unsigned i=0;i<saved.size();++i) {
            auto value=m.Read(r[13]+i*4);if(!value)return false;saved[i]=*value;
        }
        // Preflight the exact supported-language payload before entering a conversation.
        // An unsupported resource must never fall back to a Yes/No save prompt.
        resume_text_active=true;
        if(!SingleScreenText::Apply(m,0x6b3738,0xc01)) {
            resume_text_active=false;resume_notice=false;return false;
        }
        player=link;play=context;old_message=*message>>16;
        target=owl;focus_sound_pending=true;
        m.Write(link+0x118,(*message&0xffff)|(0xc01u<<16));
        m.Write(link+0x11dd0,owl);
        resume_notice=false;
        // Balance the idle prologue and tail-call the verified conversation
        // initializer. It opens the message, installs 0x22d06c and locks input.
        // Owl talking/activation flags are untouched, so no save is offered.
        for(unsigned i=0;i<8;++i)r[4+i]=saved[i];
        r[14]=saved[8];r[13]+=36;
        r[0]=context;r[1]=link;r[15]=0x224f14;
        return true;
    }
    template<class M,class R> bool SuppressFocusSound(M& m,const R& r) {
        // Traced camera attention-urgency cue at 0x18b5e4. r6 is Camera,
        // whose +0xa0 points to Play. Suppress only this return's first cue.
        if(!focus_sound_pending||!player||r[0]!=0x010007a3||
           !SingleScreenText::Pointer(r[6],0xa4)||m.Read(r[6]+0xa0)!=play||
           m.Read(player+0x11dd0)!=target||m.Read(player+0xe14)!=0x22d06c)return false;
        focus_sound_pending=false;
        return true;
    }
    template<class M> bool HideFocusBars(M& m) const {
        // Draw-only override: leave the native camera and bar animation intact.
        // The entrance can draw bars BEFORE Enter intercepts the first idle
        // update. Cover the verified pending resume as well as its dialogue.
        // Once Enter owns the notice, its lifetime is authoritative. Native
        // camera/actor state changes during the first message update; using
        // those transient fields as extra guards lets one bar frame escape.
        // Finish and new-load reset end this scope explicitly.
        if(player&&resume_text_active)return true;
        if(!ResumeNoticeArmed())return false;
        const auto context=m.Read(0x72f5c0);
        if(!context||!SingleScreenText::Pointer(*context,0x170))return false;
        const auto current=m.Read(*context+0x16c);
        if(!current||!SingleScreenText::Pointer(*current,0x10000))return false;
        const auto state=m.Read(*current+0x13c),link=m.Read(*current+0x20dc);
        if(!state||(*state&0xffff)!=0x402||!link||
           !SingleScreenText::Pointer(*link,0x13000))return false;
        return true;
    }
    template<class M,class R> bool Finish(M& m,const R& r) {
        if(!player||r[4]!=player||r[6]!=play||r[0]!=2)return false;
        // Native conversation handler now releases input/camera normally.
        const auto message=m.Read(player+0x118);
        if(message)m.Write(player+0x118,(*message&0xffff)|(old_message<<16));
        player=play=old_message=target=0;focus_sound_pending=false;resume_text_active=false;
        return true;
    }
};
}
