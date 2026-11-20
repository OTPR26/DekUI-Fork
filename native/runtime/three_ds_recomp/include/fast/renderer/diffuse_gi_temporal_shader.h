#pragma once

// GLSL compute kernels of the diffuse GI temporal accumulation and a-trous
// filter (milestone M3). They follow the Precision::Float32 path of
// diffuse_gi_temporal.h operation by operation and read the same std140
// blocks (TemporalShaderParams, FilterShaderParams). Compiled at run time by
// the GPU tests; not wired into the renderer, the effect graph, settings or F1.
//
// Temporal kernel, set 0: 0 uniform TemporalShaderParams; 1 vec4 raw[] (J.rgb,
// A raw); 2 vec4 geometry[] (normal.xyz, view depth; 0 = not a receiver);
// 3 vec2 motion[]; 4 vec4 previous_position[] (xyz); 5 uint ids[]; 6 history
// in, 7 history out (four vec4 per pixel, layout of kHistoryValues); 8 vec4
// colour[] (J.rgb, A); 9 vec2 variance[] (Var J, Var A); 10 vec4 diag[]
// (alpha, alpha A, length, length A); 11 vec4 diag2[] (z J, z A, confidence,
// flags: 1 receiver, 2 valid, 4 accepted, 8 moving, 16 current valid,
// 32 light response). All buffers are viewport sized; history in and out are
// distinct buffers (ping-pong, never aliased).
//
// Filter kernel, set 0: 0 uniform FilterShaderParams; 1 vec4 geometry[];
// 2 vec4 colour in[]; 3 vec2 variance in[]; 4 vec4 colour out[]; 5 vec2
// variance out[]. One dispatch per level, ping-pong between levels.
//
// Workgroups are 8 x 8; neither kernel has a barrier, so out-of-range
// invocations return at once; every coordinate is checked before a read.

#include <cstdint>
#include <string>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kTemporalDiagReceiver = 1U;
inline constexpr uint32_t kTemporalDiagValid = 2U;
inline constexpr uint32_t kTemporalDiagAccepted = 4U;
inline constexpr uint32_t kTemporalDiagMoving = 8U;
inline constexpr uint32_t kTemporalDiagCurrentValid = 16U;
inline constexpr uint32_t kTemporalDiagLightResponse = 32U;

// `shading` (M8): the smooth-surface paths compiled in, each behind its flag
// (temporal: Extent.z & 2, filter: Normals.z != 0; both off run the M3-M7
// arithmetic unchanged). Temporal binding 12 and filter binding 6: vec4
// shading geometry[] (oriented shading normal, w = 1 when valid).
[[nodiscard]] std::string BuildDiffuseGiTemporalShader(bool shading = false);
[[nodiscard]] std::string BuildDiffuseGiFilterShader(bool shading = false);

} // namespace Fast::Renderer::DiffuseGi
