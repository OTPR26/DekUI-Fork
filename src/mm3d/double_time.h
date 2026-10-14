// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/song_list.h"

namespace MM3D::DoubleTime {
using namespace NativeInventory;
enum Part : unsigned { Background, Prompt, Timeline, Instructions, Buttons, Count };
struct Options {
    bool enabled=true,current_day_only=true;
    std::array<GearMenu::Pose,Count> poses;
    Options(){for(auto& p:poses)p={40,0,1};}
};
inline std::atomic<bool> enabled{true},current_day_only{true};
inline std::array<GearMenu::AtomicPose,Count> poses;
inline void Apply(const Options& o){
    enabled=o.enabled;current_day_only=o.current_day_only;
    for(unsigned i=0;i<Count;++i){poses[i].x=o.poses[i].x;poses[i].y=o.poses[i].y;poses[i].scale=o.poses[i].scale;}
}
struct Hook {
    SongList::Hook helper;
    using Write=NativeInventory::Hook::Write;
    U32 panel=0,background=0,pass=~0u;
    NotebookPrompt::Hook shade_scratch;
    float shade_alpha=0;
    std::array<GearMenu::Pose,Count> frame;
    std::vector<std::vector<Write>> frames;
    // Follow the same embedded-layout/group references as the native tree walk.
    template<class M> U32 EmbeddedRoot(M& m,U32 node){
        auto embedded=m.Read(node+0x20),group=m.Read(node+0x24);
        if(!embedded||!group)return 0;
        if(*embedded)return Pointer(*embedded,0x148)?*embedded+8:0;
        if((*group&0xffff)==0xffff)return 0;
        auto owner=m.Read(node+4);if(!owner||!Pointer(*owner,0x14c))return 0;
        auto count=m.Read(*owner+0x148),list=m.Read(*owner+0x144);
        if(!count||!list||*count>1024||(*group&0xffff)>=*count||!Pointer(*list,*count*4))return 0;
        auto target=m.Read(*list+4*(*group&0xffff));return target&&Pointer(*target,0x144)?*target+4:0;
    }
    template<class M> bool Prepare(M& m,std::vector<Write>& w){
        auto all=helper.Child(m,panel+8,"all");if(!all)return false;
        const std::array<std::pair<const char*,Part>,9> groups{{
            {"message_g",Prompt},{"link_icon_g",Timeline},{"disp_time_g",Timeline},
            {"sche_base_g",Timeline},{"popup_window00_g",Instructions},
            {"btn_advance_g",Buttons},{"btn_quit_g",Buttons},
            {"touch_rect_center_l",Timeline},{"touch_rect_link_l",Timeline}}};
        for(auto [name,part]:groups){
            auto node=helper.Child(m,all,name);if(!node)return false;
            auto p=frame[part];std::vector<U32> seen;
            if(!helper.helper.Transform(m,node,w,seen,{p.scale,0,0,p.scale,p.x,p.y}))return false;
        }
        if(current_day_only){
            // Original5F4C68 stores the opening/current day (0..2) at220.
            // Native day animation already positions that day's card and markers.
            auto day=m.Read(panel+0x220);if(!day||*day>2)return false;
            auto base=helper.Child(m,all,"sche_base_g");
            auto link=helper.Child(m,base,"sche_base_l");if(!link)return false;
            auto root=EmbeddedRoot(m,link);if(!root)return false;
            auto days=helper.Child(m,root,"all");if(!days)return false;
            constexpr const char* names[]{"sche_base0","sche_base1","sche_base2"};
            for(unsigned i=0;i<3;++i){
                auto node=helper.Child(m,days,names[i]);if(!node)return false;
                if(i!=*day&&!helper.Hide(m,node,w))return false;
            }
        }
        return true;
    }
    template<class M> float ShadeAlpha(M& m) {
        float progress=1.f;
        auto players=m.Read(panel+0x14c),count=m.Read(panel+0x150);
        if(players&&count&&*count<=64&&Pointer(*players,*count*4)) {
            for(unsigned i=0;i<*count;++i) {
                auto player=m.Read(*players+4*i);if(!player)continue;
                auto animation=m.Read(*player+0xc),frame_at=m.Read(*player+0x10);
                if(!animation||!frame_at)continue;
                auto name=m.Read(*animation+8);if(!name)continue;
                const bool opening=DialogueScale::Name(m,*name,"in_menu_lap_time_song_anim");
                const bool closing=DialogueScale::Name(m,*name,"out_menu_lap_time_song_anim");
                if(!opening&&!closing)continue;
                const float f=std::bit_cast<float>(*frame_at);
                if(!std::isfinite(f)||f<0)return 0;
                // Four native animation frames (~0.13 s at 30 Hz).
                progress=std::clamp(f/4.f,0.f,1.f);
                if(closing)progress=1.f-progress;
                break;
            }
        }
        return .30f*progress;
    }
    template<class M> void Pass(M& m,U32 manager,U32 screen){
        pass=screen;if(screen!=4)return;
        panel=background=0;shade_alpha=0;if(!enabled)return;
        NativeInventory::Hook::Buckets from,to,bf,bt;
        auto bottom=m.Read(manager+28),top=m.Read(manager+20),bgfrom=m.Read(manager+24),bgto=m.Read(manager+12);
        if(!bottom||!top||!bgfrom||!bgto||*bottom==*top||*bgfrom==*bgto||
           !helper.helper.ReadQueue(m,*bottom,from)||!helper.helper.ReadQueue(m,*top,to)||
           !helper.helper.ReadQueue(m,*bgfrom,bf)||!helper.helper.ReadQueue(m,*bgto,bt))return;
        for(auto& bucket:from)for(U32 p:bucket)if(LayoutNamed(m,p,"LapTimeSong")){if(panel){panel=0;return;}panel=p;}
        if(!panel)return;
        for(auto& bucket:bf)for(U32 p:bucket)if(LayoutNamed(m,p,"CommonBottomBg")){if(background){panel=0;return;}background=p;}
        for(unsigned i=0;i<Count;++i){frame[i]={poses[i].x.load(),poses[i].y.load(),poses[i].scale.load()};auto p=frame[i];
            if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.scale)||p.scale<.1f||p.scale>3){panel=0;return;}}
        std::vector<Write> preview;
        if(!Prepare(m,preview)||to[15].size()>=32||(background&&bt[0].size()>=32)){panel=0;return;}
        for(auto& bucket:to)if(std::find(bucket.begin(),bucket.end(),panel)!=bucket.end()){panel=0;return;}
        to[15].push_back(panel);for(auto& bucket:from)std::erase(bucket,panel);
        if(background){for(auto& bucket:bf)std::erase(bucket,background);bt[0].insert(bt[0].begin(),background);}
        const std::array queues{std::pair{*bottom,&from},std::pair{*top,&to},std::pair{*bgfrom,&bf},std::pair{*bgto,&bt}};
        for(auto [q,buckets]:queues)for(unsigned p=0;p<16;++p)for(unsigned i=0;i<(*buckets)[p].size();++i)
            if(!m.Read(q+4+p*0x80+i*4)){panel=0;return;}
        shade_alpha=ShadeAlpha(m);
        for(auto [q,buckets]:queues){unsigned total=0;for(unsigned p=0;p<16;++p){auto& bucket=(*buckets)[p];
            for(unsigned i=0;i<bucket.size();++i)m.Write(q+4+p*0x80+i*4,bucket[i]);
            m.Write(q+0x804+p*4,bucket.size());total+=bucket.size();}m.Write(q+0x844,total);}
    }
    template<class M> void Begin(M& m,U32 layout){
        frames.emplace_back();if(!panel)return;std::vector<Write> w;
        if(layout==panel&&pass==3){if(!Prepare(m,w))return;}
        else if(layout==background&&pass==1){auto p=frame[Background];std::vector<U32> seen;
            if(!helper.helper.Transform(m,layout+8,w,seen,{p.scale,0,0,p.scale,p.x,p.y}))return;}
        else return;
        for(auto write:w)m.Write(write.address,write.after);frames.back()=std::move(w);
    }
    // Emit one untextured black pane in background pass 1, before all top UI.
    // Clone draw data in temporary guest stack space; restore it after emission.
    template<class M> U32 ShadeRoot(M& m,U32 layout,U32 root,U32 sp) {
        shade_scratch.frames.emplace_back();auto& frame=shade_scratch.frames.back();
        const float alpha=shade_alpha;
        if(!enabled||!panel||!background||layout!=background||pass!=1||alpha<=0)return root;
        std::vector<Write> unused;std::vector<U32> nodes;
        if(!helper.helper.Transform(m,layout+8,unused,nodes,{1,0,0,1,0,0}))return root;
        U32 plate=0;
        for(U32 node:nodes) {
            auto geometry=m.Read(node+0x28);if(!geometry)continue;
            auto type=m.Read(*geometry+4);
            if(type&&(*type&65535)==6){plate=node;break;}
        }
        if(!plate)return root;
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
        return base;
    }
    template<class M> void End(M& m){if(frames.empty())return;
        for(auto i=frames.back().rbegin();i!=frames.back().rend();++i)m.Write(i->address,i->before);frames.pop_back();}
};
}
