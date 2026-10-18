// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <vector>
#include <optional>
#include <cmath>
#include <cstdint>
#include <limits>
namespace MM3D::RippleInterpolation {
using Point=std::array<float,3>;
using Color=std::array<float,4>;
struct Mesh {std::vector<Point> points;std::vector<Color> colors;};
inline bool Near(float a,float b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<.01f;}
struct Ring {Point center;float radius;};
inline std::optional<Ring> Describe(const Mesh& m,unsigned i) {
    const auto& a=m.points[i];const auto& b=m.points[i+1];const auto& c=m.points[i+4];
    const float radius=(b[0]-a[0])*.5f;
    if(!std::isfinite(radius)||radius<=0||radius>1000)return {};
    const Point center{(a[0]+b[0])*.5f,a[1],(a[2]+c[2])*.5f};
    constexpr int x[]{-1,1,-1,1,1,-1,1,-1},z[]{-1,-1,0,0,1,1,0,0};
    for(unsigned k=0;k<8;++k) {
        for(unsigned axis=0;axis<3;++axis) {
            const float expected=center[axis]+(axis==0?x[k]*radius:axis==2?z[k]*radius:0);
            if(!Near(m.points[i+k][axis],expected))return {};
        }
        for(unsigned axis=0;axis<3;++axis)if(!Near(m.colors[i+k][axis],0))return {};
        if(!Near(m.colors[i+k][3],m.colors[i][3]) || m.colors[i+k][3]<0 || m.colors[i+k][3]>1)return {};
    }
    return Ring{center,radius};
}
class History {
    Mesh previous;
    std::uint64_t frame{};
    std::uint32_t texture{},generation{};
public:
    std::optional<Mesh> Apply(std::uint64_t next,std::uint32_t tex,std::uint32_t gen,const Mesh& mesh) {
        if(mesh.points.size()!=mesh.colors.size()||mesh.points.size()<9||mesh.points.size()>513||(mesh.points.size()-1)%8)return {};
        std::vector<Ring> rings;
        for(unsigned i=0;i+1<mesh.points.size();i+=8){auto r=Describe(mesh,i);if(!r)return {};rings.push_back(*r);}
        auto out=mesh;bool changed=false;
        if(next==frame+1 && tex==texture && gen==generation) {
            std::vector<Ring> old_rings;
            for(unsigned j=0;j+1<previous.points.size();j+=8) {
                const auto r=Describe(previous,j);
                if(!r)return {};
                old_rings.push_back(*r);
            }
            // Repeated impacts can have several concentric rings. Center alone
            // cannot identify them: use their expanding radius and fading alpha,
            // then require a reciprocal, unambiguous match in both directions.
            const auto score=[&](unsigned current,unsigned old)->std::optional<float> {
                for(unsigned c=0;c<3;++c)
                    if(!Near(rings[current].center[c],old_rings[old].center[c]))return {};
                const float growth=rings[current].radius-old_rings[old].radius;
                const float fade=previous.colors[old*8][3]-mesh.colors[current*8][3];
                if(growth<0 || growth>=10 || fade<-.001f)return {};
                return growth*growth+fade*fade*1024;
            };
            const auto best=[&](unsigned index,bool reverse)->std::optional<unsigned> {
                std::optional<unsigned> match;
                float first=std::numeric_limits<float>::infinity(),second=first;
                const unsigned count=reverse?static_cast<unsigned>(rings.size()):static_cast<unsigned>(old_rings.size());
                for(unsigned j=0;j<count;++j) {
                    const auto distance=reverse?score(j,index):score(index,j);
                    if(!distance)continue;
                    if(*distance<first){second=first;first=*distance;match=j;}
                    else if(*distance<second)second=*distance;
                }
                return second>first*1.5f+.01f?match:std::nullopt;
            };
            for(unsigned i=0;i<rings.size();++i) {
                const auto match=best(i,false);
                if(!match || best(*match,true)!=i)continue;
                for(unsigned k=0;k<8;++k) {
                    for(unsigned a=0;a<3;++a)out.points[i*8+k][a]=(mesh.points[i*8+k][a]+previous.points[*match*8+k][a])*.5f;
                    out.colors[i*8+k][3]=(mesh.colors[i*8+k][3]+previous.colors[*match*8+k][3])*.5f;
                }
                changed=true;
            }
        }
        previous=mesh;frame=next;texture=tex;generation=gen;
        return changed?std::optional<Mesh>{std::move(out)}:std::nullopt;
    }
};
}
