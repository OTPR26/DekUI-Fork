// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
namespace MM3D {
struct GlareInterpolation {
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
