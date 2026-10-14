// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>

namespace MM3D::CpuInterpolation {
// Published by the settings overlay; read by the emulation/rendering paths.
inline std::atomic<bool> enabled{false};
inline bool Enabled() {
    return enabled.load();
}
}
