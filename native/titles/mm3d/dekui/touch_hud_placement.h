// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "native_inventory.h"
namespace Mm3dDekui::TouchHud {
struct Placement {std::atomic<bool> active{false};std::atomic<float> scale{1},x{0},y{0};};
struct Target {std::atomic<bool> active{false};std::atomic<float> x{0},y{0},half{0};};
inline std::array<Target,6> targets;
inline std::atomic<float> icon_opacity{1.f};
inline std::atomic<bool> external_artwork{false},external_failed{false};
inline std::atomic<float> external_canvas_width{240.f*16.f/9.f},external_content_scale{1.f};
inline std::array<std::atomic<float>,6> icon_scales{1,1,1,1,1,1};
// ReTuned's default items are 12 native HUD units; the A verb is 9 high,
// capped at 48 wide. Resize artwork independently of the touch hit target.
inline float ContentScale(unsigned slot,float width,float height,float individual){
    if(!(width>0)||!(height>0)||!std::isfinite(individual))return 1;
    individual=std::clamp(individual,.5f,1.5f)*(external_artwork.load()?external_content_scale.load():1.f);
    return slot==0?std::min(9.f*individual/height,48.f*individual/width):12.f*individual/std::max(width,height);
}
inline Placement dpad;
inline std::atomic<bool> requested{false};
// Observed from the complete Hud2 D-pad group, including masks and ocarina,
// rather than the optional centre-cross texture (which skins may replace).
struct DpadBounds {
    std::atomic<bool> known{false};
    std::atomic<float> x{0},y{0},radius{0},width{0},height{0};
    void Observe(float px,float py,float r) {
        if(!std::isfinite(px)||!std::isfinite(py)||!std::isfinite(r)||r<=0)return;
        x=px;y=py;radius=r;width=2*r;height=2*r;known=true;
    }
    void ObserveRect(float px,float py,float w,float h){
        if(!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0)return;
        Observe(px,py,.5f*std::max(w,h));width=w;height=h;
    }
};
inline DpadBounds dpad_bounds;
inline std::array<DpadBounds,6> button_bounds;
// Bounds come only from panes that actually reached native draw emission.
// Layout matrices and invisible children are not valid item measurements.
inline NativeInventory::Hook::Affine Compose(const Placement& p,const NativeInventory::Hook::Affine& d){
    if(!p.active.load())return d;
    const float s=p.scale.load(),x=p.x.load(),y=p.y.load();
    if(!std::isfinite(s)||!std::isfinite(x)||!std::isfinite(y)||!(s>0))return d;
    return {s*d.xx,s*d.xy,s*d.yx,s*d.yy,s*d.tx+x,s*d.ty+y};
}
}
