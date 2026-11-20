// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>
namespace Oot3dNativeGame {
inline std::optional<std::vector<uint8_t>> RebasePicaIndices(std::span<const uint8_t> bytes,bool wide,int32_t base) {
    const size_t stride=wide?2:1;
    if(bytes.size()%stride)return std::nullopt;
    std::vector<uint8_t> result(bytes.size());
    for(size_t i=0;i<bytes.size();i+=stride){
        uint16_t raw=0;if(wide)std::memcpy(&raw,bytes.data()+i,2);else raw=bytes[i];
        const int64_t value=int64_t(raw)+base;
        if(value<0||value>(wide?65535:255))return std::nullopt;
        const uint16_t adjusted=uint16_t(value);
        if(wide)std::memcpy(result.data()+i,&adjusted,2);else result[i]=uint8_t(adjusted);
    }
    return result;
}
}
