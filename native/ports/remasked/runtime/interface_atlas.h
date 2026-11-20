// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <span>

namespace ReMasked {
// Oversized UI atlases keep their original on-disk identity. Reduce only the
// worker's RGBA staging image, before hashing its pixels and building mipmaps.
// Average premultiplied colour so transparent atlas borders cannot turn dark.
inline std::vector<uint8_t> CopyLimitedInterfaceAtlas(std::span<const uint8_t> pixels,
                                uint32_t& width, uint32_t& height, uint32_t maximum) {
    if (!width || !height || pixels.size() != uint64_t(width) * height * 4)
        throw std::runtime_error("Invalid interface atlas geometry");
    std::vector<uint8_t> owned;
    while (maximum && (width > maximum || height > maximum)) {
        const uint32_t w = std::max(1U, (width + 1) / 2);
        const uint32_t h = std::max(1U, (height + 1) / 2);
        std::vector<uint8_t> reduced(uint64_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            uint32_t colour[3]{}, alpha = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) for (uint32_t dx = 0; dx < 2; ++dx) {
                const auto at = (uint64_t(std::min(height - 1, 2*y + dy)) * width +
                                 std::min(width - 1, 2*x + dx)) * 4;
                const uint32_t a = pixels[at + 3];
                alpha += a;
                for (unsigned c = 0; c < 3; ++c) colour[c] += pixels[at+c] * a;
            }
            const auto at = (uint64_t(y) * w + x) * 4;
            for (unsigned c = 0; c < 3; ++c)
                reduced[at+c] = alpha ? (colour[c] + alpha/2) / alpha : 0;
            reduced[at+3] = (alpha + 2) / 4;
        }
        owned = std::move(reduced); pixels = owned; width = w; height = h;
    }
    if (owned.empty()) return {pixels.begin(), pixels.end()};
    return owned;
}
inline void LimitInterfaceAtlas(std::vector<uint8_t>& pixels, uint32_t& width,
                                uint32_t& height, uint32_t maximum) {
    if (!maximum || (width <= maximum && height <= maximum)) return;
    pixels = CopyLimitedInterfaceAtlas(pixels, width, height, maximum);
}
}
