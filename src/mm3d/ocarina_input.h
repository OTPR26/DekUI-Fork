// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
namespace MM3D::OcarinaInput {
inline std::atomic<std::int64_t> list_seen{0};
inline std::int64_t Now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void Observe(bool list){list_seen=list?Now():0;}
struct BackButton {
    bool consumed=false,held=false;
    std::uint32_t Filter(std::uint32_t buttons,bool list) {
        constexpr std::uint32_t B=2,Start=8;
        if(!(buttons&B)){held=consumed=false;return buttons;}
        if(!held&&list)consumed=true;
        held=true;
        if(!consumed)return buttons;
        // Start is the existing Songs toggle. Keep B consumed through the
        // closing animation until release, so one press cannot also Cancel.
        return (buttons&~B)|(list?Start:0);
    }
};
inline BackButton back;
inline void Filter(std::uint32_t& buttons){const auto seen=list_seen.load();buttons=back.Filter(buttons,seen>0&&Now()-seen<250);}
}
