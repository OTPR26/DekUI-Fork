// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <mutex>
#include "mm3d/widescreen.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
namespace MM3D::TouchHud {
// Guest top-screen coordinates, supplied by the Android viewport bridge.
struct Anchor {float x=0,y=0,diameter=0;bool Valid()const{return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(diameter)&&diameter>0&&diameter<240;}};
struct State {bool enabled=false;std::array<Anchor,10> anchors;}; // A B X Y L R D-pad up/down/left/right
inline std::mutex mutex;
inline State current;
inline void Trace(unsigned slot,const char* stage,const Anchor& target,float a=0,float b=0,float c=0,float d=0){
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"ReMaskedTouchHud","slot=%u %s target=%.2f,%.2f size=%.2f data=%.2f,%.2f,%.2f,%.2f",slot,stage,target.x,target.y,target.diameter,a,b,c,d);
#endif
}
inline void Publish(const State& next){std::scoped_lock lock(mutex);current=next;}
inline State Snapshot(){std::scoped_lock lock(mutex);return current;}
inline Anchor Project(float x,float y,float diameter,float windowWidth,float windowHeight,
                      float left,float top,float width,float height,bool widescreen){
    if(width<=0||height<=0||windowWidth<=0||windowHeight<=0)return {};
    // Undo the viewport mapping and the renderer's centered clip compression
    // so the game artwork lands on the Android overlay's button centers.
    constexpr float nativeWidth=400.f;
    const float horizontalScale=widescreen ? Widescreen::HorizontalScale : 1.f;
    return {200.f+((x*windowWidth-left)/width-.5f)*nativeWidth/horizontalScale,
        (y*windowHeight-top)/height*240.f,diameter*windowWidth/height*240.f};
}
}
