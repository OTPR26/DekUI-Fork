#pragma once
#include <string_view>

namespace Fast::Renderer3ds {
// Float and integer image interfaces cannot be switched through a uniform.
// All other texture selection remains native per-draw data. Coordinates come
// from the caller's scope: an extension may shadow them in main (PBR relief),
// exactly as for inline specialized sampling.
inline constexpr std::string_view PicaFloatTexture0Program = R"glsl(
vec4 pica_native_texture0(vec2 tc0) {
    uint type = (fragment_uniforms.fragment_control.w >> 28u) & 7u;
    if (type == 5u) return vec4(0.0);
    if (type == 3u) return textureProj(pica_texture0,
        vec3(tc0.x, pica_texcoord0_w - tc0.y, pica_texcoord0_w));
    return pica_sample_texture0(vec2(tc0.x, 1.0 - tc0.y));
}
)glsl";
inline constexpr std::string_view PicaIntegerTexture0Program = R"glsl(
vec4 pica_native_texture0(vec2 tc0) {
    return pica_sample_shadow2d(tc0, pica_texcoord0_w);
}
)glsl";
inline constexpr std::string_view PicaTextureSelectionProgram = R"glsl(
vec4 pica_native_texture(uint unit, vec2 tc0, vec2 tc1, vec2 tc2) {
    if (unit == 3u) return pica_native_texture3();
    if ((fragment_uniforms.fragment_control.z & (1u << unit)) == 0u) return vec4(0.0);
    if (unit == 0u) return pica_native_texture0(tc0);
    if (unit == 1u) return pica_sample_texture1(vec2(tc1.x, 1.0 - tc1.y));
    vec2 uv = (fragment_uniforms.fragment_control.z & 8192u) != 0u ? tc1 : tc2;
    return pica_sample_texture2(vec2(uv.x, 1.0 - uv.y));
}
)glsl";
} // namespace Fast::Renderer3ds
