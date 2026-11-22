// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <libretro.h>

// Private core/frontend command. The result is an MM3D::TouchHud::State in
// native 400x240 top-screen coordinates, with a zero diameter for hidden controls.
inline constexpr unsigned RemaskedGetTouchHud = RETRO_ENVIRONMENT_PRIVATE | 1;

inline constexpr unsigned RemaskedGetPictograph = RETRO_ENVIRONMENT_PRIVATE | 3;
inline auto RemaskedPictographTime() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::atomic<std::int64_t> RemaskedPictographRequested{0};

inline constexpr unsigned RemaskedGetPicture = RETRO_ENVIRONMENT_PRIVATE | 4;
inline constexpr float RemaskedNativeAspect = 400.f / 240.f;
struct RemaskedPictureSettings {
    float aspect = RemaskedNativeAspect;
    float leftInset = 0.f;
    float rightInset = 0.f;
};
inline std::atomic<float> RemaskedPictureAspect{RemaskedNativeAspect};
inline std::atomic<float> RemaskedPictureLeftInset{0.f};
inline std::atomic<float> RemaskedPictureRightInset{0.f};
