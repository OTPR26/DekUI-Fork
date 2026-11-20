#pragma once

// Savestates carry renderer-owned content (render targets, GPU texture copies,
// display images) at the guest's own extent, so a state does not depend on the
// internal resolution, the output size or any enhancement setting that was
// active when it was written. These helpers convert between that extent and
// the extent a renderer uses; they are CPU-only and title-neutral.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace Fast::Renderer3ds {

// RenderScalePermille of a render-target snapshot (and of a display image's
// depth association) stored at the guest framebuffer extent: the target is
// recreated at the restoring renderer's own scale.
inline constexpr uint16_t kPicaGuestExtentRenderScalePermille = 0U;

namespace GuestExtentDetail {

inline void RequireExtent(size_t elements, uint32_t width, uint32_t height,
                          uint32_t channels) {
    if (width == 0U || height == 0U ||
        elements != static_cast<size_t>(width) * height * channels) {
        throw std::invalid_argument("guest-extent resample: invalid image");
    }
}

// Source range [begin, end) covered by destination index `index` when `source`
// samples shrink to `destination` samples.
inline void CoveredRange(uint32_t index, uint32_t source, uint32_t destination,
                         uint32_t& begin, uint32_t& end) {
    begin = static_cast<uint32_t>(static_cast<uint64_t>(index) * source /
                                  destination);
    end = static_cast<uint32_t>(
        (static_cast<uint64_t>(index) + 1U) * source / destination);
    end = std::max(end, begin + 1U);
}

} // namespace GuestExtentDetail

// Area average for downsampling and centre-aligned bilinear interpolation for
// upsampling, chosen per axis. RGBA8 colour only.
[[nodiscard]] inline std::vector<uint8_t> ResamplePicaRgba8(
    std::span<const uint8_t> source, uint32_t sourceWidth,
    uint32_t sourceHeight, uint32_t width, uint32_t height) {
    GuestExtentDetail::RequireExtent(source.size(), sourceWidth, sourceHeight, 4U);
    if (width == 0U || height == 0U) {
        throw std::invalid_argument("guest-extent resample: invalid extent");
    }
    if (width == sourceWidth && height == sourceHeight) {
        return {source.begin(), source.end()};
    }
    // Axis samples: either the covered source range (shrink) or the two
    // bilinear neighbours with their weight (enlarge).
    struct AxisSample {
        uint32_t Begin = 0;
        uint32_t End = 0;
        float Weight = 0.0F;
        bool Average = false;
    };
    const auto axis = [](uint32_t sourceExtent, uint32_t extent) {
        std::vector<AxisSample> samples(extent);
        for (uint32_t index = 0; index < extent; ++index) {
            auto& sample = samples[index];
            if (extent <= sourceExtent) {
                GuestExtentDetail::CoveredRange(index, sourceExtent, extent,
                                                sample.Begin, sample.End);
                sample.Average = true;
                continue;
            }
            const float position =
                (static_cast<float>(index) + 0.5F) *
                    static_cast<float>(sourceExtent) /
                    static_cast<float>(extent) -
                0.5F;
            const float clamped = std::clamp(
                position, 0.0F, static_cast<float>(sourceExtent - 1U));
            sample.Begin = static_cast<uint32_t>(std::floor(clamped));
            sample.End = std::min(sample.Begin + 1U, sourceExtent - 1U);
            sample.Weight = clamped - static_cast<float>(sample.Begin);
        }
        return samples;
    };
    const auto columns = axis(sourceWidth, width);
    const auto rows = axis(sourceHeight, height);
    std::vector<uint8_t> result(static_cast<size_t>(width) * height * 4U);
    const auto texel = [&](uint32_t x, uint32_t y, uint32_t channel) {
        return static_cast<float>(
            source[(static_cast<size_t>(y) * sourceWidth + x) * 4U + channel]);
    };
    for (uint32_t y = 0; y < height; ++y) {
        const auto& row = rows[y];
        for (uint32_t x = 0; x < width; ++x) {
            const auto& column = columns[x];
            for (uint32_t channel = 0; channel < 4U; ++channel) {
                // Reduce along X for one source row, then along Y.
                const auto alongX = [&](uint32_t sourceY) {
                    if (column.Average) {
                        float sum = 0.0F;
                        for (uint32_t sx = column.Begin; sx < column.End; ++sx) {
                            sum += texel(sx, sourceY, channel);
                        }
                        return sum / static_cast<float>(column.End - column.Begin);
                    }
                    return texel(column.Begin, sourceY, channel) * (1.0F - column.Weight) +
                           texel(column.End, sourceY, channel) * column.Weight;
                };
                float value = 0.0F;
                if (row.Average) {
                    for (uint32_t sy = row.Begin; sy < row.End; ++sy) {
                        value += alongX(sy);
                    }
                    value /= static_cast<float>(row.End - row.Begin);
                } else {
                    value = alongX(row.Begin) * (1.0F - row.Weight) +
                            alongX(row.End) * row.Weight;
                }
                result[(static_cast<size_t>(y) * width + x) * 4U + channel] =
                    static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
    return result;
}

// Nearest sample at each destination centre, for values that must not be
// blended: depth, stencil and integer payloads.
template <typename T>
[[nodiscard]] std::vector<T> ResamplePicaNearest(
    std::span<const T> source, uint32_t sourceWidth, uint32_t sourceHeight,
    uint32_t width, uint32_t height) {
    GuestExtentDetail::RequireExtent(source.size(), sourceWidth, sourceHeight, 1U);
    if (width == 0U || height == 0U) {
        throw std::invalid_argument("guest-extent resample: invalid extent");
    }
    std::vector<T> result(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        const auto sourceY = static_cast<uint32_t>(
            (static_cast<uint64_t>(y) * 2U + 1U) * sourceHeight / (2ULL * height));
        for (uint32_t x = 0; x < width; ++x) {
            const auto sourceX = static_cast<uint32_t>(
                (static_cast<uint64_t>(x) * 2U + 1U) * sourceWidth / (2ULL * width));
            result[static_cast<size_t>(y) * width + x] =
                source[static_cast<size_t>(sourceY) * sourceWidth + sourceX];
        }
    }
    return result;
}

} // namespace Fast::Renderer3ds
