// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
namespace MM3D::AudioCapture {
// Diagnostic only. No audio/state mutation or disk I/O on the DSP thread.
// Little-endian records: kind, source, payload bytes, elapsed microseconds
// (four u32s), followed by payload. Kind 1: pre-ParseConfig configuration
// + 16 s16 ADPCM coefficients; 2: filtered stereo PCM16 frame;
// 3: decoded stereo PCM16 buffer prefix (at most 65536 samples).
inline std::atomic<bool> active{false};
inline std::mutex mutex;
inline std::vector<std::uint8_t> bytes;
inline std::chrono::steady_clock::time_point start;
inline bool truncated=false;
constexpr std::size_t Limit=128*1024*1024;
inline void Start(){
    std::scoped_lock lock(mutex);
    bytes.clear();bytes.reserve(Limit);truncated=false;
    const char magic[16]="MM3D_AUDIO_V1";
    bytes.insert(bytes.end(),magic,magic+16);
    start=std::chrono::steady_clock::now();active=true;
}
inline bool Running(){
    if(!active.load())return false;
    std::scoped_lock lock(mutex);
    if(std::chrono::steady_clock::now()-start>=std::chrono::seconds(10))active=false;
    return active.load();
}
inline void Record(std::uint32_t kind,std::uint32_t source,const void* data,std::size_t size,
                   const void* extra=nullptr,std::size_t extra_size=0){
    if(!active.load())return;
    std::scoped_lock lock(mutex);
    if(!active.load())return;
    const auto elapsed=std::chrono::steady_clock::now()-start;
    if(elapsed>=std::chrono::seconds(10)){active=false;return;}
    if(size+extra_size+16>Limit-bytes.size()){truncated=true;active=false;return;}
    const std::uint32_t header[]{kind,source,static_cast<std::uint32_t>(size+extra_size),
        static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count())};
    const auto append=[&](const void* p,std::size_t n){if(!n)return;const auto* b=static_cast<const std::uint8_t*>(p);bytes.insert(bytes.end(),b,b+n);};
    append(header,sizeof(header));append(data,size);append(extra,extra_size);
}
}
