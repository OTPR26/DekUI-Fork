#pragma once

// Diffuse screen-space GI, milestone M5: the CPU half of the runtime pass
// (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M5"). Title-neutral and Vulkan-free:
// settings and their validation, the transport key, the work scale, the
// resources with their sizes and lifetimes, the layout of the uniform
// parameter buffer and the plan of one frame (reset decision and every
// constant block, built by the M2-M4 functions that the FP32 twins read).
// The Vulkan execution is diffuse_gi_pass.h.
//
// Pipeline of one frame and view (one dispatch per line, a compute barrier
// between consecutive dispatches):
//   normal pass (M2)            input depth/class/normals -> oriented normals (in place)
//   hierarchy level 0 (M4)      -> cells of level 0 (with the mean radiance cache)
//   hierarchy reduce (M4)       one dispatch per level 1 .. L-1
//   gather (M4 kernel)          -> raw J and A, aux (C, validity, counts)
//   geometry pack (M3)          -> receivers (normal, view depth)
//   temporal (M3)               reconstruction + accumulation; history ping-pong
//   a-trous (M3)                one dispatch per level, ping-pong buffers
//   output (M5)                 -> J (RGBA16F) and A (R32F) images at the pass extent, or
//   upsample (M5)               -> the same images at the output extent (work scale f > 1)
// The hierarchy runs only when the footprint or the cache is on with a radial
// schedule; guiding, the SH backend and blue-noise rotations are not part of
// the runtime pass (M4 decisions, open point 20).
//
// Work scale (open point 24): with f = 2 or 4 the pass runs on one texel of
// every f x f block of the output, at phase o = (f - 1) / 2 (work pixel (i, j)
// is output texel (f i + o, f j + o)). The work image is an exact point
// subsample: its texels are output texels, its ray map is the output ray map
// composed with the affine index map, so depth, normal and radiance of a work
// texel always describe one surface (no averaging across silhouettes). The
// adapter writes the work inputs and, per output texel, an output guide (view
// depth, normal code) that the upsample kernel uses to keep light on its
// surface.

#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_gather_shader.h"
#include "fast/renderer/diffuse_gi_hierarchy.h"
#include "fast/renderer/diffuse_gi_m4_shader.h"
#include "fast/renderer/diffuse_gi_pass_shader.h"
#include "fast/renderer/diffuse_gi_temporal.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

// Surface test of the upsampler (the M3 hard surface test against the output
// texel's own plane and normal).
struct PassUpsampleSettings {
    double PlaneSine = 0.05;
    double PlaneDepth = 1.0e-3; // relative to the output texel's view depth
    double NormalCos = 0.9;
    // M8: the smooth-crease rule of TemporalSettings (the output texel's
    // shading normal from the adapter's output guide).
    bool ShadingNormals = false;
    double HardEdgeRatio = 0.5; // in angle about 0.71 (document "M8")
};

struct PassSettings {
    GatherSettings Gather;       // transport (R, tau in scene units), quality, rotations, M4 switches
    HierarchySettings Hierarchy; // levels and tolerances; SH order 0 (no SH sums)
    TemporalSettings Temporal;   // Enabled = false: every frame resets (alpha = 1)
    FilterSettings Filter;       // Levels = 0: no spatial filter
    PassUpsampleSettings Upsample;
    // Largest stored |J| per channel. The default is the FP16 storage bound;
    // a lower value is the artistic highlight clamp (specification section 9).
    double OutputLimit = 65504.0;
};

// The M4 recommendation: metric radial schedule K 16, e 2, footprint strips
// and the mean cache; one stratified section per pixel and frame (tile 4 x 4,
// cycle 7, lattice radial jitter), N 64; guiding, SH and blue noise off; the
// M3 temporal and filter defaults. R = 1 and tau = 0.5 are module
// placeholders in scene units: the adapter declares the title's values
// (open point 2).
[[nodiscard]] PassSettings DefaultPassSettings();
// Empty when the settings can run in the pass, otherwise the first violated rule.
[[nodiscard]] const char* ValidatePassSettings(const PassSettings& settings) noexcept;
// The hierarchy is built (and allocated) only for the footprint or the cache
// on a radial schedule.
[[nodiscard]] bool PassUsesHierarchy(const PassSettings& settings) noexcept;
// M8: any stage takes shading normals (the gather's smooth-shaded receiver or
// a smooth-crease rule): the adapter writes them and the pass allocates them.
[[nodiscard]] bool PassUsesShadingNormals(const PassSettings& settings) noexcept;
// The M8 smooth-shaded receiver in every stage (`on`), or the M7 pass exactly
// (the option of the document's "M8", R5): gather, temporal, filter, upsampler.
void SetPassShadingNormals(PassSettings& settings, bool on) noexcept;
// FNV-1a 64 over every transport field (R, tau, S, N, schedule, K, e, jitter,
// masks, tolerances, rotation sequence, M4 switches, hierarchy, normal
// source). Its change resets the history (kResetTransport); temporal, filter
// and upsample settings are not part of it.
[[nodiscard]] uint64_t PassTransportKey(const PassSettings& settings, GeometricNormalSource normals) noexcept;

inline constexpr uint32_t kMaxPassWorkFactor = 4;

struct PassExtent {
    uint32_t BufferWidth = 0, BufferHeight = 0; // extent of the per-texel inputs (the work image)
    ViewportRect Viewport;                      // pixels the GI runs on (inside the buffers)
    // Work scale: 1 writes the outputs at the buffer extent; 2 or 4 writes
    // them at the output extent through the upsampler (the viewport is then
    // the whole buffer and the buffer is the work image of the output).
    uint32_t WorkFactor = 1;
    uint32_t WorkPhase = 0;
    uint32_t OutputWidth = 0, OutputHeight = 0; // 0 with WorkFactor 1 (the buffer extent)
};
[[nodiscard]] const char* ValidatePassExtent(const PassExtent& extent) noexcept;
// The extent of a pass that runs at 1 / factor of an output of this size
// (factor 1: the output itself, no padding).
[[nodiscard]] PassExtent MakeWorkScaleExtent(uint32_t outputWidth, uint32_t outputHeight, uint32_t factor) noexcept;
// Width and height of the output images.
[[nodiscard]] std::array<uint32_t, 2> PassOutputExtent(const PassExtent& extent) noexcept;
// The camera of the work image from the output's camera (same encoding, near,
// far; ray map composed with u_out = (f w / W) u_work + (o + 1/2 - f/2) / W).
// Factor 1 returns the camera unchanged.
[[nodiscard]] DepthCamera WorkScaleCamera(const DepthCamera& output, const PassExtent& extent) noexcept;
// Multipliers that turn a normalized output-UV displacement into a normalized
// work-UV displacement: W / (f w) and H / (f h) (exactly 1 when f divides the
// output extent and o = 0).
[[nodiscard]] std::array<double, 2> WorkMotionScale(const PassExtent& extent) noexcept;

// Optional per-pixel inputs the adapter provides; declared at Configure.
struct PassInputLayout {
    bool PreviousPositions = false; // HistoryValidation::PreviousPosition
    bool SurfaceIds = false;
};

// ---------------------------------------------------------------------------
// Resources. T = buffer texels, P = viewport pixels, C = hierarchy cells of
// all levels (about 4/3 P), O = output texels (work scale only). Every buffer
// is pass-owned device memory except the two output images, which the caller
// (the effect graph) owns.

enum class PassResourceId : uint8_t {
    // Inputs (written by the adapter before Execute).
    Depth,            // float per texel: window depth in the declared encoding
    Class,            // uint per texel: TexelClass
    Radiance,         // vec4 per texel: linear outgoing radiance (rgb)
    Normal,           // vec4 per texel: face normal in (RendererFaceNormal), oriented geometric normal out
    Motion,           // vec2 per pixel: declared motion convention
    PreviousPosition, // vec4 per pixel: previous view-space position (PreviousPosition only)
    SurfaceId,        // uint per pixel (optional, 0 = unknown)
    OutputGuide,      // uvec2 per output texel (work scale): view depth bits, normal code
    ShadingNormal,    // M8: vec4 per texel: renderer shading normal in, oriented by the normal pass (w = 1 valid)
    OutputShading,    // M8: uint per output texel (work scale): shading normal code
    // Frame transients.
    Cells,            // 3 vec4 per cell: plane + flags/count, normal + ZMin, mean radiance + ZMax
    Results,          // vec4 per pixel: raw J, A raw
    Aux,              // vec4 per pixel: C, validity, scheduled, occupied sectors
    Geometry,         // vec4 per pixel: receiver normal, view depth (0: not a receiver)
    Colour,           // vec4 per pixel: accumulated J, A raw (pre-filter)
    Variance,         // vec2 per pixel: Var J, Var A of the estimate
    Diag,             // vec4 per pixel: alpha, alpha A, length, length A
    Diag2,            // vec4 per pixel: z J, z A, confidence, flags
    Filter0, Filter1, // vec4 per pixel: a-trous ping-pong (J, A raw)
    FilterVariance0, FilterVariance1, // vec2 per pixel
    Shading,          // M8: vec4 per pixel: the receivers' oriented shading normals (pack -> temporal, filter, upsample)
    // Persistent (per view).
    History0, History1, // 4 vec4 per pixel (kHistoryValues): ping-pong histories
    // Static.
    Parameters,  // uniform buffer image of PassParameterLayout
    Placeholder, // 16 bytes bound where a kernel declares data this pass never uses (SH, guide, noise)
    Count,
};
inline constexpr uint32_t kPassResourceCount = static_cast<uint32_t>(PassResourceId::Count);

enum class PassLifetime : uint8_t {
    Input,      // written by the adapter each frame, read by the pass in the same frame
    Frame,      // produced and consumed within one Execute
    Persistent, // carried to the next frame of the same view
    Static,     // written by the pass itself (parameters, placeholder)
};

struct PassResource {
    PassResourceId Id = PassResourceId::Count;
    const char* Name = "";
    const char* Layout = "";    // element type in the kernels
    uint32_t ElementBytes = 0;
    uint64_t Elements = 0;
    uint64_t Bytes = 0;         // 0 when not allocated
    PassLifetime Lifetime = PassLifetime::Frame;
    const char* Access = "";    // producer -> consumers
    bool Allocated = false;
};

struct PassOutputImage {
    const char* Name = "";
    const char* Format = "";
    uint32_t TexelBytes = 0;
    uint64_t Bytes = 0;
};

struct PassResourcePlan {
    PassExtent Extent;
    std::array<PassResource, kPassResourceCount> Buffers{};
    std::array<PassOutputImage, 2> Outputs{}; // DiffuseIllumination (J) and accessibility (A), caller-owned
    uint32_t HierarchyCells = 0;
    [[nodiscard]] uint64_t Bytes(PassLifetime lifetime) const noexcept;
    [[nodiscard]] uint64_t PassBytes() const noexcept;   // every buffer the pass allocates
    [[nodiscard]] uint64_t OutputBytes() const noexcept; // the caller's images
    [[nodiscard]] const PassResource& operator[](PassResourceId id) const noexcept {
        return Buffers[static_cast<size_t>(id)];
    }
};

[[nodiscard]] PassResourcePlan PlanPassResources(const PassExtent& extent, const PassSettings& settings,
                                                 const PassInputLayout& inputs, uint32_t uniformAlignment);

// ---------------------------------------------------------------------------
// Uniform parameter buffer: every constant block of a frame at offsets aligned
// to the device's uniform offset alignment (dynamic offsets select the
// hierarchy level, the filter level and the output mode). Written once per
// frame by the command stream (vkCmdUpdateBuffer), so frames in flight never
// see a later frame's constants.

struct PassParameterLayout {
    uint32_t Alignment = 256;
    uint32_t Gather = 0;                              // GatherShaderParams
    std::array<uint32_t, kMaxHierarchyLevels> M4{};   // M4ShaderParams with Build.x = level
    uint32_t Temporal = 0;                            // TemporalShaderParams
    std::array<uint32_t, kMaxFilterLevels> Filter{};  // FilterShaderParams with step 2^level
    uint32_t Output = 0;                              // OutputShaderParams, final mode
    uint32_t OutputNeutral = 0;                       // OutputShaderParams, neutral mode
    uint32_t Upsample = 0;                            // UpsampleShaderParams (work scale)
    uint32_t Size = 0;
};
// `uniformAlignment` is minUniformBufferOffsetAlignment (a power of two);
// blocks start at multiples of max(it, 16).
[[nodiscard]] PassParameterLayout MakePassParameterLayout(uint32_t uniformAlignment) noexcept;

// ---------------------------------------------------------------------------
// Frames.

struct PassFrame {
    // Encoding, near, far, UV -> ray map of the OUTPUT image of this frame
    // (jitter included); with a work scale the pass derives the work camera.
    DepthCamera Camera;
    GeometricNormalSource NormalSource = GeometricNormalSource::DepthReconstruction;
    uint64_t FrameIndex = 0; // drives the rotation sequence
    uint32_t ViewId = 0;
    bool CameraCut = false;
    bool ExplicitReset = false;
    double PreExposure = 1.0;
    double JitterX = 0.0, JitterY = 0.0; // output pixels, positive along +u / +v
    RigidTransform CurrentToPrevious;    // view to previous view
    MotionDeclaration Motion;            // of the motion the adapter wrote (work-image UV)
    HistoryValidation Validation = HistoryValidation::FlowAndDepth;
    bool SurfaceIds = false; // the adapter wrote this frame's surface ids
};

[[nodiscard]] const char* ValidatePassFrame(const PassFrame& frame, const PassInputLayout& inputs) noexcept;

struct PassFramePlan {
    uint32_t ResetReasons = 0;
    TemporalFrameState Temporal; // committed once the frame is recorded
    DepthCamera WorkCamera;      // the camera of the pass's own image
    bool Hierarchy = false;
    uint32_t HierarchyLevels = 0;
    std::array<std::array<uint32_t, 2>, kMaxHierarchyLevels> LevelExtent{}; // cells per level
    uint32_t FilterLevels = 0;
    GatherShaderParams Gather{};
    std::array<M4ShaderParams, kMaxHierarchyLevels> M4{};
    TemporalShaderParams TemporalParams{};
    std::array<FilterShaderParams, kMaxFilterLevels> Filter{};
    OutputShaderParams Output{}, OutputNeutral{};
    UpsampleShaderParams Upsample{};
    std::vector<uint8_t> Parameters; // PassParameterLayout::Size bytes, every block in place
};

// Plans one frame (settings already validated). `history` is read, not
// changed: the caller commits plan.Temporal after recording the frame.
// `writeAccessibility` selects whether the output kernels store A.
[[nodiscard]] const char* PlanPassFrame(const PassExtent& extent, const PassSettings& settings,
                                        const PassInputLayout& inputs, const PassParameterLayout& layout,
                                        const TemporalHistoryState& history, const PassFrame& frame,
                                        PassFramePlan& plan, bool writeAccessibility = true);

// The output block of a mode over the output images (the buffer extent with
// work factor 1, the output extent otherwise).
[[nodiscard]] OutputShaderParams MakeOutputShaderParams(const PassExtent& extent, uint32_t mode,
                                                        bool writeAccessibility = true) noexcept;

// Work of one frame: dispatches and the private compute barriers between
// them (two leading barriers: producers -> this frame, parameter update ->
// uniform reads).
struct PassWorkload {
    uint32_t Dispatches = 0;
    uint32_t PrivateBarriers = 0;
};
[[nodiscard]] PassWorkload PassFrameWorkload(const PassSettings& settings) noexcept;

} // namespace Fast::Renderer::DiffuseGi
