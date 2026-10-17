// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/water_particle_interpolation.h"

namespace MM3D::LeafParticleInterpolation {
using Vertex = std::array<std::array<float,4>,4>; // position, normal, color, UV
struct Quad {
    std::array<Vertex,4> vertices;
    std::array<std::uint32_t,4> indices;
    bool grass{};
};
using Key = std::array<std::uint32_t,2>; // active texture and scene generation
using ForestParticleInterpolation::Near;
inline std::array<float,3> Center(const Quad& q) {
    std::array<float,3> out{};
    for(const auto& v:q.vertices)for(unsigned a=0;a<3;++a)out[a]+=v[0][a]*.25f;
    return out;
}
inline std::optional<std::vector<Quad>> Decode(std::span<const Vertex> inputs,
                                              std::span<const std::uint32_t> order) {
    if(inputs.empty() || inputs.size()>513 || order.size()>768)return {};
    std::vector<ForestParticleInterpolation::Vertex> projected;
    for(const auto& v:inputs)projected.push_back({v[0],v[2],v[3]});
    const auto decoded=WaterParticleInterpolation::DecodeQuads(projected,order);
    if(!decoded)return {};
    std::vector<Quad> out;
    for(const auto& source:*decoded) {
        Quad q{};q.indices=source.indices;
        for(unsigned k=0;k<4;++k) {
            q.vertices[k]=inputs[q.indices[k]];
            const auto& v=q.vertices[k];
            for(const auto& row:v)for(float f:row)if(!std::isfinite(f))return {};
            for(unsigned a=0;a<4;++a)
                if(!Near(v[2][a],1) || !Near(v[1][a],q.vertices[0][1][a]))return {};
            if(!Near(v[1][3],1))return {};
        }
        // Captured brown leaf atlas: the two upper cells and lower-left cell.
        const auto& uv=q.vertices[0][3];
        q.grass=Near(uv[0],0) && Near(uv[1],1) && Near(q.vertices[2][3][0],1);
        if(!q.grass && !((Near(uv[0],0) && (Near(uv[1],1) || Near(uv[1],.5f))) ||
             (Near(uv[0],.5f) && Near(uv[1],1))))return {};
        const float uv_size=q.grass?1.f:.5f;
        for(unsigned k=0;k<4;++k)
            if(!Near(q.vertices[k][3][0],uv[0]+(k<2?0.f:uv_size)) ||
               !Near(q.vertices[k][3][1],uv[1]-(k%2?uv_size:0.f)))return {};
        float aa=0,bb=0,ab=0,nn=0,an=0,bn=0;
        for(unsigned c=0;c<3;++c) {
            const float a=q.vertices[1][0][c]-q.vertices[0][0][c];
            const float b=q.vertices[2][0][c]-q.vertices[0][0][c];
            const float n=q.vertices[0][1][c];
            aa+=a*a;bb+=b*b;ab+=a*b;nn+=n*n;an+=a*n;bn+=b*n;
        }
        if(std::abs(ab)>.05f || std::abs(an)>.05f || std::abs(bn)>.05f)return {};
        if(q.grass) {
            // Cut grass uses full-texture rectangles with a 7:5 aspect ratio.
            // Both captured variants have normals scaled to short_edge / 20.
            // Their animated squared short edges span 8.81--71.20 in Snowhead.
            if(aa<8.7f || aa>71.3f || std::abs(bb-aa*1.96f)>.05f || std::abs(nn*400-aa)>.05f)return {};
        } else {
            // Brown leaves and Majora's falling fragments use 6--16 unit
            // squares. f24 rounding puts nominal six-unit edges just below 36.
            if(aa<35.9f || aa>256 || std::abs(aa-bb)>.1f || std::abs(nn-aa)>.1f)return {};
        }
        out.push_back(q);
    }
    return out;
}
inline float Distance(const Quad& a,const Quad& b) {
    if(a.grass!=b.grass)return std::numeric_limits<float>::infinity();
    const auto ac=Center(a),bc=Center(b);
    float distance=0,normal=0,an=0,bn=0;
    for(unsigned c=0;c<3;++c) {
        distance+=(ac[c]-bc[c])*(ac[c]-bc[c]);
        const float d=a.vertices[0][1][c]-b.vertices[0][1][c];normal+=d*d;
        an+=a.vertices[0][1][c]*a.vertices[0][1][c];
        bn+=b.vertices[0][1][c]*b.vertices[0][1][c];
    }
    // Small detachment fragments accelerate to twelve units per guest frame.
    // Extend their motion bound only for equal-size squares; reciprocal nearest
    // matching still rejects overlapping particles and compacted array slots.
    const bool falling=!a.grass && an<=81 && bn<=81 && std::abs(an-bn)<.1f;
    if(distance>(a.grass?64.f:falling?150.f:36.f) || normal>(a.grass?.04f:16.f))return std::numeric_limits<float>::infinity();
    for(unsigned k=0;k<4;++k)for(unsigned c=0;c<3;++c)
        if(std::abs(a.vertices[k][0][c]-b.vertices[k][0][c])>(a.grass?10.f:falling?16.f:8.f))
            return std::numeric_limits<float>::infinity();
    return distance;
}
inline std::optional<unsigned> Match(const Quad& q,const std::vector<Quad>& candidates) {
    std::optional<unsigned> index;
    float best=std::numeric_limits<float>::infinity(),second=best;
    for(unsigned i=0;i<candidates.size();++i) {
        const float d=Distance(q,candidates[i]);
        if(d<best){second=best;best=d;index=i;}else if(d<second)second=d;
    }
    return second>best*1.5f+.01f?index:std::nullopt;
}
class History {
    struct Entry { std::uint64_t frame;std::vector<Quad> quads; };
    std::map<Key,Entry> entries;
public:
    void Reset(){entries.clear();}
    std::optional<std::vector<Quad>> Apply(std::uint64_t frame,const Key& key,const std::vector<Quad>& quads) {
        if(quads.empty() || quads.size()>128)return {};
        for(auto it=entries.begin();it!=entries.end();)
            if(it->second.frame>frame || frame-it->second.frame>1)it=entries.erase(it);else ++it;
        auto it=entries.find(key);
        if(it==entries.end()) {
            if(entries.size()<16)entries.emplace(key,Entry{frame,quads});
            return {};
        }
        auto& e=it->second;
        if(e.frame==frame){e.quads.clear();return {};}
        auto out=quads;bool changed=false;
        if(e.frame+1==frame)for(unsigned i=0;i<quads.size();++i) {
            const auto match=Match(quads[i],e.quads);
            if(!match || Match(e.quads[*match],quads)!=i)continue;
            // Atlas cells change discretely as leaves tumble. Keep current UVs
            // and colors, averaging only position and normal (not atlas coordinates).
            for(unsigned k=0;k<4;++k)for(unsigned a=0;a<2;++a)for(unsigned c=0;c<3;++c)
                out[i].vertices[k][a][c]=(quads[i].vertices[k][a][c]+e.quads[*match].vertices[k][a][c])*.5f;
            changed=true;
        }
        e={frame,quads};return changed?std::optional{std::move(out)}:std::nullopt;
    }
};
}
