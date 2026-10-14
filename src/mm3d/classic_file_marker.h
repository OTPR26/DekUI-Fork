// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/classic_saving.h"
#include "mm3d/native_file_select.h"
#include "mm3d/ocarina_prompt.h"
namespace MM3D::ClassicSaving {
struct FileMarker {
    using U32=std::uint32_t;
    NotebookPrompt::Hook scratch;
    // Read the detailed card's own file-number animation. Selection transition
    // players on FileSelect can remain bound at frame zero after completion;
    // their presence does not identify which file this card is displaying.
    template<class M> unsigned DetailSlot(M& m,U32 layout) {
        auto players=m.Read(layout+0x14c),count=m.Read(layout+0x150);
        if(!players||!count||*count>64||!NativeInventory::Pointer(*players,*count*4))return 3;
        for(unsigned i=0;i<*count;++i) {
            auto player=m.Read(*players+i*4);if(!player)continue;
            auto anim=m.Read(*player+0xc),time=m.Read(*player+0x10);
            if(!anim||!time)continue;
            auto name=m.Read(*anim+8);
            if(!name||!DialogueScale::Name(m,*name,"switch_file_num_ani"))continue;
            const float frame=std::bit_cast<float>(*time);
            if(std::isfinite(frame)&&frame>=0&&frame<=2&&frame==std::floor(frame))return unsigned(frame);
        }
        return 3;
    }
    // Native Hud.icon_sakasa_uta_pane: main resource15, (368,240,16,16).
    template<class M> U32 Root(M& m,const NativeFileSelect::Hook& files,U32 layout,
                               U32 content,U32 sp,unsigned mask,std::uint64_t time_us) {
        scratch.frames.emplace_back();auto& frame=scratch.frames.back();
        if(!enabled||!mask||!files.active)return content;
        const bool cards=layout==files.panel&&files.pass==3;
        const bool detail=layout==files.summary&&files.pass<4;
        if(!cards&&!detail)return content;
        OcarinaPrompt::Hook resources;const U32 texture=resources.Parts(m,15);
        if(!texture)return content;
        NativeInventory::Hook helper;
        struct Entry {U32 name,icon,geo;float x,y;};
        std::vector<Entry> entries;
        auto collect=[&](U32 card) {
            std::vector<NativeInventory::Hook::Write> unused;std::vector<U32> nodes;
            if(!helper.Transform(m,card+8,unused,nodes,{1,0,0,1,0,0}))return;
            U32 name=0,icon=0,geo=0;
            for(U32 n:nodes) {
                auto g=m.Read(n+0x28);if(!g||!*g)continue;
                auto type=m.Read(*g+4);if(!type)continue;
                if((*type&65535)==3&&NativeInventory::Named(m,n,"name"))name=n;
                if((*type&65535)==6&&!icon){icon=n;geo=*g;}
            }
            if(!name||!icon)return;
            auto g=m.Read(name+0x28),flags=m.Read(name+0xd4),alpha=m.Read(name+0x134);
            if(!g||!flags||!(*flags&1)||!alpha||std::bit_cast<float>(*alpha)<=0)return;
            auto glyphs=m.Read(*g+0x2fc),count=m.Read(*g+0x300),visible=m.Read(*g+0x308);
            if(!glyphs||!count||!visible)return;
            const U32 n=std::min(*count,*visible);if(!n||n>16)return;
            float right=-1e6f,top=1e6f;
            for(U32 i=0;i<n;++i) {
                auto x=m.Read(*glyphs+i*60+20),y=m.Read(*glyphs+i*60+24),w=m.Read(*glyphs+i*60+32);
                if(!x||!y||!w)return;
                float px=std::bit_cast<float>(*x),py=std::bit_cast<float>(*y),width=std::bit_cast<float>(*w);
                if(!std::isfinite(px)||!std::isfinite(py)||!std::isfinite(width))return;
                right=std::max(right,px+width);top=std::min(top,py);
            }
            // The atlas flag has transparent padding above its visible artwork.
            entries.push_back({name,icon,geo,right+4,top-3.f});
        };
        if(cards) {
            auto list=m.Read(layout+0x154),count=m.Read(layout+0x158);
            if(!list||!count||*count>512)return content;
            constexpr const char* names[]={"plate_x_n","plate_y_n","plate_z_n"};
            for(U32 i=0;i<*count;++i) {
                auto node=m.Read(*list+i*4);if(!node)continue;
                if(NativeInventory::Named(m,*node,"null_plate_g")) {
                    auto flags=m.Read(*node+0xd4);if(!flags||!(*flags&1))return content;
                }
                for(unsigned slot=0;slot<3;++slot)if((mask&(1u<<slot))&&NativeInventory::Named(m,*node,names[slot])) {
                    auto card=m.Read(*node+0x20);
                    if(card&&NativeInventory::LayoutNamed(m,*card,"FilePlate"))collect(*card);
                }
            }
        } else {
            const unsigned slot=DetailSlot(m,layout);
            if(slot<3&&(mask&(1u<<slot)))collect(layout);
        }
        if(entries.empty()||entries.size()>3)return content;
        constexpr U32 Size=0x1000;
        if(sp<0x104000||(sp&7))return content;
        const U32 base=sp-Size;
        for(U32 at=base-Size;at<sp;at+=4)if(!m.Read(at))return content;
        std::array<U32,Size/4> words{};
        auto put=[&](U32 at,U32 v){words[at/4]=v;};
        auto f=[&](U32 at,float v){put(at,std::bit_cast<U32>(v));};
        auto copy=[&](U32 at,U32 from,U32 size){for(U32 i=0;i<size;i+=4){auto v=m.Read(from+i);if(!v)return false;put(at+i,*v);}return true;};
        if(!copy(0,content,0x140))return content;
        put(0x14,base+0x140);put(0x18,entries.size()*2+1);put(0x20,0);put(0x24,0xffff);put(0x28,0);
        put(0x140+entries.size()*8,content);
        const float pulse=.5f+.5f*std::cos(float(time_us%1200000)*6.2831853f/1200000.f);
        for(unsigned i=0;i<entries.size();++i)for(unsigned layer=0;layer<2;++layer) {
            const bool glow=layer==1;
            const auto e=entries[i];const U32 node=0x180+(i*2+layer)*0x200,geo=node+0x140;
            if(!copy(node,e.name,0x140)||!copy(geo,e.geo,0x7c))return content;
            // Reverse native traversal: steady flag first, glow over it.
            put(0x140+(i*2+(glow?0:1))*4,base+node);put(node+0x28,base+geo);
            put(node+0x14,0);put(node+0x18,0);put(node+0x20,0);put(node+0x24,0xffff);
            put(node+0xdc,words[(node+0xdc)/4]&~0xff00u);
            for(unsigned c=0;c<6;++c)f(node+0x110+c*4,(c==0||c==4)?1.f:0.f);
            // Neutral modulation preserves the original atlas colors.
            for(unsigned c=0;c<3;++c)f(node+0x128+c*4,1.f);
            if(glow)f(node+0x134,std::bit_cast<float>(words[(node+0x134)/4])*pulse);
            put(geo+0xc,0x00010101);put(geo+0x10,texture);
            f(geo+0x14,e.x-(glow?1.f:0.f));f(geo+0x18,e.y);f(geo+0x1c,0);
            f(geo+0x20,glow?18.f:16.f);f(geo+0x24,16);
            f(geo+0x28,(glow?344.f:368.f)/512);f(geo+0x2c,240.f/256);
            f(geo+0x30,(glow?18.f:16.f)/512);f(geo+0x34,16.f/256);put(geo+0x38,0);
            for(unsigned c=0;c<16;++c)f(geo+0x3c+c*4,1);
        }
        for(U32 i=0;i<words.size();++i)frame.writes.push_back({base+4*i,*m.Read(base+4*i),words[i]});
        for(auto w:frame.writes)m.Write(w.address,w.after);frame.sp=sp;return base;
    }
};
}
