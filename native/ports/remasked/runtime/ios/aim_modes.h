// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "three_ds_input.h"
namespace ReMaskedIOS {
using MotionSource=ThreeDsRecomp::Input::MotionSource;
inline MotionSource NormalizeAimMode(MotionSource mode){
    return mode==MotionSource::ControllerGyroscope||mode==MotionSource::ControllerAccelerometer?
        MotionSource::ControllerMotion:mode;
}
inline bool VisibleAimMode(MotionSource mode){
    return mode==MotionSource::Disabled||mode==MotionSource::RightStick||
        mode==MotionSource::ControllerMotion||mode==MotionSource::Automatic;
}
}
