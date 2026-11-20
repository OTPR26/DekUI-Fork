// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <SDL.h>
#include <atomic>
namespace ReMaskedMobile {
// iOS publishes its actual scene orientation. On Android SDL reports display
// rotation relative to natural sensor axes, including landscape-native devices.
inline std::atomic<SDL_DisplayOrientation> SceneMotionOrientation{SDL_ORIENTATION_UNKNOWN};
inline SDL_DisplayOrientation MotionOrientation() {
    const auto scene=SceneMotionOrientation.load();
    return scene==SDL_ORIENTATION_UNKNOWN?SDL_GetDisplayOrientation(0):scene;
}
}
