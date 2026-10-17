// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <vector>
#include "mm3d/camera_midpoint.h"
#include "mm3d/song_effect_interpolation.h"

namespace MM3D::WaterModelInterpolation {
using CameraMidpoint::Matrix;
using SongEffectInterpolation::Values;
using Tev=std::array<std::array<std::uint32_t,5>,6>;
struct Pattern {
    unsigned vertices,mask,destination,alpha_stage;
    std::array<float,6> basis,step;
};
inline constexpr Pattern Patterns[]{
    {288,7,7,1,{3,1,5,2,1,1},{0,-1.f/60,0,-1.f/15,0,0}},
    {192,3,1,1,{4,2,6,2,0,0},{0,-1.f/30,0,-1.f/15,0,0}},
    {192,7,1,1,{4,1,6,2,1,1},{0,-1.f/30,0,-1.f/10,0,0}},
    {192,3,1,1,{6,2,8,2,0,0},{0,-1.f/10,0,-1.f/15,0,0}},
    {120,7,1,0,{1,1,2,2,2,5},{0,0,-1.f/60,1.f/30,1.f/60,1.f/24}},
    {120,7,7,0,{1,1,2,2,2,5},{0,0,-1.f/60,1.f/30,1.f/60,1.f/24}},
};
// Captured layered splash columns and expanding surface waves. These materials
// animate their model scale, scrolling UVs and combiner opacity independently.
inline std::optional<unsigned> Identify(unsigned vertices,unsigned mask,unsigned destination,
                                        const Values& uv,const Tev& tev) {
    for(unsigned kind=0;kind<std::size(Patterns);++kind) {
        const auto& p=Patterns[kind];
        if(vertices!=p.vertices || mask!=p.mask ||
           (destination!=p.destination && !(kind==0 && destination==1)))continue;
        Tev expected{};
        for(auto& stage:expected)stage={268374015,0,0,0xffffffff,0};
        if(kind<4) {
            expected[0]=(kind==0||kind==2)?std::array<std::uint32_t,5>{4392259,139264,589832,0xff000000,0}:
                std::array<std::uint32_t,5>{200259,8192,65537,0xff000000,2};
            expected[1]={250547743,0,65536,0xffffffff,0};
        } else if(kind==4) {
            expected[0]={239076675,131072,589833,0xffffffff,1};
            expected[1]={239013439,131072,65545,0xfff2e5d8,0};
        } else {
            expected[0]={239076675,131072,589833,0xffffffff,0};
            expected[1]={239013439,131072,65538,0xff332600,0};
            expected[2]={250547951,0,65537,0xff332600,0};
        }
        auto material=tev;material[p.alpha_stage][3]|=0xff000000;
        if(material!=expected)continue;
        bool valid=true;
        for(unsigned r=0;r<(p.mask==7?6u:4u);++r)for(unsigned c=0;c<4;++c)
            valid &= std::isfinite(uv[r][c]) &&
                (c==3 || std::abs(uv[r][c]-(c==r%2?p.basis[r]:0.f))<1e-5f);
        if(valid)return kind;
    }
    return {};
}
struct Sample { Matrix model;Values uv;float alpha; };
inline bool Model(const Matrix& m) {
    if(!CameraMidpoint::Near(m,m,0))return false;
    // Upright, yaw-rotated, independently scaled columns. A surface wave has
    // zero vertical scale; do not require an invertible skeletal transform.
    const float radius=std::hypot(m[0][0],m[2][0]);
    return radius>.00001f && radius<1 && m[1][1]>=-1e-6 && m[1][1]<1 &&
        std::abs(m[0][0]-m[2][2])<1e-5 && std::abs(m[0][2]+m[2][0])<1e-5 &&
        std::abs(m[0][1])<1e-5 && std::abs(m[2][1])<1e-5 &&
        std::abs(m[1][0])<1e-5 && std::abs(m[1][2])<1e-5 &&
        m[3]==std::array<double,4>{0,0,0,1};
}
inline bool Same(const Sample& a,const Sample& b) {
    return CameraMidpoint::Near(a.model,b.model,1e-6) && a.uv==b.uv && a.alpha==b.alpha;
}
inline std::optional<double> Score(const Sample& now,const Sample& old,unsigned kind) {
    const auto& p=Patterns[kind];
    double distance=0,dot=0,aa=0,bb=0;
    for(unsigned c=0;c<3;++c) {
        distance+=std::pow(now.model[c][3]-old.model[c][3],2);
        dot+=now.model[c][0]*old.model[c][0];
        aa+=now.model[c][0]*now.model[c][0];bb+=old.model[c][0]*old.model[c][0];
    }
    if(distance>.01*.01 || dot<.9999*std::sqrt(aa*bb) ||
       now.alpha>old.alpha+.001f || old.alpha-now.alpha>.1f)return {};
    double score=distance+std::pow(now.alpha-old.alpha,2);
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c) {
        const double delta=now.model[r][c]-old.model[r][c];
        if(std::abs(delta)>.01)return {};
        score+=delta*delta*10000;
    }
    bool moving=true,stationary=true;
    for(unsigned r=0;r<(p.mask==7?6u:4u);++r) {
        const float delta=now.uv[r][3]-old.uv[r][3];
        const float step=delta-std::round(delta);
        moving &= std::abs(step-p.step[r])<1e-5;
        stationary &= std::abs(step)<1e-5;
    }
    return moving||stationary?std::optional{score}:std::nullopt;
}
class History {
public:
    using Key=std::array<std::uint32_t,5>; // active textures, generation, pattern
private:
    struct Current {Sample native;std::optional<Sample> half;};
    struct Entry {
        std::uint64_t frame{};
        std::vector<Sample> previous;
        std::vector<Current> current;
        std::vector<bool> used;
    };
    std::map<Key,Entry> entries;
public:
    void Reset(){entries.clear();}
    std::optional<Sample> Apply(std::uint64_t frame,const Key& key,const Sample& value) {
        if(key[4]>=std::size(Patterns) || !Model(value.model) || !std::isfinite(value.alpha) ||
           value.alpha<0 || value.alpha>1)return {};
        const unsigned rows=Patterns[key[4]].mask==7?6:4;
        for(unsigned r=0;r<rows;++r)for(float x:value.uv[r])if(!std::isfinite(x))return {};
        for(auto it=entries.begin();it!=entries.end();)
            if(it->second.frame>frame || frame-it->second.frame>1)it=entries.erase(it);else ++it;
        auto it=entries.find(key);
        if(it==entries.end()) {
            if(entries.size()>=32)return {};
            it=entries.emplace(key,Entry{}).first;
        }
        auto& e=it->second;
        if(e.frame!=frame) {
            e.previous.clear();
            if(e.frame+1==frame)for(const auto& v:e.current)e.previous.push_back(v.native);
            e.current.clear();e.used.assign(e.previous.size(),false);e.frame=frame;
        }
        for(const auto& v:e.current)if(Same(v.native,value))return v.half;
        if(e.current.size()>=32)return {};
        std::optional<unsigned> best;double first=1e30,second=1e30;
        for(unsigned i=0;i<e.previous.size();++i) {
            const auto score=Score(value,e.previous[i],key[4]);
            if(!score)continue;
            if(*score<first){second=first;first=*score;best=i;}else if(*score<second)second=*score;
        }
        std::optional<Sample> half;
        if(best && !e.used[*best] && second>first*1.5+1e-8) {
            half=value;const auto& old=e.previous[*best];e.used[*best]=true;
            for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
                half->model[r][c]=(value.model[r][c]+old.model[r][c])*.5;
            for(unsigned r=0;r<rows;++r) {
                const float delta=value.uv[r][3]-old.uv[r][3];
                half->uv[r][3]=value.uv[r][3]-(delta-std::round(delta))*.5f;
            }
            half->alpha=(value.alpha+old.alpha)*.5f;
        }
        e.current.push_back({value,half});return half;
    }
};
}
