#pragma once

// The module's normal code (diffuse screen-space GI, documents "M5" and "M8"):
// the octahedral map of a unit vector (n / (|x| + |y| + |z|), the lower
// hemisphere folded over the diagonals), each axis in unorm16 offset by one
// so that 0 means "no normal". The GLSL functions (dgi_encode_normal,
// dgi_decode_normal) and their FP32 twins (same operation order) live here so
// every kernel and twin that stores a normal in 32 bits shares one code: the
// output guide of the upsampler (face and shading normals) and the shading
// normal kept in the temporal history (M8).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Fast::Renderer::DiffuseGi {

inline constexpr const char* kDiffuseGiNormalCodecGlsl = R"glsl(
vec3 dgi_decode_normal(uint code) {
    if (code == 0u) return vec3(0.0);
    float u = (float(code & 0xFFFFu) - 1.0) / 65534.0 * 2.0 - 1.0;
    float v = (float(code >> 16u) - 1.0) / 65534.0 * 2.0 - 1.0;
    float z = 1.0 - abs(u) - abs(v);
    float x = u;
    float y = v;
    if (z < 0.0) {
        x = (1.0 - abs(v)) * (u >= 0.0 ? 1.0 : -1.0);
        y = (1.0 - abs(u)) * (v >= 0.0 ? 1.0 : -1.0);
    }
    float len = sqrt(x * x + y * y + z * z);
    return len > 0.0 ? vec3(x, y, z) / len : vec3(0.0);
}
uint dgi_encode_normal(vec3 n) {
    float s = abs(n.x) + abs(n.y) + abs(n.z);
    if (!(s > 0.0) || isinf(s) || isnan(s)) return 0u;
    float u = n.x / s;
    float v = n.y / s;
    if (n.z < 0.0) {
        float pu = (1.0 - abs(v)) * (u >= 0.0 ? 1.0 : -1.0);
        float pv = (1.0 - abs(u)) * (v >= 0.0 ? 1.0 : -1.0);
        u = pu;
        v = pv;
    }
    uint cu = uint(floor(clamp(u * 0.5 + 0.5, 0.0, 1.0) * 65534.0 + 0.5)) + 1u;
    uint cv = uint(floor(clamp(v * 0.5 + 0.5, 0.0, 1.0) * 65534.0 + 0.5)) + 1u;
    return cu | (cv << 16u);
}
)glsl";

[[nodiscard]] inline uint32_t EncodeDiffuseGiNormal(const std::array<float, 3>& n) noexcept {
    const float s = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
    if (!(s > 0.0F) || !std::isfinite(s)) return 0U;
    float u = n[0] / s;
    float v = n[1] / s;
    if (n[2] < 0.0F) {
        const float pu = (1.0F - std::abs(v)) * (u >= 0.0F ? 1.0F : -1.0F);
        const float pv = (1.0F - std::abs(u)) * (v >= 0.0F ? 1.0F : -1.0F);
        u = pu;
        v = pv;
    }
    const auto code = [](float t) {
        return static_cast<uint32_t>(std::floor(std::clamp(t * 0.5F + 0.5F, 0.0F, 1.0F) * 65534.0F + 0.5F)) + 1U;
    };
    return code(u) | (code(v) << 16U);
}

[[nodiscard]] inline std::array<float, 3> DecodeDiffuseGiNormal(uint32_t code) noexcept {
    if (code == 0U) return {};
    const float u = (static_cast<float>(code & 0xFFFFU) - 1.0F) / 65534.0F * 2.0F - 1.0F;
    const float v = (static_cast<float>(code >> 16U) - 1.0F) / 65534.0F * 2.0F - 1.0F;
    const float z = 1.0F - std::abs(u) - std::abs(v);
    float x = u, y = v;
    if (z < 0.0F) {
        x = (1.0F - std::abs(v)) * (u >= 0.0F ? 1.0F : -1.0F);
        y = (1.0F - std::abs(u)) * (v >= 0.0F ? 1.0F : -1.0F);
    }
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 0.0F)) return {};
    return {x / len, y / len, z / len};
}

} // namespace Fast::Renderer::DiffuseGi
