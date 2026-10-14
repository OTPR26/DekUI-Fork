// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/notebook_prompt.h"
#include "mm3d/classic_saving.h"
#include <string_view>
namespace MM3D::ClassicSaving {
struct OwlDraw {
    using U32=std::uint32_t;
    NotebookPrompt::Hook scratch;
    std::vector<std::vector<NativeInventory::Hook::Write>> frames;
    template<class M> U32 Root(M& m,U32 layout,U32 root,U32 sp,U32 window,unsigned tick) {
        frames.emplace_back();auto& edits=frames.back();
        scratch.frames.emplace_back();auto& frame=scratch.frames.back();
        if(!enabled||!window||(layout!=window&&m.Read(window+0x358)!=layout))return root;
        owl_draw_status.fetch_or(1);
        NativeInventory::Hook helper;std::vector<NativeInventory::Hook::Write> unused;
        std::vector<U32> nodes;
        if(!helper.Transform(m,root,unused,nodes,{1,0,0,1,0,0}))return root;
        auto stage=[&](U32 at,U32 value){auto old=m.Read(at);if(old)edits.push_back({at,*old,value});};
        const auto arrow=m.Read(window+0x318);
        U32 plate=0;
        for(U32 node:nodes) {
            if(arrow&&node==*arrow) {
                owl_draw_status.fetch_or(4);
                auto flags=m.Read(node+0xd4);if(flags)stage(node+0xd4,*flags&~1u);
                stage(node+0x134,0); // computed alpha, after native animation
            }
            auto geo=m.Read(node+0x28);if(!geo||!NativeInventory::Pointer(*geo,0x30c))continue;
            auto type=m.Read(*geo+4);if(!type)continue;
            if((*type&65535)==6 && !plate)plate=node;
            if((*type&65535)!=3)continue;
            auto font=m.Read(*geo+0x28),glyphs=m.Read(*geo+0x2fc),count=m.Read(*geo+0x300),visible=m.Read(*geo+0x308);
            if(!font||!glyphs||!count||!visible)continue;
            *count=std::min(*count,*visible);
            if(*count<4||*count>256)continue;
            // Match Unicode glyphs, not UTF-8 bytes (Spanish Sí has two glyphs).
            // Keep positions and glyph counts intact while flashing/hiding choices.
            std::u32string_view labels;
            unsigned yes_count=0;
            for(auto candidate:{std::u32string_view(U"YesNo"),std::u32string_view(U"OuiNon"),
                                std::u32string_view(U"SíNo")}) {
                if(*count<candidate.size())continue;
                bool match=true;
                for(unsigned i=0;i<candidate.size();++i) {
                    auto expected=NotebookFont::Lookup(m,*font,candidate[i],0);
                    if(!expected){match=false;break;}
                    const U32 at=*glyphs+(*count-candidate.size()+i)*60;
                    for(unsigned word:{0u,10u,11u,12u,13u})
                        if(m.Read(at+4*word)!=expected->words[word])match=false;
                }
                if(match){labels=candidate;yes_count=candidate==U"SíNo"?2:3;break;}
            }
            if(labels.empty())continue;
            owl_draw_status.fetch_or(2);
            // Color indices address float RGBA entries at geometry+0x78.
            // Reserve an unused entry, leaving the question's red words intact.
            std::array<bool,15> used{};
            for(U32 i=0;i<*count;++i) {
                auto color=m.Read(*glyphs+i*60+56);
                if(color&&*color<used.size())used[*color]=true;
            }
            int slot=14;while(slot>=0&&used[slot])--slot;
            if(slot>=0) {
                const float brightness=.70f+.30f*std::cos(float(tick%36)*6.2831853f/36.f);
                const auto original_color=m.Read(*glyphs+(*count-labels.size())*60+56);
                std::array<float,4> rgba{1,1,1,1};
                bool valid=original_color.has_value();
                if(original_color&&*original_color!=0xffffffffu) {
                    valid=*original_color<15;
                    for(unsigned c=0;c<4&&valid;++c) {
                        auto value=m.Read(*geo+0x78+*original_color*16+c*4);
                        if(!value){valid=false;break;}
                        rgba[c]=std::bit_cast<float>(*value);
                        valid=std::isfinite(rgba[c]);
                    }
                }
                // Dim the original choice color, preserving its hue and alpha.
                if(valid)for(unsigned c=0;c<4;++c)
                    stage(*geo+0x78+slot*16+c*4,
                          std::bit_cast<U32>(rgba[c]*(c==3?1.f:brightness)));
                else slot=-1;
            }
            for(unsigned i=0;i<labels.size();++i) {
                const U32 glyph=*glyphs+(*count-labels.size()+i)*60;
                if(i>=yes_count)stage(glyph+32,0); // No: zero width, no reflow
                else if(slot>=0)stage(glyph+56,U32(slot));
            }
        }
        for(auto e:edits)m.Write(e.address,e.after);
        const float alpha=exit_world_alpha.load();
        if(layout!=window || !plate || alpha<=0)return root;
        constexpr U32 Size=0x800,Node=0x180,Geo=0x2c0;
        if(sp<0x102000||(sp&7))return root;
        const U32 base=sp-Size;
        for(U32 at=base-Size;at<sp;at+=4)if(!m.Read(at))return root;
        std::array<U32,Size/4> words{};
        auto put=[&](U32 at,U32 v){words[at/4]=v;};
        auto number=[&](U32 at,float v){put(at,std::bit_cast<U32>(v));};
        auto copy=[&](U32 at,U32 from,U32 size){for(U32 n=0;n<size;n+=4){auto v=m.Read(from+n);if(!v)return false;put(at+n,*v);}return true;};
        if(!copy(0,root,0x140)||!copy(Node,plate,0x140))return root;
        auto geometry=m.Read(plate+0x28);if(!geometry||!copy(Geo,*geometry,0x7c))return root;
        put(0x14,base+0x140);put(0x18,2);put(0x20,0);put(0x24,0xffff);put(0x28,0);
        // Native traversal visits children from last to first.
        put(0x140,root);put(0x144,base+Node);
        put(Node+0x14,0);put(Node+0x18,0);put(Node+0x20,0);put(Node+0x24,0xffff);
        put(Node+0x28,base+Geo);put(Node+0xd4,15);
        put(Node+0xdc,words[(Node+0xdc)/4]&~0xff00u);
        for(unsigned i=0;i<8;++i)number(Node+0xe0+i*4,(i==0||i==5)?1.f:0.f);
        for(unsigned i=0;i<3;++i)number(Node+0x128+i*4,0);
        number(Node+0x134,std::clamp(alpha,0.f,1.f));
        // Keep the type-6 sprite/vtable, but remove its optional texture.
        // The native emitter then selects its untextured color shader.
        put(Geo+0x10,0);put(Geo+0xc,0x00000100);put(Geo+0x38,0);
        number(Geo+0x14,-2048);number(Geo+0x18,-2048);number(Geo+0x1c,0);
        number(Geo+0x20,4096);number(Geo+0x24,4096);
        for(unsigned c=0;c<16;++c)number(Geo+0x3c+c*4,1.f);
        for(U32 i=0;i<words.size();++i)frame.writes.push_back({base+4*i,*m.Read(base+4*i),words[i]});
        for(auto w:frame.writes)m.Write(w.address,w.after);
        frame.sp=sp;
        owl_draw_status.fetch_or(8);
        if(alpha>=1)exit_world_black_drawn=true;
        return base;
    }
    template<class M> U32 End(M& m) {
        const auto sp=scratch.End(m);
        if(!frames.empty()) {
            for(auto it=frames.back().rbegin();it!=frames.back().rend();++it)m.Write(it->address,it->before);
            frames.pop_back();
        }
        return sp;
    }
};
} // namespace MM3D::ClassicSaving
