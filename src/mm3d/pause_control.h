// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdint>

namespace MM3D {
// Inspection time excludes pauses, including time spent stepping frames.
class PauseClock {
public:
    void SetPaused(bool value, std::int64_t now) {
        if (value == paused) return;
        if (value) started = now;
        else excluded += now - started;
        paused = value;
    }
    std::int64_t Now(std::int64_t now) const {
        return (paused ? started : now) - excluded;
    }
private:
    bool paused{};
    std::int64_t started{}, excluded{};
};
inline std::atomic<bool> inspection_paused{false}, step_requested{false};
inline std::mutex pause_mutex;
inline PauseClock pause_clock;
inline std::int64_t WallMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void SetInspectionPaused(bool paused) {
    std::scoped_lock lock{pause_mutex};
    pause_clock.SetPaused(paused, WallMilliseconds());
    inspection_paused.store(paused);
    if (!paused) step_requested.store(false);
}
inline std::int64_t InspectionMilliseconds() {
    std::scoped_lock lock{pause_mutex};
    return pause_clock.Now(WallMilliseconds());
}
} // namespace MM3D
