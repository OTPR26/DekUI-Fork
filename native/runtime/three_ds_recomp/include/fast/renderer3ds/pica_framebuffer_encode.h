#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace Fast::Renderer3ds {
// Resolve an internally scaled RGBA attachment to its native tiled bytes.
//
// Row order: attachment row r holds PICA window row r (the viewport maps NDC
// y = -1 to row 0), while native memory stores the top window row first:
// memory row y holds window row height - 1 - y. Azahar's framebuffer writes
// window row y at that memory row and its texture unit reads coordinate t from
// row height - 1 - t, so a raw copy sampled as a texture shows the frame the
// way it was rendered. Memory row y is therefore read from the attachment's
// native row height - 1 - y, sampled at its centre. Columns keep their order;
// no screen rotation or presentation effect is applied.
inline std::vector<uint8_t> EncodePicaFramebuffer(
    std::span<const uint8_t> rgba, uint32_t imageWidth, uint32_t imageHeight,
    uint32_t width, uint32_t height, uint8_t format) {
    if (width == 0 || height == 0 || width % 8 || height % 8 || format > 4 ||
        imageWidth == 0 || imageHeight == 0 ||
        rgba.size() % 4 != 0 || uint64_t{imageWidth} * imageHeight != rgba.size() / 4)
        throw std::runtime_error("invalid PICA framebuffer encode extent/format");
    const uint32_t bpp = format == 0 ? 4 : format == 1 ? 3 : 2;
    if (uint64_t{width} * height > std::numeric_limits<std::size_t>::max() / bpp)
        throw std::runtime_error("PICA framebuffer byte size overflows");
    std::vector<uint8_t> result(std::size_t{width} * height * bpp);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto sx = static_cast<uint32_t>((uint64_t{x} * 2 + 1) * imageWidth / (uint64_t{width} * 2));
            const auto sy = static_cast<uint32_t>(
                (uint64_t{height - 1U - y} * 2 + 1) * imageHeight / (uint64_t{height} * 2));
            const auto* c = rgba.data() + (std::size_t{sy} * imageWidth + sx) * 4;
            uint32_t morton = 0;
            for (uint32_t bit = 0; bit < 3; ++bit) {
                morton |= ((x >> bit) & 1) << (bit * 2);
                morton |= ((y >> bit) & 1) << (bit * 2 + 1);
            }
            auto* out = result.data() + (((y / 8) * (width / 8) + x / 8) * 64 + morton) * bpp;
            if (format == 0) { out[0] = c[3]; out[1] = c[2]; out[2] = c[1]; out[3] = c[0]; }
            else if (format == 1) { out[0] = c[2]; out[1] = c[1]; out[2] = c[0]; }
            else {
                const uint16_t packed = format == 2 ? ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3) :
                    format == 3 ? ((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (c[3] >> 7) :
                    ((c[0] >> 4) << 12) | ((c[1] >> 4) << 8) | ((c[2] >> 4) << 4) | (c[3] >> 4);
                out[0] = static_cast<uint8_t>(packed); out[1] = static_cast<uint8_t>(packed >> 8);
            }
        }
    }
    return result;
}

// Offset of native texel (x, y) of a width-wide tiled framebuffer, in texels.
[[nodiscard]] inline std::size_t PicaFramebufferTexel(uint32_t x, uint32_t y, uint32_t width) noexcept {
    uint32_t morton = 0;
    for (uint32_t bit = 0; bit < 3; ++bit) {
        morton |= ((x >> bit) & 1) << (bit * 2);
        morton |= ((y >> bit) & 1) << (bit * 2 + 1);
    }
    return (std::size_t{y / 8} * (width / 8) + x / 8) * 64 + morton;
}

// Inverse of EncodePicaFramebuffer at the native extent: RGBA8 rows in
// attachment order (row r = window row r).
inline std::vector<uint8_t> DecodePicaFramebuffer(
    std::span<const uint8_t> bytes, uint32_t width, uint32_t height, uint8_t format) {
    if (width == 0 || height == 0 || width % 8 || height % 8 || format > 4)
        throw std::runtime_error("invalid PICA framebuffer decode extent/format");
    const uint32_t bpp = format == 0 ? 4 : format == 1 ? 3 : 2;
    if (bytes.size() < std::size_t{width} * height * bpp)
        throw std::runtime_error("PICA framebuffer bytes are truncated");
    // n-bit channel to 8 bits by bit replication (n = 4, 5 or 6).
    const auto expand = [](uint32_t value, uint32_t bits) {
        return static_cast<uint8_t>((value << (8U - bits)) | (value >> (2U * bits - 8U)));
    };
    std::vector<uint8_t> rgba(std::size_t{width} * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto* in = bytes.data() + PicaFramebufferTexel(x, y, width) * bpp;
            auto* c = rgba.data() + (std::size_t{height - 1U - y} * width + x) * 4;
            if (format == 0) { c[3] = in[0]; c[2] = in[1]; c[1] = in[2]; c[0] = in[3]; }
            else if (format == 1) { c[2] = in[0]; c[1] = in[1]; c[0] = in[2]; c[3] = 255; }
            else {
                const uint32_t p = in[0] | (uint32_t{in[1]} << 8);
                if (format == 2) {
                    c[0] = expand(p >> 11, 5); c[1] = expand((p >> 5) & 63, 6); c[2] = expand(p & 31, 5); c[3] = 255;
                } else if (format == 3) {
                    c[0] = expand(p >> 11, 5); c[1] = expand((p >> 6) & 31, 5); c[2] = expand((p >> 1) & 31, 5);
                    c[3] = (p & 1) != 0 ? 255 : 0;
                } else {
                    c[0] = expand(p >> 12, 4); c[1] = expand((p >> 8) & 15, 4); c[2] = expand((p >> 4) & 15, 4);
                    c[3] = expand(p & 15, 4);
                }
            }
        }
    }
    return rgba;
}

// Native depth/stencil formats: 0 D16, 2 D24, 3 D24S8 (stencil in the high
// byte). Bytes per texel; 0 for an unknown format.
[[nodiscard]] inline uint32_t PicaDepthFormatBytes(uint8_t format) noexcept {
    return format == 0 ? 2U : format == 2 ? 3U : format == 3 ? 4U : 0U;
}

// Window depth in [0, 1] and optional stencil, rows in attachment order, to
// native tiled bytes (the colour's row and tile order).
inline std::vector<uint8_t> EncodePicaDepthStencil(
    std::span<const float> depth, std::span<const uint8_t> stencil,
    uint32_t width, uint32_t height, uint8_t format) {
    const uint32_t bpp = PicaDepthFormatBytes(format);
    if (width == 0 || height == 0 || width % 8 || height % 8 || bpp == 0 ||
        depth.size() != std::size_t{width} * height ||
        (!stencil.empty() && stencil.size() != depth.size()))
        throw std::runtime_error("invalid PICA depth encode extent/format");
    const double scale = format == 0 ? 65535.0 : 16777215.0;
    std::vector<uint8_t> result(std::size_t{width} * height * bpp);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const std::size_t source = std::size_t{height - 1U - y} * width + x;
            const float value = depth[source];
            const double clamped = !(value > 0.0F) ? 0.0 : value > 1.0F ? 1.0 : double(value);
            const auto quantized = static_cast<uint32_t>(clamped * scale + 0.5);
            auto* out = result.data() + PicaFramebufferTexel(x, y, width) * bpp;
            out[0] = static_cast<uint8_t>(quantized);
            out[1] = static_cast<uint8_t>(quantized >> 8);
            if (bpp >= 3) out[2] = static_cast<uint8_t>(quantized >> 16);
            if (bpp == 4) out[3] = stencil.empty() ? 0U : stencil[source];
        }
    }
    return result;
}

inline void DecodePicaDepthStencil(
    std::span<const uint8_t> bytes, uint32_t width, uint32_t height, uint8_t format,
    std::vector<float>& depth, std::vector<uint8_t>& stencil) {
    const uint32_t bpp = PicaDepthFormatBytes(format);
    if (width == 0 || height == 0 || width % 8 || height % 8 || bpp == 0 ||
        bytes.size() < std::size_t{width} * height * bpp)
        throw std::runtime_error("invalid PICA depth decode extent/format");
    const double scale = format == 0 ? 65535.0 : 16777215.0;
    depth.assign(std::size_t{width} * height, 0.0F);
    stencil.assign(bpp == 4 ? depth.size() : 0U, 0U);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto* in = bytes.data() + PicaFramebufferTexel(x, y, width) * bpp;
            uint32_t value = in[0] | (uint32_t{in[1]} << 8);
            if (bpp >= 3) value |= uint32_t{in[2]} << 16;
            const std::size_t target = std::size_t{height - 1U - y} * width + x;
            depth[target] = static_cast<float>(value / scale);
            if (bpp == 4) stencil[target] = in[3];
        }
    }
}
} // namespace Fast::Renderer3ds
