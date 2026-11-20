// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "fast/extension/remasked_duo_layout.h"
namespace ReMaskedIOS {
struct PortraitLayout {
    ReMaskedDuo::Rect Game,Controls;
    bool Active() const { return Game.Valid() && Controls.Valid(); }
};
// UIKit points, independent of drawable density. Only ordinary phones opt in;
// a hinge-capable phone keeps the separate Single/Dual display policy.
inline PortraitLayout ResolvePortraitLayout(ReMaskedDuo::Rect bounds,
        ReMaskedDuo::Rect safe,bool ordinaryPhone,float aspect=16.f/9.f) {
    if(!ordinaryPhone || !bounds.Valid() || !safe.Valid() ||
       bounds.Height<=bounds.Width || !std::isfinite(aspect) || aspect<=0) return {};
    const float bottom=safe.Y+safe.Height;
    if(safe.X<bounds.X || safe.Y<bounds.Y ||
       safe.X+safe.Width>bounds.X+bounds.Width+.5f || bottom>bounds.Y+bounds.Height+.5f) return {};
    constexpr float margin=0,gap=20,topClearance=16;
    const float width=safe.Width-2*margin;
    if(width<=0) return {};
    const float gameHeight=width/aspect;
    ReMaskedDuo::Rect game{safe.X+margin,safe.Y+topClearance,width,gameHeight};
    const float available=bottom-(game.Y+game.Height+gap);
    if(available<180) return {}; // compact windows retain the overlay layout
    const float controlsHeight=available;
    return {game,{safe.X,bottom-controlsHeight,safe.Width,controlsHeight}};
}
struct PortraitControlRows {float ThumbCenter,SystemTop;};
inline PortraitControlRows ResolvePortraitControlRows(float height,float scale){
    const float systemTop=height-16.f*scale-30.f*scale;
    // The diamond extends 66 units below its centre. Reserve a real gap to
    // the system row, instead of sharing the landscape bottom offsets.
    return {std::min(height*.60f,systemTop-(32.f+66.f)*scale),systemTop};
}
class PortraitDisplay {
    mutable std::mutex Mutex;
    ReMaskedDuo::Rect Bounds,Safe;
    bool Enabled=false;
public:
    void Publish(ReMaskedDuo::Rect bounds,ReMaskedDuo::Rect safe,bool enabled) {
        std::lock_guard lock(Mutex);Bounds=bounds;Safe=safe;Enabled=enabled;
    }
    PortraitLayout Pixels(float width,float height) const {
        std::lock_guard lock(Mutex);
        auto layout=ResolvePortraitLayout(Bounds,Safe,Enabled);
        if(!layout.Active() || !std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0)return {};
        const auto scale=[&](ReMaskedDuo::Rect p){return ReMaskedDuo::Rect{
            (p.X-Bounds.X)*width/Bounds.Width,(p.Y-Bounds.Y)*height/Bounds.Height,
            p.Width*width/Bounds.Width,p.Height*height/Bounds.Height};};
        return {scale(layout.Game),scale(layout.Controls)};
    }
};
inline PortraitDisplay PhonePortrait;

}
