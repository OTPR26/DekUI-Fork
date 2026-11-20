// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace ReMasked {
// A dispatch warning can arrive between render-loop samples. Keep its lower
// cache limit until 30 seconds after the most recent warning, never promoting
// a critical limit back to the ordinary cache size on the next frame.
class AppleMemoryPolicy {
    std::atomic<uint64_t> warningUntil{0}, criticalUntil{0};
public:
    void Warn(uint64_t nowMs, bool critical) {
        auto& deadline = critical ? criticalUntil : warningUntil;
        uint64_t old = deadline.load();
        const uint64_t next = nowMs + 30000;
        while (old < next && !deadline.compare_exchange_weak(old, next)) {}
    }
    uint64_t DecodedMiB(uint64_t nowMs, uint64_t footprintMiB,
                        uint64_t headroomMiB, bool phone) const {
        if (nowMs < criticalUntil.load() || headroomMiB < 256) return 16;
        if (nowMs < warningUntil.load() || footprintMiB > (phone ? 1536U : 2048U) ||
            headroomMiB < 768) return 32;
        return phone ? 96 : 192;
    }
};
}
