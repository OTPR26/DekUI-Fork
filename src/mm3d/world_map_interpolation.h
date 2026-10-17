// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/camera_midpoint.h"

namespace MM3D::WorldMapInterpolation {
using CameraMidpoint::Matrix;
// Projection sanity is deliberately independent of the map's far plane: that
// plane changes with map contents. Projection alone never grants ownership.
inline bool Projection(const Matrix& p) {
    if(!CameraMidpoint::WorldProjection(p) || p[0][1]<1 || p[0][1]>16 ||
       std::abs(p[1][0]+.6*p[0][1])>1e-5)return false;
    for(unsigned r=0;r<2;++r)for(unsigned c=0;c<4;++c)
        if(c!=1-r && p[r][c]!=0)return false;
    return p[2][2]>1 && p[2][2]<1.01 &&
        std::abs(p[2][3]-5*p[2][2])<1e-6 && p[2][0]==0 && p[2][1]==0;
}
inline bool MapModel(const Matrix& camera,const Matrix& model_view) {
    if(!CameraMidpoint::Rigid(camera))return false;
    auto expected=CameraMidpoint::Identity();
    for(unsigned i=0;i<3;++i)expected[i][i]=.04;
    const auto model=CameraMidpoint::Multiply(camera,model_view);
    // Float matrix cancellation needs a larger tolerance for translation than
    // for scale. Every sampled map frame begins with this common landscape.
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
        if(!std::isfinite(model[r][c]) ||
           std::abs(model[r][c]-expected[r][c])>(c==3?.002:1e-5))return false;
    return true;
}
class History {
    CameraMidpoint::History camera_history;
    std::uint32_t owner{},generation{};
    std::uint64_t recognized_frame{};
    Matrix recognized_camera{},recognized_projection{};
    bool recognized{};
public:
    void Reset() {camera_history.Reset();owner=0;generation=0;recognized=false;}
    // Seed each frame from the map model, then admit its markers/materials
    // only with the same camera and lens. Frozen gameplay cannot consume the
    // map's history merely because UpWorldMapHud is also being submitted.
    bool Recognize(std::uint64_t frame,std::uint32_t next_owner,
            std::uint32_t next_generation,const Matrix& camera,
            const Matrix& projection,const Matrix& model_view) {
        if(!next_owner) {Reset();return false;}
        if(owner!=next_owner || generation!=next_generation)Reset();
        owner=next_owner;generation=next_generation;
        if(recognized_frame!=frame)recognized=false;
        if(!Projection(projection) || !CameraMidpoint::Rigid(camera))return false;
        if(recognized)return CameraMidpoint::Near(camera,recognized_camera,.001) &&
            CameraMidpoint::Near(projection,recognized_projection,1e-6);
        if(!MapModel(camera,model_view))return false;
        recognized=true;recognized_frame=frame;
        recognized_camera=camera;recognized_projection=projection;
        return true;
    }
    std::optional<Matrix> Apply(std::uint64_t frame,std::int64_t now,
            std::uint32_t next_owner,std::uint32_t next_generation,
            const Matrix& camera,const Matrix& projection,const Matrix& model_view,
            bool comparison=false) {
        if(!Recognize(frame,next_owner,next_generation,camera,projection,model_view))return {};
        const auto half=camera_history.Observe(frame,now,camera,projection);
        return comparison?std::nullopt:half;
    }
};
}
