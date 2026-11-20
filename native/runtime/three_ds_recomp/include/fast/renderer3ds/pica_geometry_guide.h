#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Fast::Renderer3ds {

// Geometry guide: color attachment 6 of the instrumented layout, four 32-bit
// unsigned channels (R32G32B32A32_UINT). An integer format is never blended,
// and a multisample resolve keeps one sample's values instead of averaging
// packed fields.
//   R: native surface normal before relief and normal maps, octahedral
//      unorm16x2; 0 = no native surface. Written only by native
//      depth-tested, depth-writing, RGB-replacing scene draws (outlines).
//   G: face normal of the visible surface, from the rasterized triangle (the
//      screen derivatives of its view-space position), view space, oriented
//      toward the eye, octahedral unorm16x2; 0 = unknown.
//   B: kind of the visible surface: 0 none, 1 native scene, 2 extension
//      geometry. G and B are written by every draw that writes the scene's
//      depth and the normal guide, extension geometry included.
//   A: native window depth (float bits), with R.
// The cleared value is (0, 0, 0, bits of 1.0).
inline constexpr uint32_t kPicaGeometryGuideKindNone = 0U;
inline constexpr uint32_t kPicaGeometryGuideKindNative = 1U;
inline constexpr uint32_t kPicaGeometryGuideKindExtension = 2U;
inline constexpr std::array<uint32_t, 4> kPicaGeometryGuideClear{ 0U, 0U, 0U, 0x3F800000U };

inline std::string_view PicaGeometryGuideShaderLibrary() noexcept {
    return R"glsl(
uint pica_geometry_encode_normal(vec3 normal) {
    float sum = abs(normal.x) + abs(normal.y) + abs(normal.z);
    if (!(sum > 0.0)) return 0u;
    vec2 p = normal.xy / sum;
    if (normal.z < 0.0)
        p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return max(packUnorm2x16(p * 0.5 + 0.5), 1u);
}
vec3 pica_geometry_decode_normal(uint bits) {
    if (bits == 0u) return vec3(0.0);
    vec2 p = unpackUnorm2x16(bits) * 2.0 - 1.0;
    vec3 normal = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    float fold = max(-normal.z, 0.0);
    normal.xy += vec2(normal.x >= 0.0 ? -fold : fold, normal.y >= 0.0 ? -fold : fold);
    return normalize(normal);
}
// Face normal from the derivatives of a vector that is affine in view-space
// position (the position, or the vector toward the eye), oriented toward the
// eye; zero for a degenerate footprint.
vec3 pica_geometry_face_normal(vec3 to_eye, vec3 dx, vec3 dy) {
    vec3 normal = cross(dx, dy);
    float area = dot(normal, normal);
    if (!(area > 1.0e-20)) return vec3(0.0);
    normal *= inversesqrt(area);
    return dot(normal, to_eye) < 0.0 ? -normal : normal;
}
)glsl";
}

// CPU mirror of the shader encoding (tests, readbacks).
[[nodiscard]] inline uint32_t EncodePicaGeometryGuideNormal(const std::array<float, 3>& normal) noexcept {
    const float sum = std::abs(normal[0]) + std::abs(normal[1]) + std::abs(normal[2]);
    if (!(sum > 0.0F)) return 0U;
    float x = normal[0] / sum, y = normal[1] / sum;
    if (normal[2] < 0.0F) {
        const float foldedX = (1.0F - std::abs(y)) * (x >= 0.0F ? 1.0F : -1.0F);
        const float foldedY = (1.0F - std::abs(x)) * (y >= 0.0F ? 1.0F : -1.0F);
        x = foldedX;
        y = foldedY;
    }
    const auto unorm16 = [](float value) {
        return static_cast<uint32_t>(std::lround(std::clamp(value * 0.5F + 0.5F, 0.0F, 1.0F) * 65535.0F));
    };
    const uint32_t bits = unorm16(x) | (unorm16(y) << 16U);
    return bits != 0U ? bits : 1U;
}

[[nodiscard]] inline std::array<float, 3> DecodePicaGeometryGuideNormal(uint32_t bits) noexcept {
    if (bits == 0U) return {};
    float x = float(bits & 0xFFFFU) / 65535.0F * 2.0F - 1.0F;
    float y = float(bits >> 16U) / 65535.0F * 2.0F - 1.0F;
    const float z = 1.0F - std::abs(x) - std::abs(y);
    const float fold = std::max(-z, 0.0F);
    x += x >= 0.0F ? -fold : fold;
    y += y >= 0.0F ? -fold : fold;
    const float length = std::sqrt(x * x + y * y + z * z);
    return { x / length, y / length, z / length };
}

[[nodiscard]] inline float DecodePicaGeometryGuideDepth(uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}

} // namespace Fast::Renderer3ds
