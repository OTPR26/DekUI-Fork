// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>

namespace MM3D::AndroidCast {
enum Mode { Off, SingleScreen, TopScreen };
inline std::atomic_int mode{Off};

struct LowerScreen {
    unsigned x, y, width, height;
};

inline constexpr LowerScreen LowerScreenBounds(unsigned width, unsigned height) {
    const bool portrait = height > width;
    const unsigned available_width = portrait ? width : width * 48 / 100;
    const unsigned available_height = height * (portrait ? 48 : 86) / 100;
    const unsigned picture_width = std::min(available_width, available_height * 4 / 3);
    const unsigned picture_height = picture_width * 3 / 4;
    const unsigned panel_y = portrait ? 0 : (height - available_height) / 2;
    return {(width - picture_width) / 2, panel_y + (available_height - picture_height) / 2,
            picture_width, picture_height};
}
}
