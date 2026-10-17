// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/water_particle_interpolation.h"

namespace MM3D::FireImpactInterpolation {
using ForestParticleInterpolation::Vertex;
using ForestParticleInterpolation::Quad;
using ForestParticleInterpolation::Center;
using ForestParticleInterpolation::Near;
using Tev=std::array<std::array<std::uint32_t,5>,6>;
enum class Kind { None, Cloud, Spark };
inline Kind Material(const Tev& tev) {
    if(tev[0]!=std::array<std::uint32_t,5>{0x0ee00ee3,0,0,0,2}||
       tev[1][0]!=0x0eee0efe||tev[1][1]!=0||tev[1][2]!=1||tev[1][4]!=0||
       tev[2]!=std::array<std::uint32_t,5>{0x0ee00f30,0,8,0,0})return Kind::None;
    for(unsigned i=3;i<6;++i)
        if(tev[i]!=std::array<std::uint32_t,5>{0x0fff0fff,0,0,0xffffffff,0})return Kind::None;
    if(tev[1][3]==15129780)return Kind::Cloud;
    if((tev[1][3]&0xffffff)==0xffffff)return Kind::Spark;
    return Kind::None;
}
inline float Edge(const Quad& q,unsigned corner=1) {
    float length=0;
    for(unsigned c=0;c<3;++c) {
        const float d=q.vertices[corner][0][c]-q.vertices[0][0][c];length+=d*d;
    }
    return std::sqrt(length);
}
inline unsigned Tile(const Quad& q) {
    return unsigned(std::lround(q.vertices[0][2][0]*4))+
        4*unsigned(std::lround((1-q.vertices[0][2][1])*4));
}
inline std::optional<std::vector<Quad>> Decode(std::span<const Vertex> inputs,
        std::span<const std::uint32_t> order,Kind kind) {
    if(kind==Kind::None||inputs.size()>257||order.size()>384)return {};
    auto quads=WaterParticleInterpolation::DecodeQuads(inputs,order);
    if(!quads||quads->empty())return {};
    for(const auto& q:*quads) {
        const bool cloud=kind==Kind::Cloud;
        const std::array<float,3> tint=cloud?std::array<float,3>{-.166089892f,-.067666233f,-.070741951f}:
            std::array<float,3>{0,0,-128.f/255};
        for(unsigned c=0;c<3;++c)if(!Near(q.vertices[0][1][c],tint[c]))return {};
        const float width=Edge(q,2),height=Edge(q);
        if(width<(cloud?39.f:1.4f)||width>(cloud?124.f:2.25f)||std::abs(width-height)>.005f)return {};
        float dot=0;
        for(unsigned c=0;c<3;++c)dot+=(q.vertices[1][0][c]-q.vertices[0][0][c])*
            (q.vertices[2][0][c]-q.vertices[0][0][c]);
        if(std::abs(dot)>.001f*width*height)return {};
        const auto& uv=q.vertices[0][2];
        if(cloud&&(uv[0]<0||uv[0]>.75f||uv[1]<.25f||uv[1]>1||
            !Near(uv[0]*4,std::round(uv[0]*4))||!Near(uv[1]*4,std::round(uv[1]*4))))return {};
        for(unsigned k=0;k<4;++k) {
            const float u=cloud?uv[0]+float(k/2)*.25f:float(k/2)*2;
            const float v=cloud?uv[1]-float(k%2)*.25f:float(1-k%2)*2;
            if(!Near(q.vertices[k][2][0],u)||!Near(q.vertices[k][2][1],v))return {};
        }
    }
    return quads;
}
// Geometry is world-space; native view/model uniforms still supply camera motion.
inline bool Compatible(const Quad& current,const Quad& previous,Kind kind) {
    const bool cloud=kind==Kind::Cloud;
    for(unsigned c=0;c<3;++c)if(!Near(current.vertices[0][1][c],previous.vertices[0][1][c]))return false;
    const float alpha=current.vertices[0][1][3]-previous.vertices[0][1][3];
    if(cloud) {
        if(std::abs(alpha)>.028f||std::abs(Edge(current)-Edge(previous))>.005f||
           Tile(current)!=(Tile(previous)+1)%16)return false;
    } else if(alpha>.001f||alpha<-.08f)return false;
    const auto a=Center(current),b=Center(previous);float distance=0,dot=0;
    for(unsigned c=0;c<3;++c) {
        distance+=(a[c]-b[c])*(a[c]-b[c]);
        dot+=(current.vertices[1][0][c]-current.vertices[0][0][c])*
             (previous.vertices[1][0][c]-previous.vertices[0][0][c]);
    }
    return distance<=(cloud?16.f:4.f)&&dot>.7f*Edge(current)*Edge(previous);
}
inline std::optional<unsigned> Match(const Quad& q,const std::vector<Quad>& candidates,Kind kind,bool newer) {
    std::optional<unsigned> index;float best=std::numeric_limits<float>::infinity(),second=best;
    const auto center=Center(q);
    for(unsigned i=0;i<candidates.size();++i) {
        if(!(newer?Compatible(q,candidates[i],kind):Compatible(candidates[i],q,kind)))continue;
        const auto c=Center(candidates[i]);float d=0;
        for(unsigned k=0;k<3;++k)d+=(c[k]-center[k])*(c[k]-center[k]);
        if(d<best){second=best;best=d;index=i;}else if(d<second)second=d;
    }
    return second>best*1.5f+.01f?index:std::nullopt;
}
class History {
    using Key=std::array<std::uint32_t,3>; // active atlas, generation, kind
    struct Entry {std::uint64_t frame;std::vector<Quad> quads;};
    std::map<Key,Entry> entries;
public:
    void Reset(){entries.clear();}
    std::optional<std::vector<Quad>> Apply(std::uint64_t frame,std::uint32_t texture,
            std::uint32_t generation,Kind kind,const std::vector<Quad>& quads) {
        if(kind==Kind::None||quads.empty()||quads.size()>64)return {};
        for(auto it=entries.begin();it!=entries.end();)
            if(it->second.frame>frame||frame-it->second.frame>1)it=entries.erase(it);else ++it;
        const Key key{texture,generation,std::uint32_t(kind)};auto it=entries.find(key);
        if(it==entries.end()){if(entries.size()<8)entries.emplace(key,Entry{frame,quads});return {};}
        auto& e=it->second;
        if(e.frame==frame){e.quads.clear();return {};}
        auto out=quads;bool changed=false;
        if(e.frame+1==frame)for(unsigned i=0;i<quads.size();++i) {
            const auto match=FireImpactInterpolation::Match(quads[i],e.quads,kind,true);
            if(!match||FireImpactInterpolation::Match(e.quads[*match],quads,kind,false)!=i)continue;
            const auto& old=e.quads[*match];
            for(unsigned v=0;v<4;++v) {
                for(unsigned c=0;c<3;++c)
                    out[i].vertices[v][0][c]=(quads[i].vertices[v][0][c]+old.vertices[v][0][c])*.5f;
                out[i].vertices[v][1][3]=(quads[i].vertices[v][1][3]+old.vertices[v][1][3])*.5f;
            }
            // Do not interpolate atlas cells: that would slide through other
            // animation tiles. Retain the current sprite and blend only motion.
            ForestParticleInterpolation::PreserveSpinSize(out[i],quads[i],old);
            changed=true;
        }
        e={frame,quads};return changed?std::optional{std::move(out)}:std::nullopt;
    }
};
}
