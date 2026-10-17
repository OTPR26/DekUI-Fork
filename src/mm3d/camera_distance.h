// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/form_camera.h"

namespace MM3D::CameraDistance {
using CameraLive::U32;
inline std::atomic<float> multiplier{1.f};
constexpr U32 FirstSVC=0xfffb50;
struct Load {U32 pc,word;unsigned offset,destination,count,pointer_register;};
// NORMAL1/3 radius reads only, after native parameter construction. Keep the
// cached native parameters unchanged so repeated frames never compound scale.
constexpr std::array<Load,10> Loads{{
    {0x4483a8,0xedd41a02,8,3,1,4},{0x4483b4,0xedd40a01,4,1,1,4},
    {0x44873c,0xedd40a01,4,1,1,4},{0x448754,0xed941a02,8,2,1,4},
    {0x448b40,0xed948a01,4,16,1,4},{0x448ffc,0xed940a01,4,0,1,4},
    {0x448b70,0xecd10a02,0,1,2,1},{0x448b98,0xecd20a02,0,1,2,2},
    {0x3e44b4,0xecd20a02,0,1,2,2},{0x3e4868,0xed940a01,4,0,1,4}
}};
inline U32 Instruction(U32 pc,U32 word) {
    for(unsigned i=0;i<Loads.size();++i)if(pc==Loads[i].pc&&word==Loads[i].word)return 0xef000000|FirstSVC+i;
    return word;
}
template<class M> bool CodeMatches(M& m,U32 pc) {
    const bool normal3=pc<0x400000;
    const U32 start=normal3?0x3e3e54:0x447fe8,size=normal3?0xaa0:0x1318,expected=normal3?0x3ab807bf:0x97601e4c;
    U32 hash=2166136261u;
    for(U32 offset=0;offset<size;offset+=4) {
        auto value=m.Read(start+offset);if(!value)return false;
        for(unsigned byte=0;byte<4;++byte)hash=(hash^((*value>>(byte*8))&255))*16777619u;
    }
    return hash==expected;
}
inline float Factor(float value) {return std::isfinite(value)?std::clamp(value,.5f,3.f):1.f;}
inline U32 Scale(U32 bits,float factor) {
    factor=Factor(factor);const float native=std::bit_cast<float>(bits);
    if(factor==1.f||!std::isfinite(native)||native<10.f||native>2000.f)return bits;
    return std::bit_cast<U32>(native*factor);
}
struct Identity {U32 player,form,setting,context;};
template<class M> std::optional<Identity> Eligible(M& m,U32 cam) {
    const auto pointer=[](U32 p,U32 size){return p>=0x100000&&!(p&3)&&p<0x40000000&&size<=0x40000000-p;};
    if(!pointer(cam,0x198))return {};
    const auto ctx=m.Read(cam+0xa0);if(!ctx||!pointer(*ctx,0x20e0)||cam!=*ctx+0x408)return {};
    const auto root=m.Read(0x72f5c0);if(!root||!pointer(*root,0x170)||m.Read(*root+0x16c)!=ctx)return {};
    const auto type=m.Read(*ctx+0x13c),indices=m.Read(*ctx+0xa78),player=m.Read(*ctx+0x20dc),mode=m.Read(cam+0x158),state=m.Read(cam+0x154);
    if(!type||((*type>>8)&255)!=4||!indices||(*indices>>16)!=0||!player||!pointer(*player,0x11dbc)||
       m.Read(cam+0xa4)!=player||!mode||!state)return {};
    const U32 setting=*state>>16;
    if(!setting||setting>=0x5b||setting==9||setting==10)return {}; // scripted FREE0/DEMO0
    switch(*mode&65535) {
    case 0:case 1:case 2:case 5:case 9:case 10:case 14:case 16:case 17:case 18:case 19:case 20:case 31:break;
    default:return {}; // dialogue, first-person, items, death and cinematics
    }
    const auto id=m.Read(*player),form=m.Read(*player+0x1fc),flags=m.Read(*player+0x11db0);
    if(!id||(*id&0xffffff)!=0x20000||!form||(*form>>24)>4||!flags||(*flags&(0x80|0x800000|0x30000000)))return {};
    return Identity{*player,*form>>24,setting,*ctx};
}
struct NormalSample {U32 normal_camera=0,normal_player=0;float normal_factor=1.f;};
template<class M,class R,class F> void ApplyLoad(M& m,R& r,F& floats,unsigned hook,float configured,NormalSample* sample=nullptr) {
    if(hook>=Loads.size())return;
    const auto& load=Loads[hook];
    const U32 address=r[load.pointer_register]+load.offset;
    const auto identity=Eligible(m,r[4]);bool eligible=identity.has_value();
    if(load.count==2&&address!=r[4]+4)eligible=false;
    if(eligible&&sample) {
        sample->normal_camera=r[4];sample->normal_player=identity->player;sample->normal_factor=Factor(configured);
    }
    for(unsigned i=0;i<load.count;++i) {
        const auto value=m.Read(address+4*i);
        floats[load.destination+i]=value?(eligible?Scale(*value,configured):*value):0;
    }
}
// FREECAMERA retains the current orbit rather than continually applying the
// normal camera parameter table. Capture its unscaled requested radius once.
// If NORMAL already applied a factor, remove that factor at handover. Native
// spherical reconstruction and wall collision still follow this adjustment.
struct Orbit : NormalSample {
    U32 camera=0,player=0,setting=0,form=0;float baseline=0;bool applied=false;
    void Observe(U32 cam,U32 requested) {
        if(requested!=31&&camera==cam){camera=player=setting=form=0;baseline=0;applied=false;}
        if(requested!=31&&normal_camera==cam){normal_camera=normal_player=0;normal_factor=1.f;}
    }
    template<class M,class R> void Apply(M& m,R& r,FormCamera::Hook& native,float configured) {
        const U32 cam=r[4];const auto identity=Eligible(m,cam);const auto mode=m.Read(cam+0x158);
        if(!identity||!mode||(*mode&65535)!=31){Observe(cam,0);return;}
        if(camera!=cam||player!=identity->player||setting!=identity->setting||form!=identity->form) {
            camera=cam;player=identity->player;setting=identity->setting;form=identity->form;baseline=0;applied=false;
        }
        const float factor=Factor(configured);
        if(factor==1.f&&!applied)return; // exact default behavior
        const auto old=m.Read(r[13]);if(!old)return;
        const float radius=std::bit_cast<float>(*old);
        if(!std::isfinite(radius)||radius<10||radius>2000)return;
        if(baseline==0) {
            const float prior=normal_camera==cam&&normal_player==player?normal_factor:1.f;
            baseline=radius/prior;
        }
        m.Write(r[13],std::bit_cast<U32>(baseline*factor));
        // The existing form-camera latch also retains the unscaled radius,
        // so both toggling back to 1x and subsequent frames restore correctly.
        if(native.saved&&native.camera==cam&&native.player==player)native.radius=baseline;
        applied=factor!=1.f;
    }
};
}
