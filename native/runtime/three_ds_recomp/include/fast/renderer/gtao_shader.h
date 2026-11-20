#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Renderer {

// Native depth and encoded view-space normals are co-located. ViewX/ViewY
// map normalized storage UV to view XY / positive view depth; view Z is -depth.
// The adapter supplies orientation and projection, never the shader.
struct GtaoConstants {
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Padding = 0;
    uint32_t Quality = 0;
    std::array<float, 4> ViewX{};
    std::array<float, 4> ViewY{};
    // perspective: x / (y - hardwareDepth * z); W buffer: x + hardwareDepth * z.
    std::array<float, 4> Depth{};
    // Small radius, large radius, fade start, fade end, in view/world units.
    std::array<float, 4> RadiusFade{};
    // Small-scale gain, additional large-scale gain, power, maximum occlusion.
    std::array<float, 4> Response{};
    // Edge sharpness, squared normal decoding uncertainty; remaining components zero.
    std::array<float, 4> Filter{};
};
static_assert(sizeof(GtaoConstants) == 112);

[[nodiscard]] std::string BuildMultiscaleGtaoShader();
[[nodiscard]] std::string BuildMultiscaleGtaoResolveShader();

} // namespace Fast::Renderer
