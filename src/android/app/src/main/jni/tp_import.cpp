// SPDX-License-Identifier: GPL-2.0-or-later
#include <jni.h>
#include <cstddef>
#include <cstdint>
#include "common/cityhash.h"
#include "../../../../../../externals/tp_texture_decoder/astc.h"
extern "C" JNIEXPORT std::uint64_t dekui_tp_texture_hash(const std::uint8_t* data, std::size_t length) {
    return Common::CityHash64(reinterpret_cast<const char*>(data), length);
}
extern "C" JNIEXPORT int dekui_tp_decode_astc(const std::uint8_t* data, std::size_t length,
    int width, int height, int block_width, int block_height, std::uint32_t* output) {
    if (!data || !output || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        block_width < 4 || block_width > 12 || block_height < 4 || block_height > 12)
        return 0;
    const auto expected = std::size_t((width + block_width - 1) / block_width) *
                          ((height + block_height - 1) / block_height) * 16;
    if (length < expected) return 0;
    return decode_astc(data, width, height, block_width, block_height, output);
}
#include <span>
#include <vector>
#include "video_core/rasterizer_cache/texture_codec.h"
extern "C" JNIEXPORT std::uint64_t dekui_tp_legacy_texture_hash(const std::uint8_t* data,
    std::size_t length, int width, int height) {
    if (!data || width < 8 || height < 8 || width > 4096 || height > 4096 ||
        width % 8 || height % 8 || length != std::size_t(width) * height) return 0;
    std::vector<u8> linear(std::size_t(width) * height * 4);
    std::vector<u8> raw(data, data + length);
    VideoCore::UNSWIZZLE_TABLE[13](width, height, 0, length, linear,
                                  raw);
    return Common::CityHash64(reinterpret_cast<const char*>(linear.data()), linear.size());
}
