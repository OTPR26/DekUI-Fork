// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/classic_saving.h"
#include "mm3d/owl_sword.h"
#include "mm3d/classic_owl_presentation.h"
#include "mm3d/classic_owl_return.h"
#include <array>
#include <optional>
#include <vector>
namespace MM3D::ClassicSaving {
using U32=std::uint32_t;
constexpr U32 FirstSVC=0xfffa60;
constexpr std::array<std::array<U32,2>,20> Sites{{
    {0x7d1b14,0xe5903180}, {0x7d1b2c,0xe5943180},
    {0x18da54,0xe1a04000}, {0x18ad74,0xe5900008},
    {0x18adc0,0xe1d401b0}, {0x4bfcc8,0xe320f000},
    {0x48e9f4,0xe59f1014}, {0x346b48,0xe59f41bc}, {0x346b28,0xe1a00005},
    {0x4bfd74,0xe2841f83},
    {0x1d85bc,0xe1a04000}, {0x607504,0xe5c02058},
    {0x4bfc38,0xebf43c79}, {0x4bfcd8,0xebf43c51},
    {0x1d8568,0xe5d40053},
    {0x577ac8,0xe59f10a4}, // Newly activated owl: immediate save prompt.
    {0x22d0b0,0xe2847801}, // Player conversation: after native message-state query.
    {0x1fcb38,0xe1a04000}, // First normal player idle update, before input.
    {0x18b5e4,0xeb01d43e}, // Camera attention-urgency cue on resume.
    {0x173af4,0xe1d040d1}, // Letterbox draw: signed top/bottom bar height.
}};
constexpr std::array<std::array<U32,3>,18> Signatures{{
    {0x7d1a74,0x158,0x4fa326fc}, {0x18da50,0xc0,0x86013a03},
    {0x18ad44,0x9c,0x4c40bdbb}, {0x346ab0,0xa0,0x6bdfc5d2},
    {0x4bfcbc,0xbc,0x04fde24f}, {0x48e9d4,0x3c,0x85437f9b},
    {0x4bfd74,0x4c,0xe653bc8e},
    {0x1d85b8,0x10,0xb4acd546}, {0x6074f8,0x14,0x2739aabe},
    {0x4bfc30,0x14,0x4ee9e833},
    {0x1d8558,0x28,0x1e79d148},
    {0x577a8c,0xfc,0xe7c88f80},
    {0x22d06c,0xcc,0xd51e15d0}, // Native conversation finished branch and cleanup.
    {0x1fcb34,0x40,0x795ab2ea},
    {0x224f14,0x74,0xb6704265},
    {0x18b31c,0x14,0xf0d9bea3},
    {0x18b598,0x84,0x1bf7af83},
    {0x173ad0,0x160,0xe70bc6c9},
}};
inline U32 Instruction(U32 pc,U32 word) {
    for(unsigned i=0;i<Sites.size();++i)
        if(i!=5 && pc==Sites[i][0]&&word==Sites[i][1])return 0xef000000| (FirstSVC+i);
    return word;
}
// The JIT patches sites lazily, so later signature checks must see the
// original guest instruction even after an earlier site has become an SVC.
// Without this normalization only the first hook in the first signature range
// could install; the native writer hooks would then be skipped.
inline U32 OriginalInstruction(U32 pc,U32 word) {
    for (unsigned i=0; i<Sites.size(); ++i) {
        if (pc==Sites[i][0] && word==(0xef000000 | (FirstSVC+i)))
            return Sites[i][1];
    }
    return word;
}
template<class M> bool CodeMatches(M& m) {
    for(auto s:Signatures) {
        U32 hash=2166136261;
        for(U32 n=0;n<s[1];n+=4) {
            auto w=m.Read(s[0]+n);if(!w)return false;
            const U32 original=OriginalInstruction(s[0]+n,*w);
            for(unsigned b=0;b<4;++b)hash=(hash^((original>>(8*b))&255))*16777619;
        }
        if(hash!=s[2])return false;
    }
    return true;
}
struct Hook {
    OwlPresentation presentation;
    U32 cycle=0,writer=0,quit=0,pending_quit=0;
    U32 pending_actor=0;
    U32 quiet_owl=0,quiet_play=0,quiet_player=0;
    unsigned pending_frames=0;
    unsigned slot=3;
    Kind kind=Kind::None;
    bool native_success=false,cycle_committed=false,load_success=false;
    OwlReturn returning;
    struct CycleDraw {U32 address=0,flags=0;};
    std::vector<CycleDraw> cycle_draws;
    template<class M> void BeginCycleDraw(M& m,U32 layout) {
        cycle_draws.emplace_back();
        if(!enabled||!cycle)return;
        // The verified SotSave callback captures this instance. Live traces show
        // its native lifecycle remains Initialising (0x0a01) during the write.
        // Accept Initialising/Running, but never Changing or another state type.
        const auto state=m.Read(cycle+0x13c);
        if(!state||((*state&0xffff)!=0x0a01&&(*state&0xffff)!=0x0a02))return;
        const auto status=m.Read(cycle+0x180),window=m.Read(cycle+0x174);
        // StateEnding::message_window belongs to this Song-of-Time save.
        // Hide only its draw during snapshot/write/wait; never freeze updates.
        if(!status||*status<7||*status>9||!window||!*window||
           (layout!=*window&&m.Read(*window+0x358)!=layout))return;
        const U32 at=layout+8+0xd4;
        if(auto flags=m.Read(at)) {
            cycle_draws.back()={at,*flags};m.Write(at,*flags&~1u);
        }
    }
    template<class M> void EndCycleDraw(M& m) {
        if(cycle_draws.empty())return;
        const auto draw=cycle_draws.back();cycle_draws.pop_back();
        if(draw.address)m.Write(draw.address,draw.flags);
    }
    template<class M> static unsigned Slot(M& m) {
        auto n=m.Read(0x7887f0);return n&&(*n&0xffff)<3?(*n&0xffff):3;
    }
    template<class M> static bool State(M& m,U32 ptr,unsigned type) {
        if(!ptr||(ptr&3)||ptr>0xfffffe6c)return false;
        auto s=m.Read(ptr+0x13c);return s&&(*s&0xffff)==(type<<8|2);
    }
    template<class M> void Quit(M& m,U32 ctx) {
        if(enabled&&ctx) {
            auto state=m.Read(ctx+0x13c);
            if(state&&m.Read(ctx+0x10c)) {
                m.Write(ctx+0x10c,0);
                m.Write(ctx+0x13c,(*state&0xffff0000)|0x300); // native ChangeState(PrepareTitleScreen1)
            }
        }
        quit=0;
    }
    template<class M,class R,class Service> void Handle(M& m,R& r,unsigned site,Service& service) {
        switch(site) {
        case 0: {
            auto state=m.Read(r[0]+0x180);r[3]=state.value_or(0);
            // This site is only used by Restoration's Song-of-Time save
            // prompt.  Do not require the state object's packed type/status
            // word here: JIT execution can observe the transition between
            // those fields being initialized while this instruction runs.
            if(enabled&&r[3]==4) {
                cycle=r[0];cycle_committed=false;
                // MessageWindow and ending layouts were initialized by Restoration.
                // Enter native snapshot/write status, bypassing the choice only.
                m.Write(cycle+0x180,7);m.Write(cycle+0x188,0);r[3]=7;
            }
            break;
        }
        case 1: {
            auto state=m.Read(r[4]+0x180);r[3]=state.value_or(0);
            // Let Restoration show its normal success message after the write.
            // Only the earlier question is bypassed in classic mode.
            if(cycle==r[4] && r[3]>=10) {cycle=0;cycle_committed=false;}
            break;
        }
        case 2:
            presentation.Reset(m);
            r[4]=r[0];writer=r[0];native_success=false;kind=Kind::None;quit=0;slot=Slot(m);
            if(enabled&&slot<3) {
                // The link register is not stable across JIT block exits (and
                // differs between ARM/Thumb callers), so classify by the
                // native save-buffer identity and owning game state instead.
                if(cycle&&writer==cycle+0x148)kind=Kind::Cycle;
                else if(writer>=0x3bec)kind=Kind::Owl;
                if(kind!=Kind::None&&!service.Begin(slot,kind))kind=Kind::None;
                if(kind==Kind::Owl)presentation.Confirm(m,writer-0x3bec);
            }
            break;
        case 3: {
            // Capture the result BEFORE the native error handler can touch it.
            const auto result=m.Read(r[0]+8);r[0]=result.value_or(0);
            if(r[4]==writer&&kind!=Kind::None)
                native_success=result&&((*result&0x80000000)==0);
            break;
        }
        case 4:
            r[0]=m.Read(r[4]+0x10).value_or(0)&0xffff;
            if(r[0]==0&&r[4]==writer&&kind!=Kind::None) {
                if(enabled&&native_success&&service.Finish(slot,kind)) {
                    if(kind==Kind::Cycle)cycle_committed=true;
                    else {
                        quit=writer-0x3bec;
                        // Arm only after native write and sidecar verification.
                        // The retained confirmation page fades before exit.
                        pending_quit=quit; pending_actor=0; pending_frames=presentation.window?0:20;
                    }
                }
                if(!pending_quit)presentation.Reset(m);
                kind=Kind::None;writer=0;native_success=false;
            }
            break;
        case 5: // Preserve native owl message and location-closing transition.
            if(enabled && r[0]!=0 && quit==r[5]) {
                // Native code starts its location-closing transition after the
                // message is dismissed. Keep the checkpoint armed while that
                // native sequence runs; the safe post-update hook below waits
                // before returning to file select.
                pending_quit=quit; pending_frames=20; quit=0;
            }
            break;
        case 6: // Alternate statue completion retains its native behavior.
            r[1]=m.Read(0x48ea10).value_or(0);
            break;
        case 7: {
            // Actual file load, after native read and CommonData copy; not preview.
            r[4]=m.Read(0x346d0c).value_or(0);
            if(enabled&&load_success) {
                load_success=false;
                std::vector<std::uint8_t> bytes;bytes.reserve(0x13618);
                if(r[4]!=0x7751d8)break;
                for(U32 n=0;n<0x13618;n+=4) {
                    auto w=m.Read(r[4]+n);if(!w){bytes.clear();break;}
                    for(unsigned b=0;b<4;++b)bytes.push_back((*w>>(b*8))&255);
                }
                if(bytes.size()==0x13618)service.Load(Slot(m),bytes);
            }
            break;
        }
        case 8: {
            resume_notice=false;resume_text_active=false;returning={};
            r[0]=r[5];
            const auto result=m.Read(r[5]+8);
            load_success=result&&((*result&0x80000000)==0);
            break;
        }
        case 9: // Replay ADD r1,r4,#0x20c; keep the confirmed page through black.
            r[1]=r[4]+0x20c;
            if(!enabled) {
                presentation.Reset(m);pending_quit=pending_frames=0;
                exit_fade=ExitFade::Idle;
                break;
            }
            if(enabled && presentation.context==r[5]) {
                presentation.Tick(m);
                // Fifteen 30 Hz updates from Yes, including time spent saving.
                // Fade may run during the write; title exit still requires its
                // verified success. Failure resets the presentation and fade.
                if(presentation.frames>=15 && exit_fade==ExitFade::Idle)
                    exit_fade=ExitFade::Out;
            }
            if(enabled && pending_quit && pending_quit==r[5]) {
                if(pending_frames)--pending_frames;
                if(!pending_frames && presentation.window) {
                    if(exit_fade==ExitFade::Black) {
                        const U32 ctx=pending_quit;pending_quit=0;
                        presentation.Reset(m,false);Quit(m,ctx);
                        exit_title_ready=false;exit_fade=ExitFade::In;
                    }
                } else if(!pending_frames) {
                    const U32 ctx=pending_quit;pending_quit=0;Quit(m,ctx);
                }
            }
            break;
        case 10:
            // Hook is immediately after PUSH {r4,lr}, before VPUSH. Once
            // confirmed, return without advancing/closing the native text
            // engine. Its draw routine can still render the completed page.
            if(enabled && presentation.window && r[0]==presentation.window+0x368) {
                auto saved_r4=m.Read(r[13]),saved_lr=m.Read(r[13]+4);
                if(saved_r4&&saved_lr) {
                    r[4]=*saved_r4;r[15]=*saved_lr;r[13]+=8;
                    break;
                }
            }
            r[4]=r[0];presentation.Reveal(m,r[0]);break;
        case 11:
            if(!presentation.Choice(m,r[0]))OwlPresentation::Byte(m,r[0]+0x58,r[2]);
            break;
        case 15:
            quiet_owl=quiet_play=quiet_player=0;
            if(OwlSword::QuietCompletion(m,r[4])) {
                // Link is in Player's conversation handler, not its cutscene
                // dispatcher. Remember the exact conversation whose textbox
                // we omit, then let its normal finished path clean up next tick.
                const auto player=m.Read(r[6]+0x20dc);
                if(player&&*player>=0x100000&&*player<=0x3ffe0000&&!(*player&3)&&
                   m.Read(*player+0xe14)==0x22d06c&&m.Read(*player+0x11dd0)==r[4]) {
                    quiet_owl=r[4];quiet_play=r[6];quiet_player=*player;
                }
                r[0]=OwlSword::Idle;r[5]=0;r[15]=0x577adc;
                break;
            }
            r[1]=enabled?0xc01:m.Read(0x577b74).value_or(0);
            break;
        case 16:
            returning.Finish(m,r);
            r[7]=r[4]+0x10000; // Original ADD after Message_GetState.
            if(quiet_player&&r[4]==quiet_player) {
                if(r[6]==quiet_play&&r[0]==0&&
                   m.Read(quiet_player+0x11dd0)==quiet_owl&&
                   m.Read(quiet_owl+0x268)==OwlSword::Idle)r[0]=2;
                // One local result override, not a global message-state write.
                // Other actors, active textboxes and later A conversations are native.
                quiet_owl=quiet_play=quiet_player=0;
            }
            break;
        case 19: {
            const U32 height=(m.Read(r[0]).value_or(0)>>8)&255;
            r[4]=returning.HideFocusBars(m)?0:(height&128?height|0xffffff00u:height);
            break;
        }
        case 18:
            r[14]=Sites[18][0]+4;
            if(!returning.SuppressFocusSound(m,r))r[15]=0x2006e4; // displaced BL
            break;
        case 17:
            returning.Enter(m,r);
            break;
        case 14:
            presentation.FinishReveal(m,r[4]);
            r[0]=OwlPresentation::Byte(m,r[4]+0x53);
            break;
        case 12: case 13:
            // Only suppress Saving/Progress saved while the verified classic
            // owl confirmation is retained. Failures use the ordinary UI.
            r[14]=Sites[site][0]+4;
            if(enabled && presentation.window && presentation.context==r[0] &&
               (site==12?kind==Kind::Owl:pending_quit==r[0])) {
                presentation.Tick(m);
            } else {
                r[15]=0x1cee24; // displaced BL
            }
            break;
        }
    }
};
} // namespace MM3D::ClassicSaving
