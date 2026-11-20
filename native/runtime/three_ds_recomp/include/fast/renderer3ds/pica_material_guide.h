#pragma once

#include <string_view>

namespace Fast::Renderer3ds {

// RGBA8 material guide: R reflectivity/F0, G roughness, B reflection weight,
// A temporal reactivity. Weight uses byte 127 for 1 and 254 for 2, preserving
// exact neutral/off endpoints without another attachment or clipping boosts.
inline constexpr std::string_view PicaMaterialGuideShaderLibrary = R"glsl(
float pica_encode_reflection_weight(float weight) {
    return clamp(weight, 0.0, 2.0) * (127.0 / 255.0);
}
float pica_reflection_weight(vec4 material) {
    return material.b * (255.0 / 127.0);
}
)glsl";

} // namespace Fast::Renderer3ds
