// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace MM3D {
// Queue pointer events for the primary Vulkan presentation overlay.
bool Pointer(float x, float y, int button = -1, bool down = false);
bool Scroll(float x, float y, float horizontal, float vertical);
bool TextInput(const char* text);
void KeyInput(int key, bool down);
inline std::atomic<bool> text_input_active{false};
// Observation only: GSP submissions are not a verified simulation tick counter.
inline std::atomic<std::uint64_t> guest_frames{0};
inline std::atomic<unsigned> inspection_step_count{2};
inline std::atomic<std::uint64_t> host_present_attempts{0};
inline std::atomic<bool> overlay_visible{false};
inline std::atomic<bool> presentation_available{false};
inline void OnGuestFrame() { guest_frames.fetch_add(1, std::memory_order_relaxed); }
inline void ResetGuestFrames() { guest_frames.store(0, std::memory_order_relaxed); }
inline bool ToggleOverlay() {
    if (!presentation_available.load()) return false;
    overlay_visible.store(!overlay_visible.load());
    return true;
}
// Developer builds can preview the exact player UI; shipping has no editor route.
#if !defined(ENABLE_MM3D_GAME_FOLDER) && !defined(__ANDROID__)
inline std::atomic<bool> player_overlay{false};
#endif
inline bool PlayerOverlay() {
#if defined(ENABLE_MM3D_GAME_FOLDER) || defined(__ANDROID__)
    return true;
#else
    return player_overlay.load();
#endif
}
inline bool ToggleOverlayView(bool player) {
    if(!presentation_available.load())return false;
#if !defined(ENABLE_MM3D_GAME_FOLDER) && !defined(__ANDROID__)
    const bool same=player_overlay.exchange(player)==player;
    if(overlay_visible.load()&&!same)return true;
#endif
    return ToggleOverlay();
}
struct Timing {
    std::uint64_t host_frames{};
    double host_fps{}, guest_fps{}, interval_ms{}, max_interval_ms{};
    double previous{}, window_start{};
    std::uint64_t window_host{}, window_guest{};
    bool started{};
    void Present(double seconds, std::uint64_t guest) {
        ++host_frames;
        if (!started || seconds < previous || guest < window_guest) {
            started = true;
            previous = window_start = seconds;
            window_host = host_frames;
            window_guest = guest;
            host_fps = guest_fps = interval_ms = max_interval_ms = 0;
            return;
        }
        interval_ms = (seconds - previous) * 1000;
        if (interval_ms > max_interval_ms) max_interval_ms = interval_ms;
        previous = seconds;
        const double elapsed = seconds - window_start;
        if (elapsed >= 1) {
            host_fps = static_cast<double>(host_frames - window_host) / elapsed;
            guest_fps = static_cast<double>(guest - window_guest) / elapsed;
            window_start = seconds;
            window_host = host_frames;
            window_guest = guest;
        }
    }
};
} // namespace MM3D
