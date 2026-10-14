// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace MM3D::InterpolationTimeline {
using U64 = std::uint64_t;

// CPU timestamps use steady_clock nanoseconds. GPU durations have a separate
// clock domain and must never be subtracted from these timestamps.
struct Submission {
    U64 session{}, tick{}, requested{}, owner{}, address{}, bytes{}, pc{};
    const char* reason{};
    const char* file{};
    unsigned line{};
    std::atomic<U64> worker{}, locked{}, submitted{}, completed{}, gpu_ns{};
    std::atomic<bool> gpu_available{};
};

struct Present {
    U64 session{}, owner{}, started{}, paced{}, acquired{}, lock_requested{};
    U64 locked{}, submitted{}, returned{}, unlocked{};
    bool shared_queue{};
};

struct Breakdown {
    U64 total{}, before_worker{}, queue_lock{}, submit{}, after_submit{}, overlap{};
    bool valid{};
};

inline Breakdown Measure(const Submission& s) {
    const auto worker = s.worker.load();
    const auto locked = s.locked.load();
    const auto submitted = s.submitted.load();
    const auto completed = s.completed.load();
    if (!s.requested || worker < s.requested || locked < worker || submitted < locked ||
        !submitted || completed < locked || !completed) {
        return {};
    }
    // GPU completion can be observed before vkQueueSubmit returns on the worker.
    // Retain that sample and report the overlap instead of unsigned underflow.
    return {completed - s.requested, worker - s.requested, locked - worker,
            submitted - locked, completed > submitted ? completed - submitted : 0,
            submitted > completed ? submitted - completed : 0, true};
}

inline U64 Overlap(U64 a, U64 b, U64 c, U64 d) {
    const auto start = std::max(a, c);
    const auto finish = std::min(b, d);
    return finish > start ? finish - start : 0;
}

inline U64 PresentLockOverlap(const Submission& s, const Present& p) {
    if (s.session != p.session || s.owner != p.owner || !p.shared_queue) {
        return 0;
    }
    return Overlap(s.worker.load(), s.locked.load(), p.locked, p.unlocked);
}
} // namespace MM3D::InterpolationTimeline
