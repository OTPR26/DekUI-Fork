// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>

namespace MM3D::Fog {
// Presentation-only override; the game's fog registers remain unchanged.
inline std::atomic<bool> disabled{false};
}
