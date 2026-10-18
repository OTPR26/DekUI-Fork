// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace MM3D::WidescreenProbe {
struct Draw {
    std::uint64_t shader{},swizzle{};
    unsigned vertices{},width{},height{},textures{},blend{},depth{};
    bool operator==(const Draw&) const = default;
};
struct Candidate {
    Draw draw;
    unsigned observations{};
    std::array<int,4> scissor{};
};
// A saved fullscreen rule identifies draw state, not the size of its geometry.
// The layout shader also uses that state for small solid UI panes (including
// both Schedule portrait flashes). Keep those in the same space as their icons.
inline bool LocalLayoutPane(std::uint64_t shader,std::uint64_t swizzle,unsigned vertices,
    unsigned textures,const std::array<float,4>& control,const std::array<float,4>& x,
    const std::array<float,4>& y) {
    if(shader!=0x9b343fea686d98e0ULL || swizzle!=0xac22b79e29209c01ULL ||
       vertices!=6 || textures!=0 || (control[0]!=0 && control[0]!=1) ||
       control[1]!=3 || control[2]!=0 || control[3]!=1)return false;
    for(const auto& row:{x,y})for(float v:row)if(!std::isfinite(v))return false;
    return x[1]==0 && x[2]==0 && y[1]==0 && y[3]==0 &&
        x[3]>0 && x[3]<400 && y[2]>0 && y[2]<240;
}
// MM3D transition/title overlay, recovered from the user's tested capture.
// Observe is called only for widescreen top-screen draws without depth writes.
inline constexpr Draw TransitionOverlay{0x89b1ab98da3d9241ULL,0x6f0b398b1fc186a9ULL,
    4,480,400,0,65654,3856};
inline std::mutex mutex;
inline std::vector<Candidate> candidates;
inline std::vector<Draw> saved_draws;
inline int selected=-1;
inline unsigned bypassed=0;
inline std::int64_t capture_until=0,test_until=0;
inline auto Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void Capture() {
    std::scoped_lock lock(mutex);
    candidates.clear(); selected=-1; bypassed=0; test_until=0; capture_until=Now()+30000;
}
inline void Stop() {
    std::scoped_lock lock(mutex);
    capture_until=test_until=0; selected=-1;
}
inline bool Observe(const Draw& draw,const std::array<int,4>& scissor) {
    std::scoped_lock lock(mutex);
    const auto now=Now();
    if(now<capture_until) {
        auto it=std::find_if(candidates.begin(),candidates.end(),[&](const auto& c){return c.draw==draw;});
        if(it!=candidates.end()) { ++it->observations; it->scissor=scissor; }
        else if(candidates.size()<128) candidates.push_back({draw,1,scissor});
    }
    const bool bypass=draw==TransitionOverlay || (std::find(saved_draws.begin(),saved_draws.end(),draw)!=saved_draws.end()) ||
        (now<test_until && selected>=0 && selected<int(candidates.size()) &&
        candidates[selected].draw==draw);
    if(bypass) ++bypassed;
    return bypass;
}
}
