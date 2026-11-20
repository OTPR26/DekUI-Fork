// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../dekui/host_interfaces.h"
#include "../dekui/native_inventory.h"
#include "oot3d_ui/ui_primitives.h"
#include "oot3d_ui/ui_contract_types.h"
#include <cstdio>
#include <string>

namespace Oot3dNativeGame {
// EUR v1.1 final widget geometry, observed at ButtonEmission after all native
// visibility and touch transforms. Own values immediately; never retain a
// temporary widget/glyph-record address for a later host frame.
template<class M> std::vector<oot3d::ui::UiPrimitive> CaptureMm3dTouchArtwork(M& m,uint32_t node) {
    using namespace Mm3dDekui::NativeInventory;
    using namespace oot3d::ui;
    std::vector<UiPrimitive> out;
    const auto f=[&](uint32_t at)->std::optional<float>{auto bits=m.Read(at);if(!bits)return {};
        float v=std::bit_cast<float>(*bits);return std::isfinite(v)?std::optional(v):std::nullopt;};
    auto geo=m.Read(node+0x28),flags=m.Read(node+0xd4);
    auto alpha=f(node+0x134),xx=f(node+0xe0),xy=f(node+0xe4),yx=f(node+0xf0),yy=f(node+0xf4),tx=f(node+0xec),ty=f(node+0xfc);
    if(!geo||!*geo||!flags||!(*flags&1)||!alpha||*alpha<=0||!xx||!xy||!yx||!yy||!tx||!ty)return out;
    auto kind=m.Read(*geo+4);if(!kind)return out;
    const auto add=[&](uint32_t surface,uint32_t rawFormat,uint32_t rawType,uint32_t width,uint32_t height,
                       float x,float y,float w,float h,float u,float v,float uw,float vh,uint32_t tint,bool mutableImage=false){
        if(!surface||!width||!height||width>4096||height>4096||(width&(width-1))||(height&(height-1))||
           width<8||height<8||!(w>0&&h>0)||!std::isfinite(x+y+w+h+u+v+uw+vh))return;
        char semantic[100];std::snprintf(semantic,sizeof(semantic),"mm3d/native/touch/%u/%u/%u/%u",width,height,rawFormat,rawType);
        UiPrimitive p;p.subsystem=UiSubsystem::TouchControls;p.role=UiPrimitiveRole::TouchControl;
        p.owner_address=node;p.texture={surface,surface,semantic};
        if(mutableImage&&(rawFormat==0x6756||rawFormat==0x1906||rawFormat==0x6758||rawFormat==0x190a)){
            const unsigned bits=rawType==0x6761?4:(rawFormat==0x6758||rawFormat==0x190a)&&rawType==0x1401?16:8;
            const size_t bytes=size_t(width)*height*bits/8;if(bytes>1024*1024)return;
            uint64_t hash=1469598103934665603ULL;
            for(size_t i=0;i<bytes;i+=4){auto word=m.Read(surface+uint32_t(i));if(!word)return;hash=(hash^*word)*1099511628211ULL;}
            char suffix[20];std::snprintf(suffix,sizeof(suffix),"#%016llx",(unsigned long long)hash);p.texture.semantic_name+=suffix;
        }
        p.color={f(node+0x128).value_or(1.f),f(node+0x12c).value_or(1.f),f(node+0x130).value_or(1.f),std::clamp(*alpha,0.f,1.f)};
        p.color.red*=float((tint>>0)&255)/255;p.color.green*=float((tint>>8)&255)/255;
        p.color.blue*=float((tint>>16)&255)/255;p.color.alpha*=float((tint>>24)&255)/255;
        std::array<UiVertex,4> corners{};
        const float px[]{x,x+w,x,x+w},py[]{y,y,y+h,y+h},pu[]{u,u+uw,u,u+uw},pv[]{v,v,v+vh,v+vh};
        // Pane animation/equipped-item selection lives in the native UV
        // affine, independently of the shared geometry and texture atlas.
        const float ux=f(node+0x110).value_or(1.f),uy=f(node+0x114).value_or(0.f),ut=f(node+0x118).value_or(0.f);
        const float vx=f(node+0x11c).value_or(0.f),vy=f(node+0x120).value_or(1.f),vt=f(node+0x124).value_or(0.f);
        float left=INFINITY,top=INFINITY,right=-INFINITY,bottom=-INFINITY;
        for(unsigned i=0;i<4;++i){const float ax=*xx*px[i]+*xy*py[i]+*tx+40.f/3.f,ay=*yx*px[i]+*yy*py[i]+*ty;
            corners[i]={ax,ay,ux*pu[i]+uy*pv[i]+ut,vx*pu[i]+vy*pv[i]+vt};left=std::min(left,ax);right=std::max(right,ax);top=std::min(top,ay);bottom=std::max(bottom,ay);}
        if(!std::isfinite(left+right+top+bottom)||right<=left||bottom<=top)return;
        p.destination={left,top,right-left,bottom-top};p.uv={u,v,uw,vh};p.corners=corners;
        out.push_back(std::move(p));
    };
    if((*kind&65535)==6){
        auto texture=m.Read(*geo+0x10);auto descriptor=texture?m.Read(*texture+0x4c):std::optional<uint32_t>{};
        if(!descriptor||!Pointer(*descriptor,0x24))return out;
        auto format=m.Read(*descriptor+8),type=m.Read(*descriptor+12),size=m.Read(*descriptor+16),surface=m.Read(*descriptor+32);
        auto x=f(*geo+0x14),y=f(*geo+0x18),w=f(*geo+0x20),h=f(*geo+0x24),u=f(*geo+0x28),v=f(*geo+0x2c),uw=f(*geo+0x30),vh=f(*geo+0x34);
        if(format&&type&&size&&surface&&x&&y&&w&&h&&u&&v&&uw&&vh)
            add(*surface,*format,*type,*size&65535,*size>>16,*x,*y,*w,*h,*u,*v,*uw,*vh,~0u,true);
    }else if((*kind&65535)==3){
        auto records=m.Read(*geo+0x2fc),count=m.Read(*geo+0x308);
        if(!records||!count||!*count||*count>128||!Pointer(*records,*count*60))return out;
        for(unsigned i=0;i<*count;++i){const uint32_t at=*records+i*60;
            std::array<uint32_t,15> words{};bool valid=true;for(unsigned j=0;j<15;++j){auto b=m.Read(at+j*4);if(!b){valid=false;break;}words[j]=*b;}
            if(!valid){out.clear();return out;}
            const auto v=[&](unsigned j){return std::bit_cast<float>(words[j]);};
            add(words[0],words[1],words[2],words[3],words[4],v(5),v(6),v(8),v(9),v(10),v(11),v(12),v(13),words[14]);
        }
    }
    return out;
}
}
