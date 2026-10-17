// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/inventory_interpolation.h"

namespace MM3D::BannerInterpolation {
using FileSelectInterpolation::U32;
using FileSelectInterpolation::U64;
using FileSelectInterpolation::Key;
using FileSelectInterpolation::Values;
using FileSelectInterpolation::Tev;
using Pose=InventoryInterpolation::Pose;
struct Sample {U64 frame;U32 generation,owner;};
inline std::mutex mutex;
inline std::vector<Sample> samples;
inline void Publish(U64 frame,U32 generation,U32 owner) {
    std::scoped_lock lock(mutex);
    if(!samples.empty()&&(frame<samples.back().frame||generation!=samples.back().generation))samples.clear();
    if(!samples.empty()&&samples.back().frame==frame)samples.pop_back();
    if(samples.size()==8)samples.erase(samples.begin());
    samples.push_back({frame,generation,owner});
}
inline U32 Owner(U64 frame,U32 generation) {
    std::scoped_lock lock(mutex);
    for(auto it=samples.rbegin();it!=samples.rend();++it)
        if(it->generation==generation&&(it->frame==frame||it->frame+1==frame))return it->owner;
    return 0;
}
inline bool Near(float a,float b,float tolerance=.001f) {
    return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=tolerance;
}
// Telop's location text uses paired shadow/foreground glyphs from a 16x18
// font atlas. Keep the text, atlas, tint and size fixed; interpolate only the
// captured diagonal entrance and combiner opacity while Telop owns the screen.
inline bool Text(const Values& u,unsigned vertices,const Tev& tev) {
    if(!vertices||vertices>96||vertices%12||tev[1][1]!=32)return false;
    const unsigned count=vertices/6,free=10+count*4;
    const float scale=u[11][3]/16.f;
    if(!std::isfinite(scale)||scale<.1f||scale>4)return false;
    for(unsigned q=0;q<count;++q) {
        const unsigned r=10+4*q;const auto& desc=u[r];
        if(desc[0]!=1||desc[1]!=3||!std::isfinite(desc[2])||desc[2]<free||desc[2]>94||
           std::floor(desc[2])!=desc[2]||!std::isfinite(desc[3])||-desc[3]<free||-desc[3]>95||
           std::floor(desc[3])!=desc[3])return false;
        const unsigned uv=unsigned(desc[2]),color=unsigned(-desc[3]);
        if(u[uv][0]!=0||u[uv][2]!=0||u[uv][3]!=.0625f||
           u[uv+1][0]!=0||u[uv+1][2]!=.0703125f||u[uv+1][3]!=0||
           !std::isfinite(u[r+3][0])||u[r+3][0]<0||u[r+3][0]>.010001f||
           u[r+3][1]!=1||u[r+3][2]!=0||u[r+3][3]!=0||u[r+1][1]!=0||u[r+1][2]!=0||
           u[r+2][1]!=0||u[r+2][3]!=0||!Near(u[r+1][3],16*scale)||!Near(u[r+2][2],18*scale))return false;
        const std::array<float,4> tint=q%2?std::array<float,4>{1,244.f/255,244.f/255,244.f/255}:
            std::array<float,4>{.9f,0,0,0};
        for(unsigned c=0;c<4;++c)if(!Near(u[color][c],tint[c],.00001f))return false;
        if(q%2) {
            if(desc[2]!=u[r-4][2]||!Near(u[r-3][0]-u[r+1][0],scale)||
               !Near(u[r-2][0]-u[r+2][0],scale))return false;
        }
    }
    return true;
}
class History {
    InventoryInterpolation::History text;
public:
    void Reset(){text.Reset();}
    std::optional<Pose> Apply(U64 frame,const Key& key,const Values& u,unsigned vertices,const Tev& tev) {
        if(!key[3]||!Text(u,vertices,tev))return {};
        auto half=text.Apply(frame,key,u,vertices,tev);
        if(!half)return {};
        const float dx=half->values[11][0]-u[11][0],dy=half->values[12][0]-u[12][0];
        if(!Near(dx,dy)||std::abs(dx)>u[11][3]/16.f)return {};
        for(unsigned q=0;q<vertices/6;++q) {
            const unsigned r=11+4*q;
            if(!Near(half->values[r][0]-u[r][0],dx)||!Near(half->values[r+1][0]-u[r+1][0],dy))return {};
            if(std::abs(half->values[r+2][0]-u[r+2][0])>.001f)return {};
            for(unsigned k=r;k<r+3;++k)for(unsigned c=1;c<4;++c)
                if(!Near(half->values[k][c],u[k][c],.00001f))return {};
        }
        return half;
    }
};
}
