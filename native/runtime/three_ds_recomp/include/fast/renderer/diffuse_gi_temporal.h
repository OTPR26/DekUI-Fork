#pragma once

// Diffuse screen-space GI, milestone M3: stratum reconstruction, temporal
// accumulation and the edge-aware a-trous filter (docs/TRIAEVUM_SCREEN_SPACE_GI.md,
// "M3"; specification sections 7, 8, 9 and 11, tests T11-T16).
//
// Title-neutral and Vulkan-free. Precision::Double is the reference;
// Precision::Float32 is the twin of the GLSL kernels (diffuse_gi_temporal_shader.h):
// it reads the same std140 constant blocks and follows the kernels operation by
// operation, so CPU/GPU differences can be separated from FP32 rounding.
//
// Pipeline per frame and view:
//   raw gather (S sections per pixel, per-pixel rotations of the sequence)
//   -> stratum reconstruction (geometry-gated window that weights every
//      stratum of the rotation tile equally)
//   -> temporal accumulation of J (luminance moments) and of A (own history),
//      reprojected through the declared motion; THIS is the history
//   -> a-trous filter (variance-driven), never fed back into the history.
// Conventions (all declared by the adapter, never inferred):
// - Viewport pixels as in diffuse_gi_gather.h; camera rays from the affine
//   UV -> ray map (ViewX, ViewY) of each frame, jitter included in that map.
// - Motion: previous UV = current UV + motion (or the declared variants);
//   jitter in pixels, positive along +u/+v: the jittered projection of a point
//   is its unjittered projection plus the jitter.
// - Pre-exposed signal: J inputs are multiplied by PreExposure.

#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_reference.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

// Rigid transform between view spaces, x' = R x + T (R row-major, orthonormal).
struct RigidTransform {
    std::array<double, 9> R{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
    std::array<double, 3> T{};
    [[nodiscard]] Vec3 Apply(const Vec3& x) const noexcept;
    [[nodiscard]] Vec3 Rotate(const Vec3& x) const noexcept;
};

// History validation (specification section 7). PreviousPosition: the adapter
// supplies the previous view-space position of every current surface point
// (camera and object motion), so the expected previous surface is known
// exactly. FlowAndDepth: only motion, depth and the camera transform; the
// module transforms the current surface into the previous camera assuming it
// is static. Where the flow disagrees with that prediction (a moving surface)
// only a weaker test is possible: depth within a relative band and normals;
// such histories get a reduced confidence (a capped history length).
enum class HistoryValidation : uint8_t { PreviousPosition, FlowAndDepth };

// Pre-exposure change: rescale history and moments, or invalidate them.
enum class ExposureChange : uint8_t { Rescale, Invalidate };

// Reset reasons (bit set reported per frame).
inline constexpr uint32_t kResetFirstFrame = 1U << 0U;
inline constexpr uint32_t kResetCameraCut = 1U << 1U;
inline constexpr uint32_t kResetViewChange = 1U << 2U;
inline constexpr uint32_t kResetResize = 1U << 3U;
inline constexpr uint32_t kResetExplicit = 1U << 4U;    // includes resource invalidation
inline constexpr uint32_t kResetTransport = 1U << 5U;   // transport settings changed
inline constexpr uint32_t kResetExposure = 1U << 6U;    // invalid or invalidated exposure change
inline constexpr uint32_t kResetDisabled = 1U << 7U;    // temporal accumulation off

struct MotionDeclaration {
    MotionDirection Direction = MotionDirection::CurrentToPrevious;
    MotionUnits Units = MotionUnits::NormalizedViewportUv;
    JitterInMotion Jitter = JitterInMotion::Included;
};

// One frame of one view. Per-pixel arrays are row-major over the viewport.
struct TemporalFrame {
    uint32_t Width = 0, Height = 0;
    // Frame state (specification section 1).
    uint64_t FrameIndex = 0;
    uint32_t ViewId = 0;
    bool CameraCut = false;
    bool ExplicitReset = false;
    double PreExposure = 1.0;
    uint64_t TransportKey = 0; // any change of R, tau, S, N, schedule... resets
    // Receivers: view depth z > 0 where the gather is valid, 0 elsewhere
    // (sky, unavailable, no geometric normal); geometric normal, 3 per pixel.
    std::vector<double> Depth;
    std::vector<double> Normal;
    // Raw gather: J.r, J.g, J.b, A raw per pixel (pre-exposed J).
    std::vector<double> Raw;
    // Camera of this frame (jitter included in the ray map) and the camera
    // motion into the previous frame's view space.
    std::array<double, 3> ViewX{}, ViewY{};
    double JitterX = 0.0, JitterY = 0.0; // pixels
    RigidTransform CurrentToPrevious;
    MotionDeclaration Motion;
    std::vector<double> MotionVectors; // 2 per pixel, declared convention
    HistoryValidation Validation = HistoryValidation::FlowAndDepth;
    std::vector<double> PreviousPosition; // 3 per pixel (PreviousPosition only)
    std::vector<uint32_t> SurfaceId;      // optional, 0 = unknown
    // M8 (optional): the oriented shading normal, 3 per pixel (zero: none);
    // read by TemporalSettings::ShadingNormals.
    std::vector<double> ShadingNormal;
};

[[nodiscard]] const char* ValidateTemporalFrame(const TemporalFrame& frame) noexcept;

struct TemporalSettings {
    bool Enabled = true;        // off: every frame stands alone (alpha = 1)
    bool Reconstruction = true; // stratum reconstruction (tiles of 2 x 2 and larger)
    uint32_t TileLog2 = 2;      // must equal the rotation sequence's tile
    double AlphaMin = 1.0 / 32.0;
    double VarianceFrames = 4.0; // [B3] section 4.2: spatial variance below 4 frames
    // Light response: z = |l(x) - l(H)| / sqrt(s^2 (1 + v) + floor^2) raises
    // alpha linearly from ChangeLow to ChangeHigh.
    double ChangeLow = 3.0, ChangeHigh = 6.0; // chosen by LightResponseThresholdStudy
    double ChangeFloor = 0.02;  // relative to max(l(x), l(H)) (J)
    double ChangeFloorA = 0.01; // absolute (A)
    // Surface compatibility: |n_h . (X_h - P')| <= PlaneSine |X_h - P'| + PlaneDepth z',
    // n_h . n' >= NormalCos. Shared by reconstruction and reprojection.
    double PlaneSine = 0.05;
    double PlaneDepth = 1.0e-3;
    double NormalCos = 0.9;
    // FlowAndDepth only.
    double MovingFlow = 0.25;       // pixels between the flow and the static prediction
    double MovingDepth = 0.05;      // relative depth band of the weaker test
    double MovingConfidence = 0.25; // history length cap, fraction of 1 / AlphaMin
    ExposureChange Exposure = ExposureChange::Rescale;
    uint32_t Threads = 1;
    // M8 smooth-shaded receivers (document "M8"; off: M3-M7 exactly). Two
    // samples whose surface test fails are still one surface when they form a
    // smooth crease: their shading normals agree within NormalCos and turn
    // less than their face normals (1 - cos of the shading normals below
    // HardEdgeRatio times 1 - cos of the face normals; 1/2 is about 0.71 of the angle),
    // and the two face planes meet between them (each sample within (sine +
    // sine of the face angle) |d| + depth z of the other's plane). Used by the
    // stratum reconstruction and the history taps (the history keeps the
    // shading normal's code in value 11 when there are no surface ids).
    bool ShadingNormals = false;
    double HardEdgeRatio = 0.5; // in angle about 0.71 (document "M8")
};

[[nodiscard]] const char* ValidateTemporalSettings(const TemporalSettings& settings) noexcept;

struct TemporalPixel {
    bool Receiver = false;     // geometry with a valid gather
    bool Valid = false;        // an estimate exists (current sample or history)
    Rgb J;                     // accumulated J (pre-filter, = history)
    double A = 1.0;            // accumulated raw A
    double VarianceJ = 0.0;    // variance of the luminance estimate
    double VarianceA = 0.0;
    double Alpha = 1.0, AlphaA = 1.0;
    double Length = 0.0, LengthA = 0.0; // effective sample counts (1 / alpha)
    double ChangeJ = 0.0, ChangeA = 0.0; // z statistics of the light response
    bool HistoryAccepted = false;
    bool ChangeResponse = false; // the light response raised alpha above its base
    bool Moving = false;       // FlowAndDepth: flow differs from the static prediction
    double Confidence = 0.0;   // 1 full validation, MovingConfidence weak, 1 fresh
    bool CurrentValid = false;
    Rgb Current;               // reconstructed current sample
    double CurrentA = 1.0;
    // Diagnostics of the reprojection (CPU only): previous pixel coordinate
    // (continuous, centres at i + 1/2; NaN when no history was looked up),
    // the top-left tap (floor(x - 1/2), floor(y - 1/2)) and the mask of the
    // accepted taps (bit a + 2 b for tap (a, b)).
    double PreviousX = 0.0, PreviousY = 0.0;
    int32_t TapX = 0, TapY = 0;
    uint32_t TapMask = 0;
};

struct TemporalCounters {
    uint64_t Receivers = 0, Accepted = 0, Rejected = 0, Moving = 0;
    uint64_t ChangeResponses = 0; // alpha raised by the light response (J)
    uint64_t InvalidSamples = 0;  // non-finite raw at a receiver
};

struct TemporalResult {
    uint32_t ResetReasons = 0;
    std::vector<TemporalPixel> Pixels;
    TemporalCounters Counters;
};

enum class Precision : uint8_t { Double, Float32 };

// std140 constants of the temporal kernel; the FP32 twin reads the same block.
inline constexpr uint32_t kTemporalFlagReset = 1U;
inline constexpr uint32_t kTemporalFlagShading = 2U; // M8: the smooth-surface paths
struct TemporalShaderParams {
    std::array<uint32_t, 4> Extent{};    // width, height, flags (reset, shading), window radius
    std::array<uint32_t, 4> Modes{};     // tile size, reconstruct, validation (1 flow and depth), surface ids
    std::array<float, 4> ViewX{}, ViewY{};         // current ray map (ax, bx, cx, 0)
    std::array<float, 4> PreviousX{}, PreviousY{}; // previous ray map
    std::array<float, 4> Rotation0{}, Rotation1{}, Rotation2{}; // rows of R, w = T
    std::array<float, 4> Motion{};       // motion to pixels x, y (sign included), jitter compensation x, y
    std::array<float, 4> Accumulate{};   // alpha min, variance frames, exposure ratio, 1 / alpha min
    std::array<float, 4> Change{};       // z low, z high, J floor (relative), A floor (absolute)
    std::array<float, 4> Geometry{};     // plane sine, plane depth, normal cos, hard-edge ratio (M8)
    std::array<float, 4> Moving{};       // moving flow, moving depth, moving confidence, 0
};
static_assert(sizeof(TemporalShaderParams) == 224);

// History layout: 16 values per pixel (GPU: four vec4).
//  0-2 J, 3 m2 (second moment of luminance), 4 A, 5 second moment of A,
//  6 length J, 7 length A, 8 v J, 9 v A (sum of squared sample weights),
//  10 confidence, 11 surface id (as a float, exact below 2^24) or, with M8
//  shading normals and no surface ids, the shading normal's code
//  (diffuse_gi_normal_code.h; the bits of the float on the GPU and in the FP32
//  twin, the integer value in the double reference), 12-14 normal, 15 view
//  depth.
inline constexpr uint32_t kHistoryValues = 16;

// Frame-level part of a TemporalFrame (M5): everything the reset rules and
// the constant block depend on, without per-pixel data. The GPU pass keeps
// its per-pixel data in buffers and plans each frame from this state.
struct TemporalFrameState {
    uint32_t Width = 0, Height = 0;
    uint64_t FrameIndex = 0;
    uint32_t ViewId = 0;
    bool CameraCut = false;
    bool ExplicitReset = false;
    double PreExposure = 1.0;
    uint64_t TransportKey = 0;
    std::array<double, 3> ViewX{}, ViewY{};
    double JitterX = 0.0, JitterY = 0.0;
    RigidTransform CurrentToPrevious;
    MotionDeclaration Motion;
    HistoryValidation Validation = HistoryValidation::FlowAndDepth;
    bool SurfaceIds = false; // per-pixel surface ids are provided
};

[[nodiscard]] TemporalFrameState TemporalStateOf(const TemporalFrame& frame) noexcept;
// The frame-level rules of ValidateTemporalFrame (same order and messages).
[[nodiscard]] const char* ValidateTemporalFrameState(const TemporalFrameState& frame) noexcept;

// Reset rules of one view's history (specification section 7): the single
// owner of the decisions TemporalAccumulator and the M5 GPU pass apply.
// Prepare decides a frame without changing the state; Commit records the
// frame once its history has been written.
class TemporalHistoryState {
  public:
    struct Plan {
        uint32_t ResetReasons = 0;
        bool Reset = true;
        double ExposureRatio = 1.0; // 1 on reset
        std::array<double, 3> PreviousViewX{}, PreviousViewY{}; // the current map on reset
        double PreviousJitterX = 0.0, PreviousJitterY = 0.0;
    };
    [[nodiscard]] Plan Prepare(const TemporalFrameState& frame, const TemporalSettings& settings) const noexcept;
    void Commit(const TemporalFrameState& frame) noexcept;
    // Resource invalidation or explicit reset: the next frame resets (kResetExplicit).
    void Invalidate() noexcept;
    [[nodiscard]] bool HasHistory() const noexcept { return mInitialized; }

  private:
    bool mInitialized = false;
    bool mInvalidated = false;
    uint32_t mWidth = 0, mHeight = 0, mViewId = 0;
    uint64_t mTransportKey = 0;
    double mPreExposure = 1.0;
    std::array<double, 3> mViewX{}, mViewY{};
    double mJitterX = 0.0, mJitterY = 0.0;
};

// The FP32 constant block of the temporal kernel for a planned frame: the
// block TemporalAccumulator::LastShaderParams() holds after Process().
[[nodiscard]] TemporalShaderParams MakeTemporalShaderParams(const TemporalFrameState& frame,
                                                            const TemporalSettings& settings,
                                                            const TemporalHistoryState::Plan& plan);

class TemporalAccumulator {
  public:
    explicit TemporalAccumulator(Precision precision = Precision::Double);
    ~TemporalAccumulator();
    TemporalAccumulator(TemporalAccumulator&&) noexcept;
    TemporalAccumulator& operator=(TemporalAccumulator&&) noexcept;
    TemporalAccumulator(const TemporalAccumulator&) = delete;
    TemporalAccumulator& operator=(const TemporalAccumulator&) = delete;

    [[nodiscard]] TemporalResult Process(const TemporalFrame& frame, const TemporalSettings& settings);
    // Resource invalidation: the next frame resets (kResetExplicit).
    void Invalidate() noexcept;
    [[nodiscard]] bool HasHistory() const noexcept;
    // Current history (after the last Process), kHistoryValues per pixel.
    [[nodiscard]] std::vector<double> History() const;
    // Constants of the last frame (FP32 block, also filled for Double).
    [[nodiscard]] const TemporalShaderParams& LastShaderParams() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// ---------------------------------------------------------------------------
// Edge-aware a-trous filter (specification section 8; [B6] eqs. 1-4 and
// [B3] eq. 1 and the variance propagation of section 4.3). Kernel h = B3
// spline (1, 4, 6, 4, 1) / 16 per axis with holes 2^i; a_pq = h(q) g(p, q) w(p, q),
// g the hard surface compatibility above, w the colour weight
//   exp(-d / (sigma sqrt(Var_p + Var_q) + eps (l_p + l_q) / 2)),
// d = sum_c k_c |J_p,c - J_q,c| (luminance-weighted L1 colour distance, so an
// equal-luminance chromatic edge still stops the filter). A has its own weight
// and variance. J' = sum a J / sum a, Var' = sum a^2 Var / (sum a)^2.

struct FilterSettings {
    uint32_t Levels = 4; // 0: no spatial filter
    double LuminanceSigma = 4.0;
    double RelativeEpsilon = 0.01;
    double PlaneSine = 0.05;
    double PlaneDepth = 1.0e-3;
    double NormalCos = 0.9;
    uint32_t Threads = 1;
    // M8: the smooth-crease rule of TemporalSettings for the a-trous taps.
    bool ShadingNormals = false;
    double HardEdgeRatio = 0.5; // in angle about 0.71 (document "M8")
};

[[nodiscard]] const char* ValidateFilterSettings(const FilterSettings& settings) noexcept;

struct FilterInput {
    uint32_t Width = 0, Height = 0;
    std::array<double, 3> ViewX{}, ViewY{};
    std::vector<double> Depth;  // view depth, 0 = not a receiver
    std::vector<double> Normal; // 3 per pixel
    std::vector<double> J;      // 3 per pixel
    std::vector<double> A;
    std::vector<double> VarianceJ, VarianceA;
    std::vector<double> ShadingNormal; // M8 (optional): 3 per pixel (zero: none)
};

struct FilteredPixel {
    bool Receiver = false;
    Rgb J;
    double ARaw = 1.0;
    double A = 1.0; // clamp(ARaw, 0, 1), for composition only
    double VarianceJ = 0.0, VarianceA = 0.0;
};

inline constexpr uint32_t kMaxFilterLevels = 8;
struct FilterShaderParams {
    std::array<uint32_t, 4> Extent{}; // width, height, step, 0
    std::array<float, 4> ViewX{}, ViewY{};
    std::array<float, 4> Weights{};   // luminance sigma, relative epsilon, plane sine, plane depth
    std::array<float, 4> Normals{};   // normal cos, hard-edge ratio, shading normals (1 / 0), 0 (M8)
};
static_assert(sizeof(FilterShaderParams) == 80);
[[nodiscard]] FilterShaderParams MakeFilterShaderParams(const FilterInput& input, const FilterSettings& settings,
                                                        uint32_t level);

[[nodiscard]] FilterInput MakeFilterInput(const TemporalFrame& frame, const TemporalResult& temporal);
[[nodiscard]] std::vector<FilteredPixel> FilterImage(const FilterInput& input, const FilterSettings& settings,
                                                     Precision precision = Precision::Double);

// ---------------------------------------------------------------------------
// Helpers.

// Rec. 709 luminance of linear RGB (declared; J is never converted otherwise).
inline constexpr double kLumaR = 0.2126, kLumaG = 0.7152, kLumaB = 0.0722;
[[nodiscard]] double Luminance(const Rgb& c) noexcept;

// Adapter-side assembly from the M2 gather of the same frame: receivers,
// normals, raw J and A, and the camera ray map. Motion, frame state and
// previous positions are left to the caller.
[[nodiscard]] TemporalFrame MakeTemporalFrame(const GatherInput& input, const PreparedGeometry& geometry,
                                              const std::vector<PixelGather>& gather);

} // namespace Fast::Renderer::DiffuseGi
