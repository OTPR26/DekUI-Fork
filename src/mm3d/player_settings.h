// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
namespace MM3D::PlayerSettings {
// Render thread requests; the frontend applies/saves through its settings API.
inline std::atomic<int> resolution{4}, volume{100}, vsync{1}, cache{1};
inline std::atomic<int> set_resolution{-1}, set_volume{-1}, set_vsync{-1}, set_cache{-1};
inline std::atomic<int> hardware_shaders{0}, set_hardware_shaders{-1};
inline std::atomic<int> async_shaders{0}, set_async_shaders{-1};
inline std::atomic<int> textures{0}, set_textures{-1};
inline std::atomic<bool> open_textures{false};
inline std::atomic<bool> open_cheats{false};
// Frontend signals after the shader/loading screen has faded away.
inline std::atomic<bool> startup_hint_requested{false};
// Native CFG language IDs; requests are applied on the emulation thread.
inline std::atomic<int> game_language{1}, set_game_language{-1};
inline std::atomic<int> game_language_status{0}; // 0 idle, 1 pending, 2 saved, 3 failed
inline constexpr bool SupportedLanguage(int value){return value==1||value==2||value==5;}
inline std::atomic<bool> fullscreen_shortcut{false}, textures_shortcut{false};
inline std::atomic<bool> controls{false}, fullscreen{false}, quit{false}, about{false};
}
