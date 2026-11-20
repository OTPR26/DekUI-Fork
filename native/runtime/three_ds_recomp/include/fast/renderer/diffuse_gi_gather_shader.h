#pragma once

// GLSL compute kernels of the diffuse GI gathering (milestone M2): geometric
// normal reconstruction, gathering with sector masks, and a unit kernel for
// masks and sector weights. They follow GatherImageF32 operation by operation
// (diffuse_gi_gather.h) and are compiled at run time by the GPU tests. Not
// wired into the renderer, the effect graph, settings or F1.
//
// Bindings (set 0): 0 uniform GatherShaderParams; 1 float depth[]; 2 uint
// class[]; 3 vec4 radiance[] (rgb); 4 vec4 normal[] (xyz, w = 1 when valid;
// face normals in, oriented normals out); 5 vec4 result[] per viewport pixel
// (J.rgb, A raw); 6 vec4 aux[] per viewport pixel (C, valid, scheduled,
// occupied sectors). Buffers 1-4 cover the whole buffer extent; only the
// declared viewport is read. Workgroups are 8 x 8; the kernels have no
// barriers, so out-of-range invocations return immediately.

#include "fast/renderer/diffuse_gi_gather.h"

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kShaderFlagInterleave = 1U;
inline constexpr uint32_t kShaderFlagRejectSelf = 2U;
inline constexpr uint32_t kShaderFlagFaceNormals = 4U;
// M8: the gather's smooth-shaded receiver (GatherSettings::ShadingNormals);
// the pass's shading-normal inputs (the normal pass orients them, the pack
// copies them per viewport pixel). Kernels built with `shading`, binding 13:
// vec4 shading normals[] (buffer sized, in place like the normals); pack
// binding 14: vec4 shading geometry[] (viewport sized).
inline constexpr uint32_t kShaderFlagShading = 8U;
inline constexpr uint32_t kShaderFlagShadingInputs = 16U;

// Sequence flags (M3): stratified per-pixel rotation; per-pixel radial jitter.
inline constexpr uint32_t kShaderSequenceStratified = 1U;
inline constexpr uint32_t kShaderSequenceRadialJitter = 2U;
inline constexpr uint32_t kShaderSequenceFlip = 4U; // FrameFlip of this frame

// std140 uniform block, 176 bytes (128 in M2; M3 appends the rotation
// sequence, which the kernel ignores when its flags are zero).
struct GatherShaderParams {
    std::array<uint32_t, 4> BufferViewport{}; // buffer width, buffer height, viewport x, viewport y
    std::array<uint32_t, 4> ExtentCounts{};   // viewport width, viewport height, sections, sectors
    std::array<uint32_t, 4> Modes{};          // depth encoding, schedule, candidates per side, flags
    std::array<float, 4> ViewX{};             // ax, bx, cx, 0
    std::array<float, 4> ViewY{};             // ay, by, cy, 0
    std::array<float, 4> Depth{};             // near, far, infinite far (1/0), 0
    std::array<float, 4> Gather{};            // radius, thickness, rotation, chord epsilon
    std::array<float, 4> Radial{};            // exponent, jitter, self-intersection sine, depth
    std::array<uint32_t, 4> Sequence{};       // sequence flags, tile log2, cycle F, phase t
    std::array<uint32_t, 4> Strata{};         // M, lattice multiplier P, tiles T^2, frame shift s(t)
    std::array<float, 4> StrataScale{};       // 1 / M (float), own-surface cos (M8), hard-edge ratio (M8), 0
};
static_assert(sizeof(GatherShaderParams) == 176);

[[nodiscard]] GatherShaderParams MakeGatherShaderParams(const GatherInput& input, const GatherSettings& settings);

[[nodiscard]] std::string BuildDiffuseGiNormalShader(bool shading = false);
[[nodiscard]] std::string BuildDiffuseGiGatherShader();
// The gather's constants, buffers, sides, accumulation and candidate
// processing without its entry point (M4 kernels, diffuse_gi_m4_shader.h).
[[nodiscard]] std::string DiffuseGiGatherFunctionsGlsl(bool shading = false);
// M3 geometry pack (same bindings 0-6 as the gather, plus 7): vec4 per
// viewport pixel (oriented normal xyz, view depth z) where the gather output
// is valid (aux.y != 0), zero elsewhere. Input of the temporal kernels.
[[nodiscard]] std::string BuildDiffuseGiPackShader(bool shading = false);
// Unit kernel: binding 0 uniform uvec4 (count, sectors, mode, 0); binding 1
// vec4 inputs[]; binding 2 uvec4 outputs[2 * count] (mode 0: centre-rule mask
// of [x, y) in 8 words) or vec4 outputs[count] (mode 1: M1 weight of
// a = x, b = y, [z, w]).
[[nodiscard]] std::string BuildDiffuseGiUnitShader();

} // namespace Fast::Renderer::DiffuseGi
