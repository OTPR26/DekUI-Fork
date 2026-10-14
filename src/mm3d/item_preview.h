// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/native_inventory.h"

namespace MM3D::ItemPreview {
using U32=std::uint32_t;
inline std::atomic<float> model_x{0.f},model_y{0.f},model_scale{1.f};
// Restoration + HD HUD v110. Scope to the native Items/Masks page's lifetime.
constexpr std::array<U32,7> PCs{0x5c250c,0x5c27dc,0x111c6c,0x1122e4,0x1122ec,0x5a6b44,0x5a5fc8};
constexpr std::array<U32,7> Words{0xe1a04000,0xe1a04000,0xe1a04000,0xe1a01006,0xe320f000,0xe1a04000,0xe1a05000};
constexpr U32 FirstSVC=0xffffe0;
inline U32 Instruction(U32 pc,U32 word) {
    for(unsigned i=0;i<PCs.size();++i)
        if (pc == PCs[i] && word == Words[i])
            return 0xef000000 | (FirstSVC + i);
    return word;
}
template<class Memory> bool CodeMatches(Memory& m) {
    for(unsigned i=0;i<PCs.size();++i)
        if(m.Read(PCs[i])!=std::optional<U32>(Words[i])) return false;
    // Calls on either side establish the exact scope and matrix-copy sites.
    return m.Read(0x5c2510)==std::optional<U32>(0xebff7d4b)&&
        m.Read(0x5c27e0)==std::optional<U32>(0xebff7c9f)&&
        m.Read(0x1122e0)==std::optional<U32>(0xe5980140)&&
        m.Read(0x1122e8)==std::optional<U32>(0xeb03b1ee)&&
        m.Read(0x5a6b48)==std::optional<U32>(0xe1a06001)&&
        m.Read(0x5a5fcc)==std::optional<U32>(0xe5910004);
}
struct Hook {
    U32 page=0,owner=0,matrix=0;
    bool drawing=false,have_anchor=false;
    bool gear=false;
    float sx=1,dx=0,dy=0;
    std::array<float,3> anchor{};
    std::array<U32,12> saved{};
    template<class Memory> void Enter(Memory& m,U32 candidate,U32 controller) {
        page=owner=0;drawing=false;gear=false;
        if(!NativeInventory::Pointer(candidate,0xc4)||!NativeInventory::Pointer(controller,8)) return;
        const auto layout=m.Read(candidate+0xc0),context=m.Read(controller+4);
        if(!layout||!context||!NativeInventory::Pointer(*context,0x170)||
            (!NativeInventory::LayoutNamed(m,*layout,"MenuItem")&&
             !NativeInventory::LayoutNamed(m,*layout,"MenuMask"))) return;
        page=candidate;owner=*context;
    }
    template<class Memory> void EnterGear(Memory& m,U32 candidate,U32 controller) {
        page=owner=0;drawing=false;gear=false;
        if(!NativeInventory::Pointer(candidate,0x198)||!NativeInventory::Pointer(controller,8)) return;
        const auto layout=m.Read(candidate+0x14),context=m.Read(controller+4);
        if(!layout||!context||!NativeInventory::Pointer(*context,0x170)||
            !NativeInventory::LayoutNamed(m,*layout,"MenuCollect")) return;
        page=candidate;owner=*context;gear=true;
    }
    void Leave(U32 candidate) { if(candidate==page) {page=owner=0;drawing=false;} }
    template<class Memory> bool Description(Memory& m,U32 layout) const {
        if(!page||!owner||!NativeInventory::LayoutNamed(m,layout,"MessageWindow")) return false;
        const auto context=m.Read(owner+0x16c),window_context=m.Read(layout+0x444),id=m.Read(layout+0x3f4);
        if(!context||!*context||!window_context||*window_context!=*context||!id) return false;
        // Item selection uses message 0x1700 + item ID; other dialogue stays independent.
        return *id>=0x1700&&*id<0x1800;
    }
    void ModelFrame(U32 context) {
        drawing=page&&owner==context;have_anchor=false;
        sx=model_scale.load();dx=model_x.load();dy=model_y.load();
        if(!std::isfinite(sx)||!std::isfinite(dx)||!std::isfinite(dy)) drawing=false;
        sx=std::clamp(sx,.25f,2.f);dx=std::clamp(dx,-20.f,20.f);dy=std::clamp(dy,-20.f,20.f);
    }
    template<class Memory> void ModelBegin(Memory& m,U32 context,U32 at) {
        matrix=0;
        if(!drawing||owner!=context||!NativeInventory::Pointer(at,48)) return;
        std::array<float,12> values{};
        for(unsigned i=0;i<12;++i) {
            const auto word=m.Read(at+i*4);if(!word) return;
            saved[i]=*word;values[i]=std::bit_cast<float>(*word);
            if(!std::isfinite(values[i])) return;
        }
        if(!have_anchor) {
            for(unsigned row=0;row<3;++row) anchor[row]=values[row*4+3];
            have_anchor=true;
        }
        // The builder has finished native per-item scaling and rotation. Scale
        // all parts about the first part's native origin, retaining their offsets.
        for(unsigned row=0;row<3;++row) {
            for(unsigned col=0;col<3;++col) values[row*4+col]*=sx;
            values[row*4+3]=anchor[row]+sx*(values[row*4+3]-anchor[row]);
        }
        values[3]+=dx;values[7]-=dy; // model Y is up; GUI Y is down
        matrix=at;
        for(unsigned i=0;i<12;++i) m.Write(at+i*4,std::bit_cast<U32>(values[i]));
    }
    template<class Memory> void ModelEnd(Memory& m) {
        if(matrix) for(unsigned i=0;i<12;++i) m.Write(matrix+i*4,saved[i]);
        matrix=0;
    }
};
}
