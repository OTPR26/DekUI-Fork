// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <limits>
#include <span>
#include <vector>
#include "mm3d/camera_midpoint.h"

namespace MM3D::MothInterpolation {
using CameraMidpoint::Matrix;
using Vertex = std::array<std::array<float,4>,3>; // position, normal, UV
using Key = std::array<std::uint32_t,3>; // two active textures and scene generation

// The Woodfall moth is a fixed six-point, four-triangle wing mesh. Its
// transform animates both flight and wing compression, including zero scale.
inline bool Recognized(std::span<const Vertex> inputs, std::span<const std::uint32_t> order) {
    if(inputs.size()!=6 || order.size()!=12)return false;
    constexpr unsigned corners[]{0,1,2,3,4,0,2,5,0,0,5,3};
    constexpr float positions[6][3]{
        {0,-1.398896f,4.461480f}, {5.605600f,.303818f,4.011920f},
        {3.443792f,.050420f,-3.935964f}, {-3.443792f,.050420f,-3.935964f},
        {-5.605600f,.303818f,4.011920f}, {0,-2.000072f,-4.461480f}};
    constexpr float uv[6][2]{{32767,32767},{0,32767},{0,0},{0,0},{0,32767},{32767,0}};
    const std::array<std::uint32_t,6> indices{order[0],order[1],order[2],order[3],order[4],order[7]};
    for(unsigned k=0;k<6;++k) {
        if(indices[k]>=inputs.size())return false;
        for(unsigned j=0;j<k;++j)if(indices[k]==indices[j])return false;
        const auto& v=inputs[indices[k]];
        for(const auto& row:v)for(float f:row)if(!std::isfinite(f))return false;
        for(unsigned c=0;c<3;++c)
            if(std::abs(v[0][c]-positions[k][c])>.002f || std::abs(v[1][c])>127)return false;
        if(v[0][3]!=1 || v[1][3]!=1 || v[2][2]!=0 || v[2][3]!=1)return false;
        for(unsigned c=0;c<2;++c)if(v[2][c]!=uv[k][c])return false;
    }
    for(unsigned k=0;k<12;++k)if(order[k]!=indices[corners[k]])return false;
    return true;
}

class History {
    struct Entry { Key key; Matrix world; bool used{}; };
    std::vector<Entry> previous,current;
    std::optional<std::uint64_t> last_frame;
public:
    void Reset(){previous.clear();current.clear();last_frame.reset();}
    std::optional<Matrix> Apply(std::uint64_t frame,const Key& key,const Matrix& world) {
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
            if(!std::isfinite(world[r][c]) || std::abs(world[r][c])>(c==3?1e7:2))return {};
        for(unsigned c=0;c<4;++c)if(std::abs(world[3][c]-(c==3?1.:0.))>1e-6)return {};
        if(!last_frame || frame!=*last_frame) {
            if(last_frame && frame>*last_frame && frame-*last_frame==1)previous=std::move(current);
            else previous.clear();
            current.clear();last_frame=frame;
        }
        if(current.size()>=64)return {};
        current.push_back({key,world});
        double best=std::numeric_limits<double>::infinity(),second=best;
        Entry* match=nullptr;
        for(auto& e:previous) {
            if(e.key!=key)continue;
            double distance=0;
            for(unsigned c=0;c<3;++c)distance+=std::pow(world[c][3]-e.world[c][3],2);
            if(distance<best){second=best;best=distance;match=&e;}
            else if(distance<second)second=distance;
        }
        // Captured flight advances at most four world units per guest frame.
        // Keep close crossings, spawns, cuts and missing frames native.
        if(!match || match->used || best>4.25*4.25 || second<=best*1.5+.01)return {};
        match->used=true;
        auto half=world;
        // Do not decompose this into a rigid rotation: signed and zero wing
        // scales are intentional. Average the affine transform directly.
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
            half[r][c]=(world[r][c]+match->world[r][c])*.5;
        return half;
    }
};
}
