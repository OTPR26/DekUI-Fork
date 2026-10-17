// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/forest_particle_interpolation.h"

namespace MM3D::SnowInterpolation {
using ForestParticleInterpolation::Vertex;
using ForestParticleInterpolation::Quad;
using ForestParticleInterpolation::Center;
using ForestParticleInterpolation::Near;
using Tev=std::array<std::array<std::uint32_t,5>,6>;
inline bool Material(const Tev& tev) {
    // Captured mountain snow: inverse vertex RGB, texture alpha and ordinary
    // source-alpha blending. Do not admit other pale weather/effect sprites.
    if(tev[0]!=std::array<std::uint32_t,5>{0x0e300e30,0x20000,0x10008,0x00ffffff,0})return false;
    for(unsigned i=1;i<6;++i)
        if(tev[i]!=std::array<std::uint32_t,5>{0x0fff0fff,0,0,0xffffffff,0})return false;
    return true;
}
inline float SizeSquared(const Quad& q) {
    float result=0;
    for(unsigned c=0;c<3;++c) {
        const float edge=q.vertices[1][0][c]-q.vertices[0][0][c];result+=edge*edge;
    }
    return result;
}
inline std::optional<std::vector<Quad>> Decode(std::span<const Vertex> inputs,
                                              std::span<const std::uint32_t> order) {
    if(inputs.empty()||inputs.size()>257||order.empty()||order.size()>384||order.size()%6)return {};
    for(auto i:order)if(i>=inputs.size())return {};
    std::vector<bool> used(inputs.size());std::vector<Quad> out;
    for(unsigned i=0;i<order.size();i+=6) {
        if(order[i+4]!=order[i+3]||(i+6<order.size()&&order[i+5]!=order[i+6]))return {};
        Quad q{};
        for(unsigned k=0;k<4;++k) {
            const auto index=order[i+k];if(used[index])return {};used[index]=true;
            q.indices[k]=index;q.vertices[k]=inputs[index];const auto& v=q.vertices[k];
            for(const auto& row:v)for(float f:row)if(!std::isfinite(f))return {};
            if(!Near(v[0][3],1)||v[1][3]<0||v[1][3]>1||
               !Near(v[2][0],k<2?0.f:1.f)||!Near(v[2][1],k%2?0.f:1.f)||
               !Near(v[2][2],0)||!Near(v[2][3],1))return {};
            for(unsigned c=0;c<3;++c)if(v[1][c]!=0)return {};
            if(v[1][3]!=q.vertices[0][1][3])return {};
        }
        float aa=0,bb=0,ab=0;
        for(unsigned c=0;c<3;++c) {
            const float a=q.vertices[1][0][c]-q.vertices[0][0][c];
            const float b=q.vertices[2][0][c]-q.vertices[0][0][c];
            aa+=a*a;bb+=b*b;ab+=a*b;
            if(std::abs(q.vertices[3][0][c]-q.vertices[0][0][c]-a-b)>.001f)return {};
        }
        // Snowflakes retain a fixed size between 2 and 4.24 world units.
        if(aa<3.99f||aa>18.1f||std::abs(aa-bb)>.01f||std::abs(ab)>.01f)return {};
        out.push_back(q);
    }
    if(used[order.back()])return {}; // preserve the trailing strip connector
    return out;
}
inline bool Compatible(const Quad& a,const Quad& b) {
    if(std::abs(SizeSquared(a)-SizeSquared(b))>.01f||
       std::abs(a.vertices[0][1][3]-b.vertices[0][1][3])>.025f)return false;
    const auto ac=Center(a),bc=Center(b);float distance=0,dot=0;
    for(unsigned c=0;c<3;++c) {
        distance+=(ac[c]-bc[c])*(ac[c]-bc[c]);
        dot+=(a.vertices[1][0][c]-a.vertices[0][0][c])*
             (b.vertices[1][0][c]-b.vertices[0][0][c]);
    }
    return distance<=64 && dot>.5f*std::sqrt(SizeSquared(a)*SizeSquared(b));
}
inline std::optional<unsigned> Match(const Quad& q,const std::vector<Quad>& candidates) {
    std::optional<unsigned> index;float best=std::numeric_limits<float>::infinity(),second=best;
    const auto center=Center(q);
    for(unsigned i=0;i<candidates.size();++i) {
        if(!SnowInterpolation::Compatible(q,candidates[i]))continue;
        const auto c=Center(candidates[i]);float d=0;
        for(unsigned k=0;k<3;++k)d+=(c[k]-center[k])*(c[k]-center[k]);
        if(d<best){second=best;best=d;index=i;}else if(d<second)second=d;
    }
    return second>best*1.5f+.01f?index:std::nullopt;
}
class History {
    using Key=std::array<std::uint32_t,2>; // active texture, generation
    struct Entry {std::uint64_t frame;std::vector<Quad> quads;};
    std::map<Key,Entry> entries;
public:
    void Reset(){entries.clear();}
    std::optional<std::vector<Quad>> Apply(std::uint64_t frame,const Key& key,const std::vector<Quad>& quads) {
        if(quads.empty()||quads.size()>64)return {};
        for(auto it=entries.begin();it!=entries.end();)
            if(it->second.frame>frame||frame-it->second.frame>1)it=entries.erase(it);else ++it;
        auto it=entries.find(key);
        if(it==entries.end()){if(entries.size()<8)entries.emplace(key,Entry{frame,quads});return {};}
        auto& e=it->second;
        if(e.frame==frame){e.quads.clear();return {};}
        auto out=quads;bool changed=false;
        if(e.frame+1==frame)for(unsigned i=0;i<quads.size();++i) {
            const auto match=SnowInterpolation::Match(quads[i],e.quads);
            if(!match||SnowInterpolation::Match(e.quads[*match],quads)!=i)continue;
            const auto& old=e.quads[*match];
            for(unsigned v=0;v<4;++v) {
                for(unsigned c=0;c<3;++c)
                    out[i].vertices[v][0][c]=(quads[i].vertices[v][0][c]+old.vertices[v][0][c])*.5f;
                out[i].vertices[v][1][3]=(quads[i].vertices[v][1][3]+old.vertices[v][1][3])*.5f;
            }
            // Camera-facing sprites must not shrink while their basis turns.
            ForestParticleInterpolation::PreserveSpinSize(out[i],quads[i],old);
            changed=true;
        }
        e={frame,quads};return changed?std::optional{std::move(out)}:std::nullopt;
    }
};
}
