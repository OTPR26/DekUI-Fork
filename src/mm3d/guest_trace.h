// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>

#include "common/logging/log.h"

namespace MM3D::GuestTrace {
inline std::atomic<bool> active{false};
inline std::atomic<std::uint32_t> writes{0};
// Guest code performs many writes per frame, so 20k can expire before the
// Notebook opens. Keep this bounded, but large enough to capture the test.
inline constexpr std::uint32_t Limit = 250000;

// Omit the noisy linear/render-buffer region seen in the first capture.
// Quest/UI objects should be in the application heap below this range.
inline bool IsInterestingAddress(std::uint32_t address) {
    return address >= 0x08000000u && address < 0x14000000u;
}

inline void Start() {
    writes.store(0, std::memory_order_relaxed);
    active.store(true, std::memory_order_release);
    LOG_INFO(Core_ARM11, "MM3D guest write trace started (limit={})", Limit);
}

inline void Stop() {
    active.store(false, std::memory_order_release);
    LOG_INFO(Core_ARM11, "MM3D guest write trace stopped (writes={})",
             writes.load(std::memory_order_relaxed));
}

inline void Observe(std::uint32_t pc, std::uint32_t address, unsigned size,
                    std::uint64_t value) {
    if (!active.load(std::memory_order_acquire))
        return;
    if (!IsInterestingAddress(address))
        return;
    const auto index = writes.fetch_add(1, std::memory_order_relaxed);
    if (index < Limit) {
        LOG_INFO(Core_ARM11,
                 "MM3D guest write #{} pc={:08x} address={:08x} size={} value={:016x}",
                 index, pc, address, size, value);
    } else if (index == Limit) {
        LOG_INFO(Core_ARM11, "MM3D guest write trace limit reached");
        active.store(false, std::memory_order_release);
    }
}
} // namespace MM3D::GuestTrace
