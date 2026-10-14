// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
namespace MM3D::SongEffectInterpolation {
inline constexpr std::array<unsigned,4> Rows{10,11,14,15};
using Values=std::array<std::array<float,4>,4>;
using Key=std::array<std::uint32_t,7>;
// Captured perspective song layers: vertex count plus complete fixed UV basis.
inline bool Recognized(unsigned vertices,const Values& v) {
    struct Pattern { unsigned vertices; std::array<float,4> basis; };
    constexpr Pattern patterns[]{
        {90,{3,2,6,4}}, {90,{4,1,-8,2}}, // Time
        {90,{6,3,10,-4}}, {288,{2,4,3,6}}, // Healing
        {90,{4,3,-8,-4}}, {288,{1,2,-2,6}}, // Soaring
        {360,{2,4,-5,-8}}, {360,{1,3,3,4}}, // Storms
        {90,{8,1,6,-3}}, {480,{3,1,6,-2}}, // Temple song family
    };
    for(const auto& p:patterns) {
        if(vertices!=p.vertices)continue;
        bool match=true;
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c) {
            const float expected=c==r%2?p.basis[r]:0;
            match &= std::isfinite(v[r][c]) && std::abs(v[r][c]-expected)<1e-5f;
        }
        if(match)return true;
    }
    // Zora Hall waterfall sheets use the same scrolling-UV shader as song
    // effects, but are ordinary room draws rather than song skeleton draws.
    struct WaterPattern { unsigned vertices; std::array<std::array<float,3>,4> basis; };
    constexpr WaterPattern water_patterns[]{
        {276,{{{1,0,0},{0,1,0},{2.9883618355f,.2614479661f,0},{-.2614479661f,2.9883618355f,0}}}},
        {237,{{{1,0,0},{0,1,0},{2.9883618355f,.2614479661f,0},{-.2614479661f,2.9883618355f,0}}}},
        {282,{{{1,0,0},{0,1.5f,0},{.9961205721f,-.0871493220f,0},{.3485972881f,3.9844822884f,0}}}},
        {270,{{{1,0,0},{0,1.5f,0},{.9961205721f,-.0871493220f,0},{.3485972881f,3.9844822884f,0}}}},
        {423,{{{1.5f,0,0},{0,1,0},{2.8975834846f,-.7764056921f,0},{.5176038146f,1.9317222834f,0}}}},
        {549,{{{2,0,0},{0,2,0},{1,0,0},{0,1,0}}}},
    };
    for(const auto& p:water_patterns) {
        if(vertices!=p.vertices)continue;
        bool match=true;
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)
            match &= std::isfinite(v[r][c]) && std::abs(v[r][c]-p.basis[r][c])<1e-5f;
        if(match)return true;
    }
    // Epona and the shared background use a fixed rotated second UV basis.
    for(const auto first:std::array<std::array<float,3>,3>{{{90,1,1},{90,8,4},{144,1,5}}}) {
        if(vertices!=unsigned(first[0]))continue;
        const Values basis{{{first[1],0,0,0},{0,first[2],0,0},
            {4.6982188225f,1.7100105286f,0,0},{-1.7100105286f,4.6982188225f,0,0}}};
        bool match=true;
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<3;++c)
            match &= std::isfinite(v[r][c]) && std::abs(v[r][c]-basis[r][c])<1e-5f;
        if(match)return true;
    }
    return false;
}
class History {
    struct Entry { std::uint64_t frame{}; Values value{}; std::optional<Values> half; };
    std::map<Key,Entry> entries;
public:
    void Reset() { entries.clear(); }
    std::optional<Values> Apply(std::uint64_t frame,const Key& key,const Values& value,bool allow) {
        for(auto it=entries.begin();it!=entries.end();)
            if(it->second.frame>frame || frame-it->second.frame>2)it=entries.erase(it);else ++it;
        for(const auto& row:value)for(float v:row)if(!std::isfinite(v))return {};
        auto it=entries.find(key);
        if(it==entries.end()) {
            if(entries.size()<16)entries.emplace(key,Entry{frame,value,{}});
            return {};
        }
        auto& e=it->second;
        if(e.frame==frame)return allow && e.value==value ? e.half : std::nullopt;
        bool valid=frame==e.frame+1;
        auto half=value;
        for(unsigned row=0;row<4;++row) {
            for(unsigned c=0;c<3;++c)valid &= std::abs(value[row][c]-e.value[row][c])<1e-5f;
            // Captured offsets are unwrapped; jumps/reset/wraps keep the native value.
            valid &= std::abs(value[row][3]-e.value[row][3])<.25f;
            half[row][3]=(value[row][3]+e.value[row][3])*.5f;
        }
        e={frame,value,valid?std::optional<Values>{half}:std::nullopt};
        return allow?e.half:std::nullopt;
    }
};
}
