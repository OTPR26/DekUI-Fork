// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/inventory_interpolation.h"

namespace MM3D::HeartInterpolation {
using InventoryInterpolation::Values;
using InventoryInterpolation::Tev;
using InventoryInterpolation::Pose;
using Key=std::array<std::uint32_t,3>;
using Row=std::array<float,4>;
inline bool Near(float a,float b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<.0001f;}
inline bool Cell(const Row& a,const Row& b) {
    return a[0]==0&&Near(a[1],.97265625f)&&a[2]==0&&a[3]==.0234375f&&
        b==Row{0,.0078125f,.046875f,0};
}
inline bool Candidate(std::uint64_t shader,std::uint64_t swizzle,unsigned vertices,
                      float color,const Row& uv0,const Row& uv1) {
    return shader==0x9b343fea686d98e0ULL&&swizzle==0xac22b79e29209c01ULL&&
        vertices>=6&&vertices<=84&&vertices%6==0&&color==1&&Cell(uv0,uv1);
}
inline bool Recognized(const Values& u,unsigned vertices,const Tev& tev) {
    if(!Candidate(0x9b343fea686d98e0ULL,0xac22b79e29209c01ULL,vertices,u[10][3],u[94],u[95]))return false;
    for(unsigned i=0;i<6;++i)
        if(tev[i]!=std::array<std::uint32_t,5>{i==0?0x0ee00ee0u:i==1?0x0e3f0e3fu:0x0eef0eefu,
            0,i<2?0x10001u:0u,0xffffffffu,0})return false;
    const unsigned count=vertices/6;
    for(unsigned r=0;r<10+4*count;++r)for(float v:u[r])if(!std::isfinite(v))return false;
    for(unsigned q=0;q<count;++q) {
        const unsigned r=10+4*q,uv=94-2*q;
        if(u[r]!=Row{0,3,float(uv),1}||!Cell(u[uv],u[uv+1])||
           u[r+1][1]!=0||u[r+1][2]!=0||u[r+2][1]!=0||u[r+2][3]!=0||
           u[r+1][3]<=0||u[r+1][3]>64||!Near(u[r+1][3],u[r+2][2])||
           u[r+3]!=Row{0,.5f,0,0})return false;
    }
    return true;
}
class History {
    struct Entry {std::uint64_t frame;Key key;Values values;bool ambiguous=false;};
    std::array<std::optional<Entry>,14> entries;
public:
    void Reset(){entries={};}
    std::optional<Pose> Apply(std::uint64_t frame,const Key& key,const Values& u,unsigned vertices,const Tev& tev) {
        if(!vertices||vertices>84||vertices%6)return {};
        auto& entry=entries[vertices/6-1];
        if(!Recognized(u,vertices,tev)){entry.reset();return {};}
        if(entry&&entry->frame==frame){entry->ambiguous=true;return {};}
        bool compatible=entry&&!entry->ambiguous&&entry->frame+1==frame&&entry->key==key;
        if(compatible) {
            for(unsigned r=0;r<10;++r)if(r!=8)compatible&=entry->values[r]==u[r];
            for(unsigned q=0;q<vertices/6;++q) {
                const unsigned r=10+4*q;const auto& p=entry->values;
                // Pulse only: never interpolate HUD repositioning or health
                // atlas changes. A full heart stays centered as its size varies.
                compatible&=Near(p[r+1][0]+p[r+1][3]*.5f,u[r+1][0]+u[r+1][3]*.5f)&&
                    Near(p[r+2][0]+p[r+2][2]*.5f,u[r+2][0]+u[r+2][2]*.5f)&&
                    std::abs(p[r+1][3]-u[r+1][3])<=u[r+1][3]*.25f;
            }
        }
        std::optional<Pose> half;
        if(compatible) {
            Pose pose;pose.values=u;pose.alpha=1;bool changed=false;
            for(unsigned q=0;q<vertices/6;++q)for(unsigned r=11+4*q;r<13+4*q;++r)
                if(entry->values[r]!=u[r]) {
                    changed=true;pose.rows[r]=true;
                    for(unsigned c=0;c<4;++c)pose.values[r][c]=(entry->values[r][c]+u[r][c])*.5f;
                }
            if(changed)half=pose;
        }
        entry=Entry{frame,key,u};return half;
    }
};
}
