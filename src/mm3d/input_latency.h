// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include "common/logging/log.h"

namespace MM3D::InputLatency {
using U64=std::uint64_t;
inline std::atomic<U64> deadline{},session{},keys{},hid{},frames{};
inline U64 Now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool Active() {
    const auto until=deadline.load(std::memory_order_relaxed);
    return until&&Now()<until;
}
inline void Start() {
    const auto id=++session;
    deadline.store(Now()+30000000);
    LOG_INFO(Core_ARM11,"MM3D input timing start session={} duration_s=30; timestamps are steady-clock us, presentation is not scanout",id);
}
inline void Key(int key,bool pressed) {
    if(!Active())return;
    const auto t=Now(),id=++keys;
    LOG_INFO(Core_ARM11,"MM3D input timing key session={} key_id={} t={} code={} pressed={}",session.load(),id,t,key,pressed);
}
inline void Hid(std::uint32_t buttons,std::uint32_t changed) {
    if(!(changed&0xfff)||!Active())return;
    const auto t=Now(),id=++hid;
    LOG_INFO(Core_ARM11,"MM3D input timing hid session={} hid_id={} t={} latest_key={} buttons={:08x} changed={:08x}",session.load(),id,t,keys.load(),buttons,changed);
}
struct Frame {
    U64 session{},id{},begin{},key{},hid{},queued{};
    U64 pacing_arrival{},pacing_deadline{};
    bool paired{};
};
inline Frame BeginFrame() {
    if(!Active())return {};
    return {session.load(),++frames,Now(),keys.load(),hid.load(),0};
}
inline void Presented(const Frame& f,U64 copy,U64 acquired,U64 returned,bool repeat) {
    if(!f.id||repeat||!Active()||f.session!=session.load())return;
    LOG_INFO(Render_Vulkan,"MM3D input timing frame session={} frame={} begin={} queued={} copy={} acquired={} returned={} key_at_begin={} hid_at_begin={} pacing_arrival={} pacing_deadline={} paired={}",
        f.session,f.id,f.begin,f.queued,copy,acquired,returned,f.key,f.hid,f.pacing_arrival,f.pacing_deadline,f.paired);
}
}
