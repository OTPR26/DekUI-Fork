// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#include "mm3d/camera_midpoint.h"
namespace MM3D {
struct GlareInterpolation {
    using Tev=std::array<std::array<std::uint32_t,5>,6>;
    static bool ScreenDraw(const CameraMidpoint::Matrix& projection,
                           const CameraMidpoint::Matrix& model,const Tev& tev) {
        CameraMidpoint::Matrix p{};
        p[0]={0,-1./120,0,1};p[1]={-1./200,0,0,1};
        p[2]={0,0,1./1800,1./3};p[3][3]=1;
        auto m=CameraMidpoint::Identity();m[2][3]=-1200;
        if(!CameraMidpoint::Near(projection,p,1e-6) ||
           !CameraMidpoint::Near(model,m,1e-6))return false;
        const Tev expected{{{238030384,0,65537,0,0},
            {250547951,0,65537,0xffffffff,0},
            {268374015,0,0,0xffffffff,0},{268374015,0,0,0xffffffff,0},
            {268374015,0,0,0xffffffff,0},{268374015,0,0,0xffffffff,0}}};
        auto material=tev;material[1][3]|=0xff000000; // animated glare intensity
        return material==expected;
    }
    // CPU vertices are already shaded. For this fixed orthographic effect,
    // interpolating clip XY is equivalent to interpolating input screen XY.
    // UV/depth checks preserve topology; no guest buffer or replay cache changes.
    struct ScreenVertex {
        std::array<float,4> position{},color{};
        std::array<float,2> uv{};
        bool operator==(const ScreenVertex&) const = default;
    };
    struct ScreenSample {
        std::vector<ScreenVertex> vertices;
        float alpha{};
        bool operator==(const ScreenSample&) const = default;
    };
    class ScreenHistory {
        using Key=std::array<std::uint32_t,3>; // texture, framebuffer, generation
        Key key{};
        std::uint64_t frame{};
        ScreenSample current;
        std::optional<ScreenSample> midpoint;
    public:
        void Reset() {frame=0;current={};midpoint.reset();}
        std::optional<ScreenSample> Apply(std::uint64_t next,const Key& next_key,
                                         const ScreenSample& sample) {
            if(sample.vertices.empty() || sample.vertices.size()>216 ||
               sample.vertices.size()%3 || !std::isfinite(sample.alpha) ||
               sample.alpha<0 || sample.alpha>1) {Reset();return {};}
            for(const auto& v:sample.vertices) {
                for(float x:v.position)if(!std::isfinite(x)){Reset();return {};}
                for(float x:v.color)if(!std::isfinite(x)){Reset();return {};}
                for(float x:v.uv)if(!std::isfinite(x)){Reset();return {};}
                if(v.position[3]!=1){Reset();return {};}
            }
            if(frame==next && key==next_key)
                return current==sample?midpoint:std::nullopt;
            bool compatible=frame && next==frame+1 && key==next_key &&
                current.vertices.size()==sample.vertices.size();
            if(compatible)for(unsigned i=0;i<sample.vertices.size();++i) {
                const auto& a=current.vertices[i];const auto& b=sample.vertices[i];
                compatible &= a.uv==b.uv && a.position[2]==b.position[2] &&
                    std::abs(a.position[0]-b.position[0])<=40.f/120 &&
                    std::abs(a.position[1]-b.position[1])<=40.f/200;
            }
            midpoint.reset();
            if(compatible) {
                midpoint.emplace(sample);midpoint->alpha=(current.alpha+sample.alpha)*.5f;
                for(unsigned i=0;i<sample.vertices.size();++i) {
                    auto& v=midpoint->vertices[i];const auto& old=current.vertices[i];
                    for(unsigned c=0;c<2;++c)v.position[c]=(v.position[c]+old.position[c])*.5f;
                    for(unsigned c=0;c<4;++c)v.color[c]=(v.color[c]+old.color[c])*.5f;
                }
            }
            current=sample;key=next_key;frame=next;
            return midpoint;
        }
    };

    using Points=std::array<std::array<float,3>,49>;
    Points previous{};
    std::uint64_t frame{};
    std::uint32_t texture{};
    unsigned generation{};
    std::optional<Points> Apply(std::uint64_t now,std::uint32_t tex,unsigned epoch,const Points& current) {
        bool compatible=frame && now==frame+1 && tex==texture && epoch==generation;
        for(unsigned i=0;i<49;++i)for(unsigned j=0;j<3;++j) {
            if(!std::isfinite(current[i][j])){frame=0;return {};}
            if(j==2 || i==48)compatible &= current[i][j]==previous[i][j];
            else compatible &= std::abs(current[i][j]-previous[i][j])<=40.f;
        }
        Points result=current;
        if(compatible)for(unsigned i=0;i<48;++i)for(unsigned j=0;j<2;++j)
            result[i][j]=(current[i][j]+previous[i][j])*.5f;
        previous=current;frame=now;texture=tex;generation=epoch;
        return compatible?std::optional(result):std::nullopt;
    }
};
}
