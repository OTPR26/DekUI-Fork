// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
namespace AudioCore::HLE {
// Latched by each voice when it starts a buffer, never halfway through decoding.
inline std::atomic<bool> accurate_audio{true};
}
