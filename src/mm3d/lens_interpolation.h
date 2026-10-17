// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/inventory_interpolation.h"

namespace MM3D::LensInterpolation {
using InventoryInterpolation::Values;
using InventoryInterpolation::Tev;
using InventoryInterpolation::Pose;
using Key=std::array<std::uint32_t,3>;
struct Material {
    std::array<unsigned,7> state{};
    std::array<unsigned,8> stencil{};
    std::array<unsigned,3> alpha{};
    Tev tev{};
    unsigned textures{};
    bool depth_write{};
};
inline bool Near(float a,float b){return std::isfinite(a)&&std::abs(a-b)<.00001f;}
// Two complementary stencil masks and the purple cover use the same native
// three-quad screen mesh. Match state and transform, never a VRAM address.
inline std::optional<unsigned> Identify(const Values& u,const Material& m) {
    if(m.depth_write)return {};
    const std::array<std::array<float,4>,4> projection{{
        {0,-1.f/120,0,1},{-1.f/200,0,0,1},{0,0,1.f/1800,1.f/3},{0,0,0,1}}};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
        if(!Near(u[r][c],projection[r][c]))return {};
    const float scale=u[22][2];
    if(!std::isfinite(scale)||scale<.2499f||scale>1.5f)return {};
    const std::array<std::array<float,4>,3> model{{{-scale,0,0,200},{0,-scale,0,120},{0,0,scale,-1200}}};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
        if(!Near(u[20+r][c],model[r][c]))return {};
    Tev expected{};
    for(auto& t:expected)t={0x0fff0fff,0,0,0xffffffff,0};
    if(m.textures==1 && m.state==std::array<unsigned,7>{1,1,1,0,15,0,1}) {
        const unsigned ref=m.stencil[2];
        if(ref>1||m.stencil!=std::array<unsigned,8>{1,0,ref,255,255,2,2,2}||
           m.alpha!=std::array<unsigned,3>{1,ref?7u:4u,204})return {};
        expected[0]={238226995,0,0,0xff000000,0};
        if(m.tev==expected)return ref;
    } else if(m.textures==3 && m.state==std::array<unsigned,7>{1,6,7,0,15,1,0} && !m.alpha[0]) {
        expected[0]={238945091,131072,65545,0xff000000,1};
        expected[1]={250547951,0,65537,1721303193,0};
        if(m.tev==expected)return 2;
    }
    return {};
}
class History {
    struct Entry {std::uint64_t frame;Key key;Values values;bool ambiguous=false;};
    std::array<std::optional<Entry>,3> entries;
public:
    void Reset(){entries={};}
    std::optional<Pose> Apply(std::uint64_t frame,const Key& key,unsigned kind,const Values& u) {
        if(kind>=entries.size())return {};
        auto& e=entries[kind];
        if(e&&e->frame==frame){e->ambiguous=true;return {};}
        bool compatible=e&&!e->ambiguous&&e->frame+1==frame&&e->key==key&&
            std::abs(e->values[22][2]-u[22][2])<=.12f;
        if(compatible)for(unsigned r=0;r<14;++r)compatible &= e->values[r]==u[r];
        std::optional<Pose> half;
        if(compatible&&e->values[22][2]!=u[22][2]) {
            half.emplace();half->values=u;half->alpha=1;
            for(unsigned r=20;r<23;++r) {
                half->rows[r]=true;
                for(unsigned c=0;c<4;++c)half->values[r][c]=(e->values[r][c]+u[r][c])*.5f;
            }
        }
        e=Entry{frame,key,u};return half;
    }
};
}
