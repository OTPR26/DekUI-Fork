#pragma once

// GLSL twins of the M4 paths of the diffuse GI (docs/TRIAEVUM_SCREEN_SPACE_GI.md,
// "M4"): hierarchy level 0 and reduction (with the radiance cache and the SH
// sums), the gather with the metric radial distribution, hierarchy
// footprints, cache, guiding and blue-noise rotations, and the guide update.
// They follow the FP32 C++ twins operation by operation and are compiled at
// run time by diffuse_gi_m4_gpu_tests. Not wired into the renderer.
//
// Bindings (set 0), shared by every M4 kernel: 0-6 as the M2 gather (params,
// depth, class, radiance, normals, results, aux); 7 uniform M4ShaderParams;
// 8 vec4 cells[3 per cell] ((W, Gx, Gy, flags | count << 8 as bits),
// (normal, ZMin), (mean radiance, ZMax)); 9 vec4 sh[ShCount per cell]
// ((N.rgb, D)); 10 float guide table (per region Cdf[B + 1], then Pdf[B]);
// 11 vec4 guide records per pixel and section (contribution, density, phi,
// bin as bits; 0xFFFFFFFF: none); 12 uint noise ranks; 13 / 14 float guide
// state in / out (per region E[B], then valid); 15 float guide table out.
// Workgroups 8 x 8 (64 for the update); no barriers inside the kernels.

#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_guiding.h"
#include "fast/renderer/diffuse_gi_hierarchy.h"
#include "fast/renderer/diffuse_gi_noise.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kM4FlagFootprint = 1U;
inline constexpr uint32_t kM4FlagCacheMean = 2U;
inline constexpr uint32_t kM4FlagCacheSh = 4U;
inline constexpr uint32_t kM4FlagGuiding = 8U;
inline constexpr uint32_t kM4FlagNoise = 16U;
inline constexpr uint32_t kM4FlagIndependentA = 32U;
inline constexpr uint32_t kM4FlagRecords = 64U;

// std140 uniform block, 240 bytes.
struct M4ShaderParams {
    std::array<uint32_t, 4> Features{};                   // flags, gather max level, hierarchy levels, SH order
    std::array<std::array<uint32_t, 4>, 8> Levels{};      // width, height, first cell, 0
    std::array<float, 4> Hierarchy{};                     // plane tolerance, normal cos, spacing scale, 0
    std::array<uint32_t, 4> Guide{};                      // regions x, regions y, region log2, bins
    std::array<uint32_t, 4> Noise{};                      // width, height, depth, slice (frame mod depth)
    std::array<float, 4> NoiseScale{};                    // 1 / (W H D) as float, 0, 0, 0
    std::array<uint32_t, 4> Build{};                      // level being built, sections (update), 0, 0
    std::array<float, 4> GuideRates{};                    // mix eta, rate beta, 0, 0
};
static_assert(sizeof(M4ShaderParams) == 240);

// hierarchy: settings of the hierarchy (levels, tolerances, SH order); guide
// and guiding: the table of the frame and the guiding settings (may be null);
// noise: the mask (may be null).
[[nodiscard]] M4ShaderParams MakeM4ShaderParams(const GatherInput& input, const GatherSettings& settings,
                                                const HierarchySettings& hierarchy, const GuideTable* guide,
                                                const GuidingSettings* guiding, const NoiseMask* noise,
                                                bool records);
// Total hierarchy cells of a viewport (all levels).
[[nodiscard]] uint32_t HierarchyCellCount(uint32_t width, uint32_t height, uint32_t levels);
// Float layout of binding 10 / 15: per region Cdf[B + 1] then Pdf[B].
[[nodiscard]] std::vector<float> PackGuideTable(const GuideTable& table);
// Float layout of binding 13 / 14: per region E[B] then valid (0 / 1).
[[nodiscard]] std::vector<float> PackGuideState(const GuideField& field);

[[nodiscard]] std::string BuildDiffuseGiHierarchyLevel0Shader(bool shading = false);
[[nodiscard]] std::string BuildDiffuseGiHierarchyReduceShader(bool shading = false);
// `shading`: the M8 smooth-shaded receiver compiled in (behind its flag).
[[nodiscard]] std::string BuildDiffuseGiGatherM4Shader(bool shading = false);
[[nodiscard]] std::string BuildDiffuseGiGuideUpdateShader();

} // namespace Fast::Renderer::DiffuseGi
