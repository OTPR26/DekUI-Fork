// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Ocarina ReTuned's duo_game_display and screen_mode_policy.
#pragma once
#include "remasked_dual_screen.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>
namespace ReMaskedDuo {
// A placeholder UIHinge with Unknown status also exists on ordinary iPhones.
// Require positive state/geometry evidence; retain proved capability across a
// temporary unknown update while the interaction remains attached.
inline bool HingeCapability(bool phone,bool object,int status,bool division,bool previouslyKnown=false){
    return phone && (division || (object && (previouslyKnown || (status>=1 && status<=3))));
}
struct Rect {
    float X=0,Y=0,Width=0,Height=0;
    bool Valid() const {return std::isfinite(X)&&std::isfinite(Y)&&std::isfinite(Width)&&std::isfinite(Height)&&Width>0&&Height>0&&std::isfinite(X+Width)&&std::isfinite(Y+Height);}
    bool Contains(float x,float y) const {return Valid()&&x>=X&&y>=Y&&x<X+Width&&y<Y+Height;}
};
struct Geometry {bool Available=false,Closed=true;Rect Bounds,Safe,Division;std::vector<Rect> Occlusions;};
struct Layout {Rect Top,Lower;bool Dual() const{return Top.Valid()&&Lower.Valid();}};
struct Touch {unsigned X,Y;};
inline Rect Fit(Rect r,float aspect) {
    if(!r.Valid())return {};
    const float w=std::min(r.Width,r.Height*aspect),h=w/aspect;
    return {r.X+(r.Width-w)/2,r.Y+(r.Height-h)/2,w,h};
}
inline Layout Divide(const Geometry& g) {
    const auto s=g.Safe,d=g.Division;
    if(!g.Available||g.Closed||!g.Bounds.Valid()||!s.Valid()||!std::isfinite(d.X)||!std::isfinite(d.Y)||!std::isfinite(d.Width)||!std::isfinite(d.Height)||d.Width<0||d.Height<0||(d.Width==0&&d.Height==0))return {};
    const float right=s.X+s.Width,bottom=s.Y+s.Height;
    if(s.X<g.Bounds.X||s.Y<g.Bounds.Y||right>g.Bounds.X+g.Bounds.Width+.5f||bottom>g.Bounds.Y+g.Bounds.Height+.5f)return {};
    if(d.X<=s.X+.5f&&d.X+d.Width>=right-.5f&&d.Y>s.Y&&d.Y+d.Height<bottom)
        return {Fit({s.X,s.Y,s.Width,d.Y-s.Y},400.f/240),Fit({s.X,d.Y+d.Height,s.Width,bottom-d.Y-d.Height},320.f/240)};
    if(d.Y<=s.Y+.5f&&d.Y+d.Height>=bottom-.5f&&d.X>s.X&&d.X+d.Width<right)
        return {Fit({s.X,s.Y,d.X-s.X,s.Height},400.f/240),Fit({d.X+d.Width,s.Y,right-d.X-d.Width,s.Height},320.f/240)};
    return {};
}
class Display {
    mutable std::mutex Mutex;Geometry Current;
public:
    void Publish(Geometry g) {
        std::lock_guard lock(Mutex);Current=std::move(g);
        const auto layout=Divide(Current);
        ReMaskedDual::Available=layout.Dual();
        ReMaskedDual::Width=layout.Dual()?unsigned(layout.Lower.Width):0;
        ReMaskedDual::Height=layout.Dual()?unsigned(layout.Lower.Height):0;
        // Never overwrite the user's requested mode on close/rotation/fallback.
        if(!layout.Dual())ReMaskedDual::Touch=0;
    }
    bool Available() const {std::lock_guard lock(Mutex);return Current.Available;}
    Layout Pixels(float width,float height,bool dual) const {
        std::lock_guard lock(Mutex);
        if(!Current.Bounds.Valid()||!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0)return {};
        auto layout=dual?Divide(Current):Layout{Current.Safe,{}};
        const auto scale=[&](Rect r){if(!r.Valid())return Rect{};return Rect{(r.X-Current.Bounds.X)*width/Current.Bounds.Width,(r.Y-Current.Bounds.Y)*height/Current.Bounds.Height,r.Width*width/Current.Bounds.Width,r.Height*height/Current.Bounds.Height};};
        return {scale(layout.Top),scale(layout.Lower)};
    }
    std::optional<Touch> MapTouch(float x,float y,bool dual) const {
        std::lock_guard lock(Mutex);
        if(!dual||!std::isfinite(x)||!std::isfinite(y))return {};
        auto layout=Divide(Current);if(!layout.Lower.Contains(x,y))return {};
        for(const auto& region:Current.Occlusions)if(region.Contains(x,y))return {};
        return Touch{std::min(319u,unsigned((x-layout.Lower.X)*320/layout.Lower.Width)),std::min(239u,unsigned((y-layout.Lower.Y)*240/layout.Lower.Height))};
    }
};
inline Display GameDisplay;
}
