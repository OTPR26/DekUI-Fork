// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>

namespace MM3D::TunicColor {
inline std::atomic<int> selected{0}; // Sampled at game launch; never changes during texture decoding.
inline constexpr const char* Names="Green (Kokiri)\0Blue (Zora)\0Red (Goron)\0Purple\0Black\0White\0Gold\0";
inline bool Texture(std::uint64_t hash) {
    return hash==0xF855116F497CFEC2ULL || hash==0x56C21BD06BB9A3C6ULL;
}
inline void Apply(std::uint64_t hash,std::span<std::uint8_t> rgba) {
    const int color=selected.load();
    if(color<3||color>6||!Texture(hash))return;
    for(std::size_t i=0;i+3<rgba.size();i+=4) {
        const int r=rgba[i],g=rgba[i+1],b=rgba[i+2];
        // Only the green cloth: preserve skin, hair, leather, metal, and alpha.
        if(g<=r*1.15f||g<=b*1.15f||g-std::max(r,b)<5)continue;
        int nr=r,ng=g,nb=b;
        switch(color) {
        case 1:nr=r;ng=b;nb=g;break;
        case 2:nr=g;ng=r;nb=b;break;
        case 3:nr=g*0.8f;ng=std::min(r,b);nb=g;break;
        case 4:nr=ng=nb=g/3;break;
        case 5:nr=ng=nb=std::min(255,int(g*1.45f));break;
        case 6:nr=g;ng=g*0.72f;nb=std::min(r,b);break;
        }
        rgba[i]=nr;rgba[i+1]=ng;rgba[i+2]=nb;
    }
}
}
