// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <bit>
#include <optional>
#include <vector>
#include "mm3d/classic_saving.h"
#include "mm3d/single_screen_text_data.h"
#include "mm3d/classic_owl_text_data.h"
namespace MM3D::ClassicSaving {
// Exact USA 1.1 native message objects. No save-file data is changed here.
struct OwlPresentation {
    using U32=std::uint32_t;
    U32 window=0,context=0,choice_engine=0;
    unsigned frames=0;
    struct Label { U32 ptr=0; std::vector<unsigned char> bytes; } yes,no;
    static bool Pointer(U32 p) {return p>=0x100000 && p<0x40000000;}
    template<class M> static unsigned Byte(M& m,U32 p) {
        return (m.Read(p&~3u).value_or(0)>>((p&3)*8))&255;
    }
    template<class M> static void Byte(M& m,U32 p,unsigned value) {
        auto w=m.Read(p&~3u);if(!w)return;
        unsigned shift=(p&3)*8;
        m.Write(p&~3u,(*w&~(255u<<shift))|((value&255)<<shift));
    }
    template<class M> static U32 FinalWindow(M& m) {
        auto p=m.Read(0x6b3724); // MessageMgr::Instance().message_window
        if(!enabled||!p||!Pointer(*p)||m.Read(*p+0x3f4)!=0xc01||
           m.Read(*p+0x404)!=SingleScreenText::ClassicOwlPageCount-1||
           m.Read(*p+0x408)!=SingleScreenText::ClassicOwlPageCount)return 0;
        return *p;
    }
    template<class M> void Reveal(M& m,U32 engine) {
        const U32 w=FinalWindow(m);
        if(!w || engine!=w+0x368)return;
        m.Write(engine+0x10,std::bit_cast<U32>(0.90f));
    }
    template<class M> void FinishReveal(M& m,U32 engine) {
        // Run at the native end-of-page decision, not the next update: native
        // code can consume the last character AND close in the same update.
        const U32 w=FinalWindow(m);
        if(w && engine==w+0x368 && choice_engine==engine) {
            Byte(m,engine+0x53,1);choice_engine=0;
        }
    }
    template<class M> bool Choice(M& m,U32 engine) {
        const U32 w=FinalWindow(m);
        if(!w || engine!=w+0x368)return false;
        Byte(m,engine+0x53,0);Byte(m,engine+0x58,0);
        choice_engine=engine;return true;
    }
    template<class M> static void LabelText(M& m,const Label& label,bool visible) {
        for(unsigned i=0;i<label.bytes.size();++i)
            Byte(m,label.ptr+i,visible?label.bytes[i]:' ');
    }
    template<class M> bool Confirm(M& m,U32 ctx) {
        const U32 w=FinalWindow(m);
        if(!w||m.Read(w+0x444)!=ctx||m.Read(w+0x430)!=0)return false;
        // The native parser owns copied text spans. Keep their length/layout
        // unchanged; only the two exact final-page labels may be hidden.
        // MessageSet + 0x28/0x2c (MessageMgr + 0x38).
        auto list=m.Read(0x6b3758),count=m.Read(0x6b375c);
        if(!list||!count||!Pointer(*list)||*count>256)return false;
        Label y,n;
        for(U32 i=0;i<*count;++i) {
            auto obj=m.Read(*list+4*i);if(!obj||!Pointer(*obj)||m.Read(*obj)!=0x63d4fc)continue;
            auto ptr=m.Read(*obj+8),size=m.Read(*obj+12);
            if(!ptr||!size||!Pointer(*ptr)||(*size!=2&&*size!=3))continue;
            Label l{*ptr,{}};for(U32 j=0;j<*size;++j)l.bytes.push_back(Byte(m,*ptr+j));
            if(l.bytes==std::vector<unsigned char>{'Y','e','s'}||
               l.bytes==std::vector<unsigned char>{'O','u','i'}||
               l.bytes==std::vector<unsigned char>{'S',0xc3,0xad})y=l;
            if(l.bytes==std::vector<unsigned char>{'N','o'}||
               l.bytes==std::vector<unsigned char>{'N','o','n'})n=l;
        }
        if(!y.ptr||!n.ptr)return false;
        yes=y;no=n;window=w;context=ctx;frames=0;choice_engine=0;
        owl_draw_status=0;exit_world_black_drawn=false;
        return true;
    }
    template<class M> void Tick(M& m) {
        if(!window)return;
        ++frames;
        // Keep the fully revealed choice page drawable. State zero means
        // inactive, not "visible but frozen". Freeze updates at the reveal
        // hook instead, leaving the native completed-page states intact.
        m.Write(window+0x3dc,9);Byte(m,window+0x3b8,5);
        m.Write(window+0x430,0);
        // Choice visibility is scoped to the cached glyph draw, not the
        // already-parsed source strings (see classic_owl_draw.h).
        auto arrow=m.Read(window+0x318);
        if(arrow&&Pointer(*arrow)) {
            auto flags=m.Read(*arrow+0x84),dirty=m.Read(*arrow+0x88);
            if(flags&&dirty){m.Write(*arrow+0x84,(*flags&~1u)|0x20000u);m.Write(*arrow+0x88,*dirty&~0x20000u);}
        }
    }
    template<class M> void Reset(M& m,bool cancel_fade=true) {
        if(window){
            LabelText(m,yes,true);LabelText(m,no,true);
            if(cancel_fade) {
                exit_fade=ExitFade::Idle;exit_world_alpha=0.f;
                exit_world_black_drawn=false;exit_black_rendered=false;
            }
        }
        window=context=choice_engine=frames=0;yes={};no={};
    }
};
} // namespace MM3D::ClassicSaving
