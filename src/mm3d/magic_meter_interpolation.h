// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/inventory_interpolation.h"

namespace MM3D::MagicMeterInterpolation {
using InventoryInterpolation::Values;
using InventoryInterpolation::Tev;
using InventoryInterpolation::Pose;
using Key=std::array<std::uint32_t,3>; // texture, framebuffer, generation
using Row=std::array<float,4>;
inline bool Candidate(std::uint64_t shader,std::uint64_t swizzle,unsigned vertices,
                      float color,const Row& uv0,const Row& uv1) {
    return shader==0x9b343fea686d98e0ULL && swizzle==0xac22b79e29209c01ULL &&
        vertices==6 && color==1 && uv0==Row{0,.921875f,0,.015625f} &&
        uv1==Row{0,0,.03125f,0};
}
inline bool Recognized(const Values& u,const Tev& tev) {
    if(!Candidate(0x9b343fea686d98e0ULL,0xac22b79e29209c01ULL,6,u[10][3],u[94],u[95]) ||
       u[10]!=Row{0,3,94,1})return false;
    // Native green fill material, distinct from the black background and the
    // configurable border/shadows. Opacity may vary, but is never interpolated.
    for(unsigned i=0;i<6;++i) {
        auto t=tev[i];if(!i)t[3]|=0xff000000u;
        if(t!=std::array<std::uint32_t,5>{i==0?0x0ee00ee0u:i==1?0x0e3f0e3fu:0x0eef0eefu,
               0,i<2?0x10001u:0u,i==0?0xff06ff00u:0xffffffffu,0})return false;
    }
    for(unsigned r=0;r<14;++r)for(float v:u[r])if(!std::isfinite(v))return false;
    return u[11][1]==0 && u[11][2]==0 && u[11][3]>=0 && u[11][3]<=800 &&
        u[12][1]==0 && u[12][3]==0 && u[12][2]>0 && u[12][2]<=240 &&
        u[13][0]==0 && u[13][1]>0 && u[13][1]<=4 && u[13][2]==0 && u[13][3]==0;
}
class History {
    struct Entry {std::uint64_t frame;Key key;Values values;Tev tev;bool ambiguous=false;};
    std::optional<Entry> previous;
public:
    void Reset(){previous.reset();}
    std::optional<Pose> Apply(std::uint64_t frame,const Key& key,const Values& u,const Tev& tev) {
        if(!Recognized(u,tev)){Reset();return {};}
        if(previous && previous->frame==frame){previous->ambiguous=true;return {};}
        bool compatible=previous && !previous->ambiguous && previous->frame+1==frame &&
            previous->key==key && previous->tev==tev;
        if(compatible) {
            // Only refill/drain width may change. HUD edits, projection, tint,
            // height, placement and visibility changes seed a fresh native pose.
            for(unsigned r=0;r<14;++r)if(r!=8)for(unsigned c=0;c<4;++c)
                if(r!=11||c!=3)compatible &= previous->values[r][c]==u[r][c];
            compatible &= std::abs(previous->values[11][3]-u[11][3])<=16*u[13][1];
        }
        std::optional<Pose> half;
        if(compatible && previous->values[11][3]!=u[11][3]) {
            half.emplace();half->values=u;half->rows[11]=true;
            half->alpha=float(tev[0][3]>>24)/255.f;
            half->values[11][3]=(previous->values[11][3]+u[11][3])*.5f;
        }
        previous=Entry{frame,key,u,tev};return half;
    }
};
}
