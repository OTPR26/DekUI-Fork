// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
namespace MM3D::FileSelectInput {
inline std::atomic<std::int64_t> blocked_until{};
inline std::int64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Refreshed only by a successfully routed FileSelect entrance. Expiry avoids
// retaining a lock if the guest exits, resets or stops submitting that layout.
inline void Observe(bool entering,std::int64_t now=Now()) {blocked_until=entering?now+250:0;}
inline bool Blocked(std::int64_t now=Now()) {return now<blocked_until.load();}
template<class Axis> void Filter(std::uint32_t& buttons,Axis& x,Axis& y,std::int64_t now=Now()) {
    if(Blocked(now)) {buttons=0;x=0;y=0;}
}
}
