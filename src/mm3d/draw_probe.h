// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
namespace MM3D {
// Temporary visual identification only. This signature is NOT a verified logo identity.
struct DrawProbe {
    std::atomic<int> selection{-1};
    std::atomic<std::int64_t> expires{0};
    std::atomic<unsigned> hidden{0};
    std::uint64_t last_frame = UINT64_MAX;
    unsigned occurrences[2]{}; // render thread only
    std::atomic<bool> scissor_probe{false};
    std::atomic<std::uint64_t> scissor_frame{0};
    std::atomic<std::uint64_t> scissor_shader{0};
    std::atomic<unsigned> scissor_vertices{0};
    std::atomic<int> scissor_left{0}, scissor_top{0}, scissor_right{0}, scissor_bottom{0};
    static std::int64_t Now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Arm(int candidate) {
        hidden.store(0);
        expires.store(Now() + 5000);
        selection.store(candidate);
    }
    bool Hide(std::uint64_t frame, std::uint64_t shader, unsigned vertices, bool accelerated) {
        if (frame != last_frame) {
            last_frame = frame; occurrences[0] = occurrences[1] = 0;
        }
        if (!accelerated || shader != 0x9b343fea686d98e0ULL || (vertices != 6 && vertices != 12)) return false;
        const unsigned group = vertices == 6 ? 0 : 1;
        const unsigned occurrence = occurrences[group]++;
        const int candidate = selection.load();
        if (candidate < 0 || candidate > 3 || Now() >= expires.load()) return false;
        if (group != unsigned(candidate / 2) || occurrence != unsigned(candidate % 2)) return false;
        hidden.fetch_add(1);
        return true;
    }
    void ObserveScissor(std::uint64_t frame, std::uint64_t shader, unsigned vertices,
                        int left, int top, int right, int bottom) {
        if (!scissor_probe.load(std::memory_order_relaxed)) return;
        scissor_frame.store(frame, std::memory_order_relaxed);
        scissor_shader.store(shader, std::memory_order_relaxed);
        scissor_vertices.store(vertices, std::memory_order_relaxed);
        scissor_left.store(left, std::memory_order_relaxed);
        scissor_top.store(top, std::memory_order_relaxed);
        scissor_right.store(right, std::memory_order_relaxed);
        scissor_bottom.store(bottom, std::memory_order_relaxed);
    }
};
inline DrawProbe draw_probe;
}
