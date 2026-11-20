#pragma once

// Diffuse screen-space GI, milestone M2: gathering on a depth buffer and sector
// masks (docs/TRIAEVUM_SCREEN_SPACE_GI.md; specification sections 1, 4, 9 and
// 10, tests T05-T09).
//
// Title-neutral and Vulkan-free. The double-precision functions are the
// reference. GatherImageF32 is an FP32 twin that follows the GPU kernel
// (diffuse_gi_gather_shader.h) operation by operation, so CPU/GPU differences
// can be separated from FP32 rounding.
//
// Conventions (all declared by the adapter, never inferred):
// - View space: camera at the origin, forward -Z, positive view depth z = -Z.
// - Viewport pixel coordinates: texel (i, j) covers [i, i+1) x [j, j+1); its
//   centre is (i + 0.5, j + 0.5). Normalized viewport UV = pixel / extent.
// - Camera rays: X/z = ViewX . (u, v, 1), Y/z = ViewY . (u, v, 1); this affine
//   map carries storage orientation and off-centre projection.
// - Depth is decoded with the declared DepthEncoding, near and far planes.
// - TexelClass distinguishes geometry (one- or two-sided), sky and unavailable
//   pixels; the module does not infer it from colour or depth.

#include "fast/renderer/diffuse_gi_reference.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kMaxSectors = 256;
inline constexpr uint32_t kMaskWords = kMaxSectors / 32;

enum class TexelClass : uint8_t {
    Unavailable = 0, // no data (HUD, outside the composed surface, invalid)
    OneSided = 1,    // opaque geometry emitting only from its visible side
    TwoSided = 2,    // geometry emitting from both sides
    Sky = 3,         // no geometry along the camera ray
};

// Source of the geometric normal (section 1): a renderer-provided face normal
// or the reconstruction from depth implemented here.
enum class GeometricNormalSource : uint8_t { DepthReconstruction, RendererFaceNormal };

struct DepthCamera {
    DepthEncoding Encoding = DepthEncoding::PerspectiveZeroToOne;
    double Near = 0.1;
    // +infinity is accepted only with ReversedPerspectiveZeroToOne.
    double Far = 100.0;
    std::array<double, 3> ViewX{};
    std::array<double, 3> ViewY{};
};

struct ViewportRect {
    uint32_t X = 0, Y = 0, Width = 0, Height = 0;
};

struct GatherInput {
    uint32_t BufferWidth = 0, BufferHeight = 0;
    // Texels outside the viewport are never read.
    ViewportRect Viewport;
    DepthCamera Camera;
    std::vector<float> Depth;    // BufferWidth * BufferHeight, encoded per Camera
    std::vector<uint8_t> Class;  // TexelClass per texel
    std::vector<float> Radiance; // 3 floats per texel: linear outgoing radiance
    GeometricNormalSource NormalSource = GeometricNormalSource::DepthReconstruction;
    // RendererFaceNormal only: 3 floats per texel, view space. Oriented toward
    // the camera by the module (the visible side is the front side).
    std::vector<float> FaceNormals;
    // M8 (optional): the renderer's shading normal, 3 floats per texel, view
    // space (the normal the title's own lighting shades the texel with, e.g.
    // interpolated vertex normals). Oriented to the side of the geometric
    // normal by the module; a zero vector means none (the geometric normal
    // stands in). Read only by GatherSettings::ShadingNormals.
    std::vector<float> ShadingNormals;
};

// Empty when the input is consistent, otherwise the first violated rule.
[[nodiscard]] const char* ValidateGatherInput(const GatherInput& input) noexcept;
// The camera rules of ValidateGatherInput (same order and messages).
[[nodiscard]] const char* ValidateDepthCamera(const DepthCamera& camera) noexcept;

enum class CandidateSchedule : uint8_t {
    // Reference: every texel crossed by the section line inside the projected
    // radius, represented by the line point nearest to the texel centre.
    EveryTexel,
    // Production candidate: K positions per side at screen distances
    // lambda_k = lambda_0 + (L - lambda_0) ((k + jitter) / K)^exponent.
    Radial,
    // M4 radial distribution in scene units: K positions per side on the
    // camera rays of the section plane whose closest distance to the receiver
    // is rho_k = R ((k + jitter) / K)^exponent (the ray at angle
    // asin(rho / |P|) from the receiver's ray; rho = R is the exact bound of
    // the R-ball, so the positions scale with the scene, not with the pixel
    // resolution). Positions closer than the contact texel (one texel along the
    // dominant screen axis) are moved onto it, so no jitter value skips the
    // contact region. Documented in the M4 section of the document.
    MetricRadial,
};

// M4 guiding and A (specification section 6): IndependentUniform estimates A
// on S additional sections at the unguided angles pi (s + u) / S with the
// uniform density (A is then exactly the unguided estimator; the section work
// doubles); SamePdf weights the open weights of the guided sections by the
// same 1 / p_b as J (unbiased, no extra sections).
enum class GuidedAccessibility : uint8_t { IndependentUniform, SamePdf };

// M4 radiance cache (specification section 5): Off reads the source radiance
// of the level-0 texel (point reference); Mean reads the prefiltered mean of
// the planar hierarchy cell of the sample's footprint level; SphericalHarmonics
// evaluates the cell's low-order SH representation toward the receiver
// (diffuse_gi_hierarchy.h). Both caches act only on planar cells (one surface).
enum class CacheMode : uint8_t { Off, Mean, SphericalHarmonics };

enum class MaskMode : uint8_t {
    CentreRule,          // uniform theta sectors, sector-centre coverage (baseline)
    ContinuousIntervals, // exact union of occupied intervals (reference only)
};

// ---------------------------------------------------------------------------
// Per-pixel, per-frame section rotation (milestone M3, [B2]-derived
// requirement 1). Fixed: every pixel uses GatherSettings::Rotation (the
// deterministic, noise-free mode of M1/M2). Stratified: the strata
// k in [0, M), M = T^2 F, of a T x T pixel tile (T = 2^TileLog2) and a cycle of
// F frames are assigned so that
//   - every frame, the T^2 pixels of each tile take all T^2 coarse strata
//     c = floor(k / F): c = ((b XOR e) + s) mod T^2, b the recursive
//     ordered-dither index of the tile position, e = Frame mod 2 (T >= 2),
//     s = 2 bitrev(((Frame mod T^2) / 2)) over log2(T^2 / 2) bits;
//   - within each frame pair the strata move by (T/2, T/2) pixels (b XOR 1
//     flips the most significant position bits): with the stratum
//     reconstruction window of diffuse_gi_temporal.h this cancels, for 2 x 2
//     and 4 x 4 tiles, the first-order term by which neighbours' strata
//     leave a tile-periodic error; over T^2 frames every stratum visits every
//     tile position once, which cancels it at every order (own derivation,
//     document);
//   - the fine stratum is t = Frame mod F: any F consecutive frames of a tile
//     cover all M = T^2 F strata exactly once (bijection).
// u = (k + 1/2) / M; the stratified rotations of one tile over F consecutive
// frames are exactly the deterministic rotations of S = M sections. Tiles are
// anchored at the viewport origin. Own construction (document, M3 section).
// BlueNoise (M4): u = (rank + 1/2) / N from an own spatiotemporal blue-noise
// mask (diffuse_gi_noise.h) at (x mod W, y mod H, Frame mod D); the radial
// jitter reads the same slice at (x + W/2, y + H/2) ([B4] section 5.2: a scalar
// mask read at two offsets gives two values). Needs GatherAcceleration::Noise.
enum class RotationMode : uint8_t { Fixed, Stratified, BlueNoise };

inline constexpr uint32_t kMaxRotationTileLog2 = 3; // 8 x 8 pixels
inline constexpr uint32_t kMaxRotationCycle = 64;

struct RotationSequence {
    RotationMode Mode = RotationMode::Fixed;
    uint32_t TileLog2 = 2; // [B2]: 4 x 4 pixels
    uint32_t Cycle = 7;    // M3 decision: coprime with 16 ([B2] used 6)
    uint64_t Frame = 0;    // only Frame mod Cycle matters
};

// Recursive ordered-dither (Bayer) index of a tile position, in [0, T^2): the
// bit pairs (x_k, y_k) map to 0, 2, 3, 1 for (0,0), (1,0), (0,1), (1,1), the
// lowest pair weighted most, so neighbouring pixels differ by large indices.
[[nodiscard]] uint32_t TileOrder(uint32_t x, uint32_t y, uint32_t tileLog2) noexcept;
[[nodiscard]] uint32_t StratumCount(const RotationSequence& sequence) noexcept; // M
// s = 2 bitrev((Frame mod T^2) / 2) (0 for a 1 x 1 tile).
[[nodiscard]] uint32_t FrameShift(const RotationSequence& sequence) noexcept;
// 1 on odd frames with tiles of 2 x 2 or larger, else 0.
[[nodiscard]] uint32_t FrameFlip(const RotationSequence& sequence) noexcept;
// k = F (((TileOrder XOR flip) + shift) mod T^2) + t for viewport pixel (x, y).
[[nodiscard]] uint32_t StratumIndex(const RotationSequence& sequence, uint32_t x, uint32_t y) noexcept;
// Multiplier of the second (radial-jitter) dimension: the integer coprime to M
// nearest to M (sqrt(5) - 1) / 2 (ties: the smaller); rank-1 lattice
// (k / M, frac(k P / M)), each projection a perfect stratification.
[[nodiscard]] uint32_t LatticeMultiplier(uint32_t strata) noexcept;
// 1 / M rounded to float: the FP32 twin and the GPU multiply by it.
[[nodiscard]] float StratumScaleF32(const RotationSequence& sequence) noexcept;

struct GatherSettings {
    double Radius = 1.0;    // R, scene units: finite and positive
    double Thickness = 0.5; // tau, scene units, along the camera ray behind q
    uint32_t Sections = 8;  // S; phi_s = pi (s + Rotation) / S, p = 1/pi
    uint32_t Sectors = 64;  // N in [1, kMaxSectors]; independent of S (default: sector study)
    double Rotation = 0.5;
    CandidateSchedule Schedule = CandidateSchedule::EveryTexel;
    uint32_t CandidatesPerSide = 16; // Radial only
    double RadialExponent = 2.0;     // Radial only, >= 1
    double RadialJitter = 0.5;       // Radial only, in [0, 1)
    MaskMode Mask = MaskMode::CentreRule;
    // Self-intersection: a candidate whose texel-centre point lies within
    // SelfIntersectionSine * |Q - P| + SelfIntersectionDepth * z_P above the
    // receiver's tangent plane (or below it) is the receiver's own surface.
    bool RejectSelfIntersection = true;
    double SelfIntersectionSine = 2.0e-3;
    double SelfIntersectionDepth = 1.0e-5;
    // Baseline interleaves both sides by screen distance (ties: + side first).
    // false processes the whole + side first; it exists to test the tie-break.
    bool InterleaveSides = true;
    // Texel chords shorter than this (pixels) are corner touches: skipped.
    double ChordEpsilon = 1.0e-3;
    uint32_t Threads = 1; // image gather only
    // M3: per-pixel rotation sequence (Fixed keeps Rotation for every pixel)
    // and, with Stratified, optionally the per-pixel radial jitter of the
    // Radial schedule from the lattice dimension (replaces RadialJitter).
    RotationSequence Rotations;
    bool SequenceRadialJitter = false;
    // M4 optimizations, each off by default and individually switchable; with
    // all of them off the M1-M3 code paths run unchanged. The hierarchy, cache
    // and guide data come from GatherAcceleration (built by the caller).
    // Footprint level of a radial sample: the largest l <= HierarchyMaxLevel
    // with 2^l <= HierarchySpacingScale x (its spacing to the next sample along
    // the line, pixels); then the descent: while the level-l cell holding the
    // sample's texel is not planar, or holds the receiver's texel, l decreases
    // (level 0 is the M2 point reference).
    // HierarchyFootprint: a sample on a planar cell above level 0 represents
    // the cell's footprint along the line: the chord of the section line
    // through the cell, on the cell's plane (exact depth, no raster footprint
    // error), clipped to the R-ball; it occupies the angular range of that
    // strip and of its thickness segments (own construction, document "M4").
    bool HierarchyFootprint = false;
    CacheMode Cache = CacheMode::Off;
    uint32_t HierarchyMaxLevel = 4;     // footprint levels used by the gather (<= hierarchy levels - 1)
    double HierarchySpacingScale = 1.0; // chosen by HierarchyFootprintStudy (document)
    // Guiding: phi_s = P^-1((s + u) / S) of the region's frozen distribution
    // p = (1 - eta) / pi + eta g, J weighted by 1 / p(phi_s); A as declared.
    bool Guiding = false;
    GuidedAccessibility GuidingAccessibility = GuidedAccessibility::IndependentUniform; // GuidingParameterStudy
    // M8 smooth-shaded receivers (document "M8"; off: the M2-M7 receiver
    // exactly). The receiver's hemisphere is the one of its shading normal
    // (GatherInput::ShadingNormals): the cosine lobe (a = n.v, b = n.t of the
    // M1 weights) uses it, and a candidate is visible when its texel-centre
    // point lies above the shading tangent plane (the M2 tolerance), off the
    // receiver's own facet (the face plane, M2 tolerance) and off the
    // receiver's own smooth surface. Own smooth surface: an adjacent facet,
    // i.e. a candidate whose shading normal agrees with the receiver's within
    // OwnSurfaceCos, whose shading turns less than its face (1 - cos of the
    // shading normals below HardEdgeRatio times 1 - cos of the face normals:
    // HardEdgeRatio 1/2 is about 0.71 of the angle), and whose face plane meets the
    // receiver's between them (each point within (sine + sine of the face
    // angle) |d| + depth z of the other's face plane). Directions between the
    // face plane and the shading tangent plane are therefore open to every
    // other surface (no shading-normal terminator); hard edges (the shading
    // turns with the face) and distinct surfaces keep their contact.
    bool ShadingNormals = false;
    double OwnSurfaceCos = 0.9;
    double HardEdgeRatio = 0.5; // in angle about 0.71 (document "M8")
};

[[nodiscard]] const char* ValidateGatherSettings(const GatherSettings& settings) noexcept;

// Rotation u and radial jitter of viewport pixel (x, y): (k + 1/2) / M in
// double; (float(k) + 0.5f) * StratumScaleF32 in the FP32 twin and the GPU.
[[nodiscard]] double PixelRotation(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept;
[[nodiscard]] float PixelRotationF32(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept;
[[nodiscard]] double PixelRadialJitter(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept;
[[nodiscard]] float PixelRadialJitterF32(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept;

struct GatherCounters {
    uint64_t Scheduled = 0;   // candidates programmed (denominator of C)
    uint64_t Valid = 0;       // geometry or sky with a decodable state (numerator of C)
    uint64_t Sky = 0;
    uint64_t SelfRejected = 0;
    uint64_t BeyondRadius = 0;
    uint64_t Degenerate = 0;  // segment meeting the receiver: rejected
    uint64_t BackFacing = 0;  // one-sided source seen from behind: occupies, emits zero
    uint64_t MissingSourceNormal = 0;
    uint64_t Duplicates = 0;  // Radial positions falling in an already used texel
    uint64_t Occupying = 0;   // candidates with a non-empty interval
    // M4 (zero unless a hierarchy or cache switch is on).
    uint64_t Coarse = 0;      // candidates evaluated on a hierarchy cell above level 0
    uint64_t Descended = 0;   // level steps taken because a cell was not one plane
    uint64_t Contact = 0;     // MetricRadial positions moved onto the contact texel
    GatherCounters& operator+=(const GatherCounters& other) noexcept;
};

struct PixelGather {
    bool Valid = false; // receiver is geometry with a usable geometric normal
    Rgb J;              // raw estimate, never clamped or renormalized
    double ARaw = 1.0;
    double A = 1.0;     // clamp(ARaw, 0, 1)
    double Confidence = 0.0; // Valid / Scheduled; 0 when nothing was scheduled
    uint64_t OccupiedSectors = 0;
    GatherCounters Counters;
};

// Whole viewport, row-major (Viewport.Width * Viewport.Height). Non-geometry
// receivers get the neutral result J = 0, A = 1, C = 0, Valid = false.
[[nodiscard]] std::vector<PixelGather> GatherImage(const GatherInput& input, const GatherSettings& settings);
[[nodiscard]] std::vector<PixelGather> GatherImageF32(const GatherInput& input, const GatherSettings& settings);

// ---------------------------------------------------------------------------
// Geometry preparation (double): decoded view depth, effective class and
// geometric normal per viewport texel.

struct PreparedGeometry {
    uint32_t Width = 0, Height = 0;
    std::vector<double> ViewDepth;        // 0 when not geometry
    std::vector<uint8_t> Class;           // effective TexelClass
    std::vector<Vec3> Normal;             // zero vector when unavailable
    std::vector<Vec3> Shading;            // M8: oriented shading normal (zero: none); empty without input
    [[nodiscard]] Vec3 Ray(const DepthCamera& camera, double px, double py) const noexcept;
    [[nodiscard]] Vec3 Position(const DepthCamera& camera, uint32_t x, uint32_t y) const noexcept;
};
[[nodiscard]] PreparedGeometry PrepareGeometry(const GatherInput& input);
// FP32 twin of the preparation (values widened to double), for diagnosis and
// CPU/GPU comparisons of the normal reconstruction.
[[nodiscard]] PreparedGeometry PrepareGeometryF32(const GatherInput& input);
[[nodiscard]] double DecodeViewDepth(double encoded, const DepthCamera& camera) noexcept;

// ---------------------------------------------------------------------------
// Section-level reference (double), exposed for the mask, order and oracle
// tests.

struct SectionSetup {
    bool Valid = false;
    Vec3 P, V, T, Normal; // receiver, unit view vector, unit section tangent, integration normal
    Vec3 Face;            // M8: the geometric (visibility) normal; equals Normal without shading normals
    double NV = 0.0, NT = 0.0; // a = n.v and b = n.t(phi) of the M1 weights
    double DirX = 0.0, DirY = 0.0; // unit pixel direction of +t on screen
    double Extent[2] = {0.0, 0.0}; // projected radius in pixels: [0] + side, [1] - side
};

struct Candidate {
    double Lambda = 0.0; // screen distance from the receiver centre (pixels)
    int Side = 1;        // +1 or -1
    int32_t CellX = 0, CellY = 0; // texel offset from the receiver
    uint32_t Pieces = 0; // 0: empty interval; 2: split at +-pi
    double Lo[2] = {0.0, 0.0}, Hi[2] = {0.0, 0.0};
    Rgb Radiance; // evaluated toward the receiver (zero when back-facing)
    Vec3 Front, Back; // view-space segment endpoints after clipping to R
    uint32_t Level = 0; // M4: footprint level of the sample (0: point reference)
    int32_t HX = 0, HY = 0; // M4: hierarchy cell at that level (absolute)
};

struct SectionCandidates {
    SectionSetup Setup;
    std::vector<Candidate> Ordered; // occupying or empty-interval geometry, in processing order
    GatherCounters Counters;
};

[[nodiscard]] SectionSetup SetupSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                        uint32_t y, double phi, const GatherSettings& settings);
[[nodiscard]] SectionCandidates BuildSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                             uint32_t y, double phi, const GatherSettings& settings);
// FP32 twin of BuildSection (prepares its own FP32 geometry; values widened).
[[nodiscard]] SectionCandidates BuildSectionF32(const GatherInput& input, uint32_t x, uint32_t y, float phi,
                                                const GatherSettings& settings);

struct SectorMask {
    std::array<uint32_t, kMaskWords> Words{};
    bool operator==(const SectorMask&) const = default;
};

// Centre rule: sector j (centre -pi + (j + 1/2) 2 pi / N) is covered by
// [lo, hi) iff lo <= centre < hi. Returns [first, end) clamped to [0, N];
// empty when hi <= lo.
[[nodiscard]] std::pair<uint32_t, uint32_t> CentreRuleRange(double lo, double hi, uint32_t sectors) noexcept;
[[nodiscard]] std::pair<uint32_t, uint32_t> CentreRuleRangeF32(float lo, float hi, uint32_t sectors) noexcept;
// Sets bits [first, end) across words; never shifts by the word width.
void SetSectorRange(SectorMask& mask, uint32_t first, uint32_t end) noexcept;
// Interval of any position with hi - lo < 2 pi (split at +-pi first).
[[nodiscard]] SectorMask CoverInterval(double lo, double hi, uint32_t sectors) noexcept;
[[nodiscard]] uint32_t CountSectors(const SectorMask& mask) noexcept;
// Boundary angle of sector index j in [0, N]: -pi + j 2 pi / N, exactly pi at N.
[[nodiscard]] double SectorBoundary(uint32_t j, uint32_t sectors) noexcept;
// Sum of the M1 weights of the set bits, evaluated per contiguous run.
[[nodiscard]] double MaskWeight(const SectorMask& mask, uint32_t sectors, double nv, double nt) noexcept;

struct SectionResult {
    Rgb F;          // sum_j w_j L_j over occupied sectors
    double FA = 0.0; // sum of the open weights
    SectorMask Mask; // CentreRule only
    std::vector<int32_t> Claimant; // CentreRule: candidate index per sector, -1 open
    std::vector<std::pair<double, double>> Occupied; // ContinuousIntervals only
};
[[nodiscard]] SectionResult AccumulateSection(const SectionSetup& setup, const std::vector<Candidate>& ordered,
                                              uint32_t sectors, MaskMode mode);

// One receiver, all sections (double reference).
[[nodiscard]] PixelGather GatherPixel(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                      uint32_t y, const GatherSettings& settings);

// ---------------------------------------------------------------------------
// M4: optimization inputs of one frame (docs/TRIAEVUM_SCREEN_SPACE_GI.md,
// "M4"). Built by the caller and frozen while the frame samples:
// - Hierarchy: depth hierarchy and radiance cache of this frame's buffers
//   (diffuse_gi_hierarchy.h), built from PrepareGeometry for the double path
//   and with float arithmetic from PrepareGeometryF32 for the FP32 twin;
// - Guide: the frozen guiding distribution per screen region
//   (diffuse_gi_guiding.h); Samples, when not null, receives one record per
//   viewport pixel and section (pixel-major) for the update after the frame;
// - Noise: the spatiotemporal blue-noise mask of RotationMode::BlueNoise
//   (diffuse_gi_noise.h).
struct DepthHierarchy;
struct GuideTable;
struct GuideSample;
struct NoiseMask;

struct GatherAcceleration {
    const DepthHierarchy* Hierarchy = nullptr;
    const GuideTable* Guide = nullptr;
    const NoiseMask* Noise = nullptr;
    std::vector<GuideSample>* Samples = nullptr;
};

// Empty when every M4 switch of `settings` has the data it needs and the data
// matches the viewport, otherwise the first violated rule.
[[nodiscard]] const char* ValidateGatherAcceleration(const GatherInput& input, const GatherSettings& settings,
                                                     const GatherAcceleration& acceleration) noexcept;

[[nodiscard]] std::vector<PixelGather> GatherImage(const GatherInput& input, const GatherSettings& settings,
                                                   const GatherAcceleration& acceleration);
[[nodiscard]] std::vector<PixelGather> GatherImageF32(const GatherInput& input, const GatherSettings& settings,
                                                      const GatherAcceleration& acceleration);
// One receiver (double); Samples, when set, receives this pixel's S records.
[[nodiscard]] PixelGather GatherPixel(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                      uint32_t y, const GatherSettings& settings,
                                      const GatherAcceleration& acceleration);
// Section-level reference with the M4 inputs (candidates carry their level).
[[nodiscard]] SectionCandidates BuildSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                             uint32_t y, double phi, const GatherSettings& settings,
                                             const GatherAcceleration& acceleration);
// Scheduled positions of one section before evaluation (double), in
// processing order: the schedule tests (contact region, footprint levels).
struct ScheduledPosition {
    double Lambda = 0.0;
    int Side = 1;
    int32_t CellX = 0, CellY = 0; // texel offset from the receiver
    uint32_t Level = 0;           // after the descent
    uint32_t Requested = 0;       // from the spacing, before the descent
    double Spacing = 0.0;         // pixels to the next sample (last: to the previous)
};
[[nodiscard]] std::vector<ScheduledPosition> ScheduleSection(const GatherInput& input,
                                                             const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                                                             double phi, const GatherSettings& settings,
                                                             const GatherAcceleration& acceleration);

} // namespace Fast::Renderer::DiffuseGi
