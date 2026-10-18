// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/ripple_interpolation.h"
#include "mm3d/water_particle_interpolation.h"
namespace MM3D::SplashInterpolation {
struct Sprite {
    std::array<RippleInterpolation::Point,5> points;
    std::array<RippleInterpolation::Color,5> colors;
    std::array<std::array<float,2>,5> uv;
};
struct Result {Sprite sprite;std::array<std::array<float,2>,5> old_uv;bool blend;};
inline std::optional<int> Cell(const Sprite& s) {
    using RippleInterpolation::Near;
    const auto u=s.uv[0][0],v=s.uv[0][1];
    if(!std::isfinite(u)||!std::isfinite(v)||u<0||u>.875f || (v!=1.f && v!=.5f))return {};
    const int col=int(std::round(u*8));if(std::abs(u-col*.125f)>1e-5f)return {};
    for(unsigned i=0;i<4;++i) {
        if(!std::isfinite(s.uv[i][0]) || !std::isfinite(s.uv[i][1]))return {};
        if(std::abs(s.uv[i][0]-(u+(i%2)*.125f))>1e-5f ||
           std::abs(s.uv[i][1]-(v-(i/2)*.5f))>1e-5f)return {};
        for(float p:s.points[i])if(!std::isfinite(p))return {};
        for(unsigned c=0;c<3;++c)if(!Near(s.colors[i][c],0))return {};
        if(!Near(s.colors[i][3],s.colors[0][3])||s.colors[i][3]<0||s.colors[i][3]>1)return {};
    }
    return col+(v==.5f?8:0);
}
struct CpuSprite {Sprite sprite;std::array<std::uint32_t,4> indices;};
inline std::optional<CpuSprite> Decode(std::span<const WaterParticleInterpolation::Vertex> inputs,
                                     std::span<const std::uint32_t> order) {
    if(order.size()!=6)return {};
    const auto quads=WaterParticleInterpolation::DecodeQuads(inputs,order);
    if(!quads || quads->size()!=1)return {};
    CpuSprite out{};
    constexpr unsigned corners[]{0,2,1,3};
    for(unsigned i=0;i<4;++i) {
        const auto& v=(*quads)[0].vertices[corners[i]];
        out.indices[i]=(*quads)[0].indices[corners[i]];
        for(unsigned c=0;c<3;++c)out.sprite.points[i][c]=v[0][c];
        out.sprite.colors[i]=v[1];
        out.sprite.uv[i]={v[2][0],v[2][1]};
    }
    return Cell(out.sprite)?std::optional{out}:std::nullopt;
}
class History {
    Sprite previous{};std::uint64_t frame{};std::uint32_t texture{},generation{};bool seen{};
public:
    void Reset(){seen=false;}
    std::optional<Result> Apply(std::uint64_t next,std::uint32_t tex,std::uint32_t gen,const Sprite& s) {
        const auto cell=Cell(s);if(!cell)return {};
        const auto old=Cell(previous);
        bool continuous=seen && next==frame+1 && tex==texture && gen==generation && old &&
            (*cell==*old || *cell==*old+1);
        for(unsigned c=0;c<3;++c)continuous &= std::abs((s.points[2][c]+s.points[3][c]-previous.points[2][c]-previous.points[3][c])*.5f)<.1f;
        for(unsigned i=0;i<4;++i)for(unsigned c=0;c<3;++c)
            continuous &= std::abs(s.points[i][c]-previous.points[i][c])<10;
        Result result{s,previous.uv,old && *old!=*cell};
        if(continuous)for(unsigned i=0;i<4;++i) {
            for(unsigned c=0;c<3;++c)result.sprite.points[i][c]=(s.points[i][c]+previous.points[i][c])*.5f;
            result.sprite.colors[i][3]=(s.colors[i][3]+previous.colors[i][3])*.5f*(result.blend?.5f:1.f);
        }
        previous=s;frame=next;texture=tex;generation=gen;seen=true;
        return continuous?std::optional<Result>{result}:std::nullopt;
    }
};
}
