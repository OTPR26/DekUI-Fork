#pragma once

// Diffuse screen-space GI: radiometric contract and independent reference
// integrator (milestone M1 of docs/TRIAEVUM_SCREEN_SPACE_GI.md).
//
// Title-neutral and Vulkan-free. Everything here follows the written
// specification "Specifica per una GI diffusa in screen space" v1.2,
// sections 1-3 and 12, and the derivations recorded in the document above.
// The double-precision quadrature is the reference; the analytic sector
// weights (double and FP32) are production candidates checked against it.

#include <cstdint>
#include <string_view>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

inline constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
    double X = 0.0, Y = 0.0, Z = 0.0;
};
[[nodiscard]] double Dot(const Vec3& a, const Vec3& b) noexcept;
[[nodiscard]] Vec3 Cross(const Vec3& a, const Vec3& b) noexcept;
[[nodiscard]] Vec3 Normalize(const Vec3& v) noexcept;
[[nodiscard]] Vec3 Scale(const Vec3& v, double s) noexcept;
[[nodiscard]] Vec3 Add(const Vec3& a, const Vec3& b) noexcept;

// Linear RGB in relative radiometric units (the contract allows relative
// units as long as they are consistent; see section 2).
struct Rgb {
    double R = 0.0, G = 0.0, B = 0.0;
    bool operator==(const Rgb&) const = default;
};

// ---------------------------------------------------------------------------
// Input contract (section 1). The adapter declares every convention; the
// module never infers gamma, depth scale or motion direction.

enum class DepthEncoding : uint8_t {
    Unknown,
    PerspectiveZeroToOne,         // window depth 0 at near, 1 at far
    ReversedPerspectiveZeroToOne, // window depth 1 at near, 0 at far
    LinearWBuffer,                // near + depth * (far - near)
    ReversedLinearWBuffer,        // far - depth * (far - near)
};
enum class ViewForward : uint8_t { Unknown, NegativeZ, PositiveZ };
enum class VectorSpace : uint8_t { Unknown, View, World };
enum class MotionDirection : uint8_t { Unknown, CurrentToPrevious, PreviousToCurrent };
enum class MotionUnits : uint8_t { Unknown, NormalizedViewportUv, Pixels };
enum class JitterInMotion : uint8_t { Unknown, Included, Excluded };
// LinearHdr is the verification reference. LdrApproximation is the separate
// profile for display-encoded or clipped sources: never calibrated radiance.
enum class RadianceProfile : uint8_t { Unknown, LinearHdr, LdrApproximation };
enum class ColorEncoding : uint8_t { Unknown, Linear, Srgb };
enum class ExposurePolicy : uint8_t { Unknown, None, PreExposed };

struct InputDeclaration {
    DepthEncoding Depth = DepthEncoding::Unknown;
    ViewForward Forward = ViewForward::Unknown;
    // Must share the space of the reconstructed positions (view space).
    VectorSpace GeometricNormal = VectorSpace::Unknown;
    // Unknown: the geometric normal drives the diffuse lobe (first version).
    VectorSpace ShadingNormal = VectorSpace::Unknown;
    MotionDirection Motion = MotionDirection::Unknown;
    MotionUnits MotionUnit = MotionUnits::Unknown;
    JitterInMotion Jitter = JitterInMotion::Unknown;
    RadianceProfile Source = RadianceProfile::Unknown;
    ColorEncoding SourceEncoding = ColorEncoding::Unknown;
    // The source is outgoing radiance, already multiplied by its material.
    // Irradiance must first be converted with SourceRadianceFromIrradiance.
    bool SourceIsOutgoingRadiance = false;
    ColorEncoding ReceiverAlbedoEncoding = ColorEncoding::Unknown;
    ExposurePolicy Exposure = ExposurePolicy::Unknown;
    bool SkyDistinguishable = false;
    bool UnavailableDistinguishable = false;
};

// Empty when the declaration is complete and consistent, otherwise the first
// violated requirement.
[[nodiscard]] std::string_view ValidateInputDeclaration(const InputDeclaration& declaration) noexcept;
[[nodiscard]] bool IsCalibratedRadiance(const InputDeclaration& declaration) noexcept;

// ---------------------------------------------------------------------------
// Radiometric contract (section 2). J is diffuse irradiance divided by pi,
// before the receiver albedo; the renderer applies rho_d exactly once.

// Lambertian conversion of direct irradiance into outgoing radiance; after it
// neither the source albedo nor the direct-light cosine is applied again.
[[nodiscard]] Rgb SourceRadianceFromIrradiance(const Rgb& directIrradiance, const Rgb& sourceAlbedo) noexcept;
// L_ind = rho_d (component-wise) J.
[[nodiscard]] Rgb ComposeIndirect(const Rgb& receiverAlbedo, const Rgb& j) noexcept;

// C_out = L_direct,d + L_spec + L_e + A L_ambient,d + rho_d J_local.
struct NativeCompositionTerms {
    Rgb DirectDiffuse;
    Rgb Specular;
    Rgb Emission;
    Rgb AmbientDiffuse; // already outgoing diffuse radiance
    Rgb ReceiverAlbedo;
    Rgb LocalJ;         // excludes the ambient term above
    double Accessibility = 1.0;
};
[[nodiscard]] Rgb ComposeNative(const NativeCompositionTerms& terms) noexcept;

// ---------------------------------------------------------------------------
// Section parameterization (section 3): v is the unit vector from the receiver
// to the camera, t(phi) = e1 cos(phi) + e2 sin(phi), phi in [0, pi), theta in
// [-pi, pi), omega = v cos(theta) + t sin(theta), d omega = |sin theta|.

struct SectionFrame {
    Vec3 V, E1, E2;
    // Deterministic orthonormal completion of a unit v.
    [[nodiscard]] static SectionFrame FromView(const Vec3& v) noexcept;
    [[nodiscard]] Vec3 Tangent(double phi) const noexcept;
    [[nodiscard]] Vec3 Direction(double phi, double theta) const noexcept;
};

// Weight of the angular interval [lo, hi] (subset of [-pi, pi]) of a section
// whose receiver normal has a = n.v and b = n.t(phi):
//   (1/pi) * integral max(0, a cos t + b sin t) |sin t| dt.
// Closed form derived in the document (own derivation); the FP32 variant is
// the production candidate.
[[nodiscard]] double SectorWeightAnalytic(double a, double b, double lo, double hi) noexcept;
[[nodiscard]] float SectorWeightAnalyticF32(float a, float b, float lo, float hi) noexcept;

// Total section weight M_phi (closed form): (r/pi)(cos g + g sin g), with
// r = |(a, b)| and g = min(|atan2(b, a)|, pi - |atan2(b, a)|).
[[nodiscard]] double SectionMass(double a, double b) noexcept;

// Interval of any position on the circle (hi - lo <= 2 pi): intervals that
// cross +-pi are split first (section 12).
[[nodiscard]] double IntervalWeight(double a, double b, double lo, double hi) noexcept;
// Normalized cumulative C_phi(theta) on [-pi, pi]; zero when M_phi is zero
// (every contribution of such a section is zero; nothing is divided).
[[nodiscard]] double Cumulative(double a, double b, double theta) noexcept;

// Reference: Gauss-Legendre quadrature in double precision, doubling the
// order of every piece until two successive orders agree.
struct QuadratureResult {
    double Value = 0.0;
    uint32_t Pieces = 0;
    uint32_t MaxOrder = 0;       // highest order needed by any piece
    double LastCorrection = 0.0; // largest |Q(m) - Q(m/2)| at acceptance
    bool Converged = false;
};
struct QuadratureOptions {
    double RelativeTolerance = 1.0e-14;
    double AbsoluteTolerance = 1.0e-17;
    uint32_t MaxOrder = 256;
    // Split at the kinks of the integrand (theta = 0 and the lobe edges). The
    // unsplit variant exists only to measure the rejected alternative.
    bool SplitAtKinks = true;
};
[[nodiscard]] QuadratureResult SectorWeightQuadrature(double a, double b, double lo, double hi,
                                                      const QuadratureOptions& options = {});

// Nodes and weights of the order-m Gauss-Legendre rule on [-1, 1] (Newton
// iteration on Legendre polynomials, double precision).
struct GaussLegendreRule {
    std::vector<double> Nodes;
    std::vector<double> Weights;
};
[[nodiscard]] const GaussLegendreRule& GaussLegendre(uint32_t order);

// ---------------------------------------------------------------------------
// Synthetic angular fields (no depth buffer). L_i is the radiance of the first
// represented element in a direction, or the declared fallback.

enum class DirectionState : uint8_t {
    Hit,          // a represented element: occluded, its radiance is L_i
    Open,         // observable and free of represented occluders
    Unobservable, // treated as open (optimistic AO fallback), lowers C
};

struct FieldSample {
    DirectionState State = DirectionState::Open;
    Rgb Radiance;     // outgoing radiance toward the receiver (Hit only)
    Rgb SourceAlbedo; // diagnostic metadata: never used by the transport
};

class AngularField {
  public:
    virtual ~AngularField() = default;
    [[nodiscard]] virtual FieldSample Sample(const Vec3& omega) const = 0;
    // Angles in (-pi, pi] where Sample changes along the section spanned by
    // v and t. Optional: used by the continuous reference to split pieces.
    virtual void SectionBreakpoints(const Vec3& v, const Vec3& t, std::vector<double>& thetas) const;
};

// Ordered spherical caps {omega : axis . omega >= cos(halfAngle)}; the first
// containing cap decides, otherwise the background. A half-space is a cap of
// half angle pi/2.
class CapField final : public AngularField {
  public:
    struct Cap {
        Vec3 Axis;
        double HalfAngle = 0.0;
        FieldSample Inside;
    };
    explicit CapField(FieldSample background, std::vector<Cap> caps = {});
    [[nodiscard]] FieldSample Sample(const Vec3& omega) const override;
    void SectionBreakpoints(const Vec3& v, const Vec3& t, std::vector<double>& thetas) const override;
    // Same geometry with every radiance multiplied by `factor`.
    [[nodiscard]] CapField Scaled(double factor) const;

  private:
    FieldSample mBackground;
    std::vector<Cap> mCaps;
};

// ---------------------------------------------------------------------------
// Estimators.

struct Estimate {
    Rgb J;              // raw estimate: never clamped, never renormalized
    double ARaw = 0.0;  // raw accessibility; may exceed 1 with few sections
    double A = 0.0;     // clamp(ARaw, 0, 1), for composition only
    double Confidence = 0.0; // valid / scheduled candidates; diagnostic only
    uint64_t Sections = 0;
    uint64_t Candidates = 0;
    uint64_t ValidCandidates = 0;
};

// Per-section contribution F(phi) = sum_j w_j(phi) L_j and its accessibility
// counterpart, with the density p(phi) used to draw phi.
struct SectionContribution {
    Rgb F;
    double FA = 0.0;
    double Pdf = 1.0 / kPi;
    uint64_t Candidates = 0;
    uint64_t ValidCandidates = 0;
};

// J_hat = (1/S) sum_s F(phi_s) / p(phi_s). Reset() returns to the initial
// state: no previous contribution survives.
class SectionAccumulator {
  public:
    void Reset() noexcept;
    void Add(const SectionContribution& contribution) noexcept;
    [[nodiscard]] Estimate Result() const noexcept;

  private:
    Rgb mSum;
    double mSumA = 0.0;
    uint64_t mSections = 0;
    uint64_t mCandidates = 0;
    uint64_t mValid = 0;
};

struct SectorEstimatorSettings {
    uint32_t Sections = 16;  // S, uniform p = 1/pi
    uint32_t Sectors = 32;   // N, uniform in theta over [-pi, pi)
    double Rotation = 0.5;   // phi_s = pi (s + Rotation) / S, Rotation in [0, 1)
    Rgb Fallback;            // radiance of open directions (zero: no local GI)
};

// Sector estimator: each sector takes the field at its centre direction (the
// deterministic centre rule); weights are the analytic sector weights.
[[nodiscard]] SectionContribution EvaluateSection(const AngularField& field, const SectionFrame& frame,
                                                  const Vec3& normal, double phi, uint32_t sectors,
                                                  const Rgb& fallback);
[[nodiscard]] Estimate EstimateSectors(const AngularField& field, const SectionFrame& frame, const Vec3& normal,
                                       const SectorEstimatorSettings& settings);
// FP32 production candidate: frame, weights and accumulation in float.
[[nodiscard]] Estimate EstimateSectorsF32(const AngularField& field, const SectionFrame& frame,
                                          const Vec3& normal, const SectorEstimatorSettings& settings);

// Continuous reference: J = integral over phi of the exact section integral,
// without sectors (measures the sector quantization error). Outer composite
// Gauss-Legendre with doubling panels; inner pieces split at the integrand
// kinks and the field breakpoints.
struct ContinuousOptions {
    double Tolerance = 1.0e-9; // relative, on every channel and on A
    uint32_t PanelOrder = 8;
    uint32_t MaxPanels = 8192;
    QuadratureOptions Inner{1.0e-13, 1.0e-16, 256, true};
};
struct ContinuousResult {
    Rgb J;
    double A = 0.0;
    uint32_t Panels = 0;
    uint32_t MaxInnerOrder = 0;
    double LastCorrection = 0.0;
    bool Converged = false;
};
[[nodiscard]] ContinuousResult IntegrateContinuous(const AngularField& field, const SectionFrame& frame,
                                                   const Vec3& normal, const Rgb& fallback,
                                                   const ContinuousOptions& options = {});

} // namespace Fast::Renderer::DiffuseGi
