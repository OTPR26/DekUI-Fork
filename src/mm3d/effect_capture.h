// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
namespace MM3D::EffectCapture {
struct Buffer {
    unsigned stride{},first{},last{};
    std::vector<unsigned> components;
    std::vector<std::uint8_t> bytes;
};
struct Draw {
    std::uint64_t frame{},shader{},swizzle{};
    std::int64_t time{};
    std::uint32_t vertices{},base{},offset{};
    bool indexed{},accelerated{};
    std::array<std::array<unsigned,3>,12> formats{};
    std::vector<Buffer> buffers;
    // Resolved shader inputs in cache-miss order, plus the submitted vertex order.
    // CPU draws cannot use the accelerated path's vertex-buffer/index metadata.
    std::vector<std::array<std::array<float,4>,16>> cpu_inputs;
    std::vector<std::uint32_t> cpu_order;
    std::array<unsigned,16> cpu_input_registers{};
    unsigned cpu_topology{};
    std::array<std::uint32_t,3> textures{};
    std::array<unsigned,7> state{};
    std::array<std::array<float,4>,96> uniforms{};
};
inline std::mutex mutex;
inline std::atomic<bool> active{};
inline std::atomic<unsigned> count{};
inline std::int64_t begin{},end{};
inline bool truncated{};
inline bool geometry_truncated{};
inline std::size_t geometry_bytes{};
inline std::vector<Draw> draws;
inline std::int64_t Now() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void Start() {
    std::scoped_lock lock(mutex);active=false;draws.clear();count=0;truncated=false;geometry_truncated=false;geometry_bytes=0;
    begin=Now()+3000;end=begin+20000;active=true;
}
inline void Poll() {std::scoped_lock lock(mutex);if(active && Now()>=end)active=false;}
inline bool Recording(std::uint64_t frame) {
    if(!active)return false;
    std::scoped_lock lock(mutex);
    const auto now=Now();return active && now>=begin && now<end && frame%10<3;
}
inline void Observe(Draw draw) {
    std::scoped_lock lock(mutex);
    if(!active || Now()<begin || Now()>=end)return;
    if(draws.size()>=16000){truncated=true;active=false;return;}
    std::size_t bytes=0;for(const auto& b:draw.buffers)bytes+=b.bytes.size();
    bytes+=draw.cpu_inputs.size()*sizeof(draw.cpu_inputs.front())+
        draw.cpu_order.size()*sizeof(std::uint32_t);
    if(geometry_bytes+bytes>16*1024*1024){
        draw.buffers.clear();draw.cpu_inputs.clear();draw.cpu_order.clear();geometry_truncated=true;
    }
    else geometry_bytes+=bytes;
    draw.time=Now()-begin;draws.push_back(std::move(draw));count=unsigned(draws.size());
}
bool Export(const std::string& path);
}
