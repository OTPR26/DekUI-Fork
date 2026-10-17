// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace MM3D::AndroidMenu {
enum Action { Bind=1, CancelBind, TouchVisible, TouchEdit, TouchButtons, TouchOpacity,
              TouchScale, Haptics, SwapScreens, LoadAmiibo, RemoveAmiibo, Textures, ClearBind, ClearTextures };
inline std::atomic<int> pending{0};
inline void Request(Action action, unsigned value=0) {
    int empty=0;
    pending.compare_exchange_strong(empty, (int(action)<<16) | int(value&65535));
}
struct State {
    std::vector<std::string> bindings;
    std::string textures, status;
    int capture=-1, opacity=50, scale=50;
    bool touch=false, haptics=true;
};
inline std::mutex mutex;
inline State state;
inline State Snapshot() { std::scoped_lock lock{mutex}; return state; }
inline constexpr const char* BindingLabels[]{"A","B","X","Y","Select","Start","Home",
    "L","R","ZL","ZR","D-pad up","D-pad down","D-pad left","D-pad right",
    "Left stick vertical","Left stick horizontal","Right stick vertical","Right stick horizontal",
    "D-pad vertical axis","D-pad horizontal axis"};
}
