// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/lens_interpolation.h"

namespace MM3D::LetterboxInterpolation {
using LensInterpolation::Values;
using LensInterpolation::Material;
using LensInterpolation::Pose;
using LensInterpolation::Near;
using Key=std::array<std::uint32_t,2>;
// Native untextured black quads anchored to the top/bottom of the 400x240
// screen. Full-screen fades and other orthographic geometry are not bars.
inline std::optional<unsigned> Identify(const Values& u,const Material& m) {
    if(m.depth_write || m.textures || m.alpha[0] ||
       m.state!=std::array<unsigned,7>{1,6,7,0,15,0,0})return {};
    auto expected=m.tev;
    for(auto& t:expected)t={0x0fff0fff,0,0,0xffffffff,0};
    expected[0]={238030384,0,0,0,0};
    if(m.tev!=expected)return {};
    const std::array<std::array<float,4>,4> projection{{
        {0,-1.f/120,0,1},{-1.f/200,0,0,1},{0,0,1.f/1800,1.f/3},{0,0,0,1}}};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
        if(!Near(u[r][c],projection[r][c]))return {};
    const float height=u[21][1];
    if(!std::isfinite(height)||height<=0||height>=120)return {};
    const unsigned side=u[21][3]>120?1:0;
    const std::array<std::array<float,4>,3> model{{
        {400,0,0,200},{0,height,0,side?240-height*.5f:height*.5f},{0,0,1,-1200}}};
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c) {
        if(!Near(u[20+r][c],model[r][c]))return {};
        if(!Near(u[70+r][c],c==r?1.f:(r==2&&c==3?1200.f:0.f)))return {};
    }
    if(u[8]!=std::array<float,4>{1.f/255,0,1.f/255,1} ||
       u[9]!=std::array<float,4>{1,1,1,1})return {};
    return side;
}
class History {
    struct Entry {std::uint64_t frame;Key key;float height;bool ambiguous=false;};
    std::array<std::optional<Entry>,2> entries;
public:
    void Reset(){entries={};}
    std::optional<Pose> Apply(std::uint64_t frame,const Key& key,unsigned side,const Values& u) {
        if(side>=entries.size())return {};
        auto& e=entries[side];
        if(e&&e->frame==frame){e->ambiguous=true;return {};}
        const float height=u[21][1];
        std::optional<Pose> half;
        if(e&&!e->ambiguous&&e->frame+1==frame&&e->key==key&&
           std::abs(height-e->height)<=5.f && height!=e->height) {
            half.emplace();half->values=u;half->alpha=0;half->rows[21]=true; // Preserve TEV constant zero.
            half->values[21][1]=(height+e->height)*.5f;
            half->values[21][3]=side?240-half->values[21][1]*.5f:half->values[21][1]*.5f;
        }
        e=Entry{frame,key,height};return half;
    }
};
}
