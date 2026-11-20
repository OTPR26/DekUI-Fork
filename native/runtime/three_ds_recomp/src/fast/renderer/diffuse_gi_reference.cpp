#include "fast/renderer/diffuse_gi_reference.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <type_traits>

namespace Fast::Renderer::DiffuseGi {
namespace {

template <typename T>
constexpr T kPiT = static_cast<T>(kPi);

// Delta - sin(Delta) without cancellation for small intervals: Taylor series
// below the threshold (truncation far below the type's epsilon), direct
// difference above it.
template <typename T>
T DeltaMinusSin(T delta) {
    const T threshold = std::is_same_v<T, float> ? T(0.5) : T(0.1);
    if (delta < threshold) {
        const T d2 = delta * delta;
        return delta * d2 *
               (T(1) / T(6) -
                d2 * (T(1) / T(120) - d2 * (T(1) / T(5040) - d2 * (T(1) / T(362880) - d2 / T(39916800)))));
    }
    return delta - std::sin(delta);
}

// integral_p^q (a cos t + b sin t) sin t dt for q >= p (own derivation):
//   1/2 [ b (D - sin D cos S) + a sin D sin S ],  D = q - p, S = q + p,
// with D - sin D cos S = (D - sin D) + 2 sin D sin^2(S / 2).
template <typename T>
T SliceIntegral(T a, T b, T p, T q) {
    const T delta = q - p;
    const T sum = q + p;
    const T sinDelta = std::sin(delta);
    const T halfSin = std::sin(sum * T(0.5));
    return T(0.5) * (b * (DeltaMinusSin(delta) + T(2) * sinDelta * halfSin * halfSin) + a * sinDelta * std::sin(sum));
}

// (1/pi) integral_lo^hi max(0, a cos t + b sin t) |sin t| dt on [-pi, pi]:
// intersect with the positive lobe [g - pi/2, g + pi/2] (wrapped into the
// domain) and with the two half circles where sin t has a constant sign.
template <typename T>
T AnalyticWeight(T a, T b, T lo, T hi) {
    const T pi = kPiT<T>;
    lo = std::max(lo, -pi);
    hi = std::min(hi, pi);
    if (!(hi > lo) || !(a * a + b * b > T(0))) return T(0);
    const T gamma = std::atan2(b, a);
    const T halfPi = pi * T(0.5);
    const T l0 = gamma - halfPi;
    const T l1 = gamma + halfPi;
    T lobes[2][2]{};
    int lobeCount = 0;
    if (l0 < -pi) {
        lobes[0][0] = -pi, lobes[0][1] = l1;
        lobes[1][0] = l0 + T(2) * pi, lobes[1][1] = pi;
        lobeCount = 2;
    } else if (l1 > pi) {
        lobes[0][0] = l0, lobes[0][1] = pi;
        lobes[1][0] = -pi, lobes[1][1] = l1 - T(2) * pi;
        lobeCount = 2;
    } else {
        lobes[0][0] = l0, lobes[0][1] = l1;
        lobeCount = 1;
    }
    T total = T(0);
    for (int lobe = 0; lobe < lobeCount; ++lobe) {
        for (int side = 0; side < 2; ++side) {
            const T sideLo = side == 0 ? -pi : T(0);
            const T sideHi = side == 0 ? T(0) : pi;
            const T p = std::max(std::max(lo, lobes[lobe][0]), sideLo);
            const T q = std::min(std::min(hi, lobes[lobe][1]), sideHi);
            if (q > p) {
                const T piece = SliceIntegral(a, b, p, q);
                total += side == 0 ? -piece : piece;
            }
        }
    }
    return total / pi;
}

double WrapAngle(double angle) {
    // Into (-pi, pi].
    angle = std::remainder(angle, 2.0 * kPi);
    return angle <= -kPi ? angle + 2.0 * kPi : angle;
}

double KinkIntegrand(double a, double b, double theta) {
    return std::max(0.0, a * std::cos(theta) + b * std::sin(theta)) * std::abs(std::sin(theta)) / kPi;
}

GaussLegendreRule BuildGaussLegendre(uint32_t order) {
    GaussLegendreRule rule;
    rule.Nodes.resize(order);
    rule.Weights.resize(order);
    const auto legendre = [order](double x, double& value, double& derivative) {
        double previous = 1.0;
        double current = x;
        for (uint32_t k = 2; k <= order; ++k) {
            const double next = ((2.0 * k - 1.0) * x * current - (k - 1.0) * previous) / k;
            previous = current;
            current = next;
        }
        value = current;
        derivative = order * (x * current - previous) / (x * x - 1.0);
    };
    for (uint32_t i = 0; i < (order + 1) / 2; ++i) {
        double x = std::cos(kPi * (i + 0.75) / (order + 0.5));
        double value = 0.0, derivative = 1.0;
        for (int iteration = 0; iteration < 100; ++iteration) {
            legendre(x, value, derivative);
            const double step = value / derivative;
            x -= step;
            if (std::abs(step) <= 1.0e-16) break;
        }
        legendre(x, value, derivative);
        const double weight = 2.0 / ((1.0 - x * x) * derivative * derivative);
        rule.Nodes[i] = -x;
        rule.Nodes[order - 1 - i] = x;
        rule.Weights[i] = weight;
        rule.Weights[order - 1 - i] = weight;
    }
    return rule;
}

template <typename Function>
double GaussLegendreIntegral(const GaussLegendreRule& rule, double p, double q, const Function& function) {
    const double half = 0.5 * (q - p);
    const double middle = 0.5 * (q + p);
    double sum = 0.0;
    for (size_t i = 0; i < rule.Nodes.size(); ++i) sum += rule.Weights[i] * function(middle + half * rule.Nodes[i]);
    return sum * half;
}

void AppendLobeEdges(double a, double b, std::vector<double>& cuts) {
    if (!(a * a + b * b > 0.0)) return;
    const double gamma = std::atan2(b, a);
    cuts.push_back(WrapAngle(gamma - 0.5 * kPi));
    cuts.push_back(WrapAngle(gamma + 0.5 * kPi));
}

std::vector<double> Pieces(double lo, double hi, std::vector<double> cuts) {
    std::vector<double> points{lo, hi};
    for (double cut : cuts)
        if (cut > lo && cut < hi) points.push_back(cut);
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

struct Vec3T {
    template <typename T>
    struct Of {
        T X, Y, Z;
    };
};

template <typename T>
typename Vec3T::template Of<T> Cast(const Vec3& v) {
    return {static_cast<T>(v.X), static_cast<T>(v.Y), static_cast<T>(v.Z)};
}

template <typename T>
T DotT(const typename Vec3T::template Of<T>& a, const typename Vec3T::template Of<T>& b) {
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

struct SectionSums {
    double R = 0.0, G = 0.0, B = 0.0, A = 0.0;
};

template <typename T>
struct SectionContributionT {
    T R = T(0), G = T(0), B = T(0), A = T(0);
    uint64_t Candidates = 0;
    uint64_t Valid = 0;
};

template <typename T>
SectionContributionT<T> EvaluateSectionT(const AngularField& field, const SectionFrame& frame, const Vec3& normal,
                                          double phi, uint32_t sectors, const Rgb& fallback) {
    SectionContributionT<T> out;
    if (sectors == 0U) return out;
    const auto v = Cast<T>(frame.V);
    const auto e1 = Cast<T>(frame.E1);
    const auto e2 = Cast<T>(frame.E2);
    const auto n = Cast<T>(normal);
    const T cosPhi = std::cos(static_cast<T>(phi));
    const T sinPhi = std::sin(static_cast<T>(phi));
    const typename Vec3T::template Of<T> t{e1.X * cosPhi + e2.X * sinPhi, e1.Y * cosPhi + e2.Y * sinPhi,
                                           e1.Z * cosPhi + e2.Z * sinPhi};
    const T a = DotT<T>(n, v);
    const T b = DotT<T>(n, t);
    const T pi = kPiT<T>;
    const T step = T(2) * pi / static_cast<T>(sectors);
    for (uint32_t j = 0; j < sectors; ++j) {
        const T lo = -pi + step * static_cast<T>(j);
        const T hi = j + 1U == sectors ? pi : -pi + step * static_cast<T>(j + 1U);
        const T weight = AnalyticWeight<T>(a, b, lo, hi);
        if (!(weight > T(0))) continue;
        ++out.Candidates;
        const T centre = T(0.5) * (lo + hi);
        const T c = std::cos(centre);
        const T s = std::sin(centre);
        const Vec3 omega{static_cast<double>(v.X * c + t.X * s), static_cast<double>(v.Y * c + t.Y * s),
                         static_cast<double>(v.Z * c + t.Z * s)};
        const FieldSample sample = field.Sample(omega);
        if (sample.State != DirectionState::Unobservable) ++out.Valid;
        const bool hit = sample.State == DirectionState::Hit;
        const Rgb& radiance = hit ? sample.Radiance : fallback;
        out.R += weight * static_cast<T>(radiance.R);
        out.G += weight * static_cast<T>(radiance.G);
        out.B += weight * static_cast<T>(radiance.B);
        if (!hit) out.A += weight;
    }
    return out;
}

} // namespace

double Dot(const Vec3& a, const Vec3& b) noexcept { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
Vec3 Scale(const Vec3& v, double s) noexcept { return {v.X * s, v.Y * s, v.Z * s}; }
Vec3 Add(const Vec3& a, const Vec3& b) noexcept { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
Vec3 Normalize(const Vec3& v) noexcept {
    const double length = std::sqrt(Dot(v, v));
    return length > 0.0 ? Scale(v, 1.0 / length) : Vec3{};
}

std::string_view ValidateInputDeclaration(const InputDeclaration& d) noexcept {
    if (d.Depth == DepthEncoding::Unknown) return "depth encoding is undeclared";
    if (d.Forward == ViewForward::Unknown) return "view forward axis is undeclared";
    if (d.GeometricNormal != VectorSpace::View)
        return "the geometric normal must be declared in view space, the space of the reconstructed positions";
    if (d.Motion == MotionDirection::Unknown) return "motion direction is undeclared";
    if (d.MotionUnit == MotionUnits::Unknown) return "motion units are undeclared";
    if (d.Jitter == JitterInMotion::Unknown) return "jitter in motion is undeclared";
    if (d.Source == RadianceProfile::Unknown) return "radiance profile is undeclared";
    if (d.SourceEncoding == ColorEncoding::Unknown) return "source encoding is undeclared";
    if (d.Source == RadianceProfile::LinearHdr && d.SourceEncoding != ColorEncoding::Linear)
        return "a calibrated HDR source must be linear";
    if (!d.SourceIsOutgoingRadiance) return "the source must be outgoing radiance: convert irradiance first";
    if (d.ReceiverAlbedoEncoding == ColorEncoding::Unknown) return "receiver albedo encoding is undeclared";
    if (d.Exposure == ExposurePolicy::Unknown) return "exposure policy is undeclared";
    if (!d.SkyDistinguishable) return "sky must be distinguishable from geometry";
    if (!d.UnavailableDistinguishable) return "unavailable pixels must be distinguishable";
    return {};
}

bool IsCalibratedRadiance(const InputDeclaration& d) noexcept {
    return ValidateInputDeclaration(d).empty() && d.Source == RadianceProfile::LinearHdr;
}

Rgb SourceRadianceFromIrradiance(const Rgb& e, const Rgb& albedo) noexcept {
    return {albedo.R * e.R / kPi, albedo.G * e.G / kPi, albedo.B * e.B / kPi};
}

Rgb ComposeIndirect(const Rgb& albedo, const Rgb& j) noexcept {
    return {albedo.R * j.R, albedo.G * j.G, albedo.B * j.B};
}

Rgb ComposeNative(const NativeCompositionTerms& t) noexcept {
    const Rgb indirect = ComposeIndirect(t.ReceiverAlbedo, t.LocalJ);
    const double a = t.Accessibility;
    return {t.DirectDiffuse.R + t.Specular.R + t.Emission.R + a * t.AmbientDiffuse.R + indirect.R,
            t.DirectDiffuse.G + t.Specular.G + t.Emission.G + a * t.AmbientDiffuse.G + indirect.G,
            t.DirectDiffuse.B + t.Specular.B + t.Emission.B + a * t.AmbientDiffuse.B + indirect.B};
}

SectionFrame SectionFrame::FromView(const Vec3& view) noexcept {
    SectionFrame frame;
    frame.V = Normalize(view);
    const Vec3 helper = std::abs(frame.V.X) < 0.9 ? Vec3{1.0, 0.0, 0.0} : Vec3{0.0, 1.0, 0.0};
    frame.E1 = Normalize(Cross(helper, frame.V));
    frame.E2 = Cross(frame.V, frame.E1);
    return frame;
}

Vec3 SectionFrame::Tangent(double phi) const noexcept {
    return Add(Scale(E1, std::cos(phi)), Scale(E2, std::sin(phi)));
}

Vec3 SectionFrame::Direction(double phi, double theta) const noexcept {
    return Add(Scale(V, std::cos(theta)), Scale(Tangent(phi), std::sin(theta)));
}

double SectorWeightAnalytic(double a, double b, double lo, double hi) noexcept {
    return AnalyticWeight<double>(a, b, lo, hi);
}

float SectorWeightAnalyticF32(float a, float b, float lo, float hi) noexcept {
    return AnalyticWeight<float>(a, b, lo, hi);
}

double SectionMass(double a, double b) noexcept {
    const double r = std::hypot(a, b);
    if (!(r > 0.0)) return 0.0;
    const double gamma = std::abs(std::atan2(b, a));
    const double g = std::min(gamma, kPi - gamma);
    return r / kPi * (std::cos(g) + g * std::sin(g));
}

double IntervalWeight(double a, double b, double lo, double hi) noexcept {
    if (!(hi > lo)) return 0.0;
    if (hi - lo >= 2.0 * kPi) return SectionMass(a, b);
    const double turns = std::floor((lo + kPi) / (2.0 * kPi));
    lo -= 2.0 * kPi * turns;
    hi -= 2.0 * kPi * turns;
    if (hi <= kPi) return SectorWeightAnalytic(a, b, lo, hi);
    return SectorWeightAnalytic(a, b, lo, kPi) + SectorWeightAnalytic(a, b, -kPi, hi - 2.0 * kPi);
}

double Cumulative(double a, double b, double theta) noexcept {
    const double mass = SectionMass(a, b);
    if (!(mass > 0.0)) return 0.0;
    theta = std::clamp(theta, -kPi, kPi);
    return std::clamp(SectorWeightAnalytic(a, b, -kPi, theta) / mass, 0.0, 1.0);
}

const GaussLegendreRule& GaussLegendre(uint32_t order) {
    static std::mutex mutex;
    static std::map<uint32_t, GaussLegendreRule> rules;
    order = std::max(order, 1U);
    std::lock_guard lock(mutex);
    auto found = rules.find(order);
    if (found == rules.end()) found = rules.emplace(order, BuildGaussLegendre(order)).first;
    return found->second;
}

QuadratureResult SectorWeightQuadrature(double a, double b, double lo, double hi,
                                        const QuadratureOptions& options) {
    QuadratureResult result;
    lo = std::max(lo, -kPi);
    hi = std::min(hi, kPi);
    result.Converged = true;
    if (!(hi > lo)) return result;
    std::vector<double> cuts;
    if (options.SplitAtKinks) {
        cuts.push_back(0.0);
        AppendLobeEdges(a, b, cuts);
    }
    const auto points = Pieces(lo, hi, std::move(cuts));
    const auto integrand = [a, b](double theta) { return KinkIntegrand(a, b, theta); };
    for (size_t piece = 0; piece + 1 < points.size(); ++piece) {
        const double p = points[piece];
        const double q = points[piece + 1];
        double previous = GaussLegendreIntegral(GaussLegendre(1), p, q, integrand);
        double accepted = previous;
        bool converged = false;
        uint32_t order = 2;
        double correction = 0.0;
        for (; order <= options.MaxOrder; order *= 2) {
            const double current = GaussLegendreIntegral(GaussLegendre(order), p, q, integrand);
            correction = std::abs(current - previous);
            accepted = current;
            if (correction <= std::max(options.RelativeTolerance * std::abs(current), options.AbsoluteTolerance)) {
                converged = true;
                break;
            }
            previous = current;
        }
        result.Value += accepted;
        result.MaxOrder = std::max(result.MaxOrder, std::min(order, options.MaxOrder));
        result.LastCorrection = std::max(result.LastCorrection, correction);
        result.Converged = result.Converged && converged;
        ++result.Pieces;
    }
    return result;
}

void AngularField::SectionBreakpoints(const Vec3&, const Vec3&, std::vector<double>&) const {}

CapField::CapField(FieldSample background, std::vector<Cap> caps)
    : mBackground(background), mCaps(std::move(caps)) {
    for (auto& cap : mCaps) cap.Axis = Normalize(cap.Axis);
}

FieldSample CapField::Sample(const Vec3& omega) const {
    for (const auto& cap : mCaps)
        if (Dot(cap.Axis, omega) >= std::cos(cap.HalfAngle)) return cap.Inside;
    return mBackground;
}

void CapField::SectionBreakpoints(const Vec3& v, const Vec3& t, std::vector<double>& thetas) const {
    for (const auto& cap : mCaps) {
        // axis . omega(theta) = A cos(theta) + B sin(theta) = R cos(theta - delta).
        const double a = Dot(cap.Axis, v);
        const double b = Dot(cap.Axis, t);
        const double r = std::hypot(a, b);
        const double c = std::cos(cap.HalfAngle);
        if (!(r > 0.0) || std::abs(c) > r) continue;
        const double delta = std::atan2(b, a);
        const double spread = std::acos(std::clamp(c / r, -1.0, 1.0));
        thetas.push_back(WrapAngle(delta - spread));
        thetas.push_back(WrapAngle(delta + spread));
    }
}

CapField CapField::Scaled(double factor) const {
    const auto scale = [factor](FieldSample sample) {
        sample.Radiance = {sample.Radiance.R * factor, sample.Radiance.G * factor, sample.Radiance.B * factor};
        return sample;
    };
    std::vector<Cap> caps = mCaps;
    for (auto& cap : caps) cap.Inside = scale(cap.Inside);
    return CapField(scale(mBackground), std::move(caps));
}

void SectionAccumulator::Reset() noexcept { *this = SectionAccumulator{}; }

void SectionAccumulator::Add(const SectionContribution& c) noexcept {
    const double inverse = 1.0 / c.Pdf;
    mSum.R += c.F.R * inverse;
    mSum.G += c.F.G * inverse;
    mSum.B += c.F.B * inverse;
    mSumA += c.FA * inverse;
    ++mSections;
    mCandidates += c.Candidates;
    mValid += c.ValidCandidates;
}

Estimate SectionAccumulator::Result() const noexcept {
    Estimate estimate;
    estimate.Sections = mSections;
    estimate.Candidates = mCandidates;
    estimate.ValidCandidates = mValid;
    if (mSections == 0U) return estimate;
    const double inverse = 1.0 / static_cast<double>(mSections);
    estimate.J = {mSum.R * inverse, mSum.G * inverse, mSum.B * inverse};
    estimate.ARaw = mSumA * inverse;
    estimate.A = std::clamp(estimate.ARaw, 0.0, 1.0);
    estimate.Confidence = mCandidates == 0U ? 0.0 : static_cast<double>(mValid) / static_cast<double>(mCandidates);
    return estimate;
}

SectionContribution EvaluateSection(const AngularField& field, const SectionFrame& frame, const Vec3& normal,
                                    double phi, uint32_t sectors, const Rgb& fallback) {
    const auto c = EvaluateSectionT<double>(field, frame, normal, phi, sectors, fallback);
    SectionContribution out;
    out.F = {c.R, c.G, c.B};
    out.FA = c.A;
    out.Pdf = 1.0 / kPi;
    out.Candidates = c.Candidates;
    out.ValidCandidates = c.Valid;
    return out;
}

Estimate EstimateSectors(const AngularField& field, const SectionFrame& frame, const Vec3& normal,
                         const SectorEstimatorSettings& settings) {
    SectionAccumulator accumulator;
    for (uint32_t s = 0; s < settings.Sections; ++s) {
        const double phi = kPi * (static_cast<double>(s) + settings.Rotation) / settings.Sections;
        accumulator.Add(EvaluateSection(field, frame, normal, phi, settings.Sectors, settings.Fallback));
    }
    return accumulator.Result();
}

Estimate EstimateSectorsF32(const AngularField& field, const SectionFrame& frame, const Vec3& normal,
                            const SectorEstimatorSettings& settings) {
    float sumR = 0.0F, sumG = 0.0F, sumB = 0.0F, sumA = 0.0F;
    uint64_t candidates = 0, valid = 0;
    const float inversePdf = kPiT<float>;
    for (uint32_t s = 0; s < settings.Sections; ++s) {
        const float phi = kPiT<float> * (static_cast<float>(s) + static_cast<float>(settings.Rotation)) /
                          static_cast<float>(settings.Sections);
        const auto c = EvaluateSectionT<float>(field, frame, normal, phi, settings.Sectors, settings.Fallback);
        sumR += c.R * inversePdf;
        sumG += c.G * inversePdf;
        sumB += c.B * inversePdf;
        sumA += c.A * inversePdf;
        candidates += c.Candidates;
        valid += c.Valid;
    }
    Estimate estimate;
    estimate.Sections = settings.Sections;
    estimate.Candidates = candidates;
    estimate.ValidCandidates = valid;
    if (settings.Sections == 0U) return estimate;
    const float inverse = 1.0F / static_cast<float>(settings.Sections);
    estimate.J = {sumR * inverse, sumG * inverse, sumB * inverse};
    estimate.ARaw = sumA * inverse;
    estimate.A = std::clamp(estimate.ARaw, 0.0, 1.0);
    estimate.Confidence = candidates == 0U ? 0.0 : static_cast<double>(valid) / static_cast<double>(candidates);
    return estimate;
}

ContinuousResult IntegrateContinuous(const AngularField& field, const SectionFrame& frame, const Vec3& normal,
                                     const Rgb& fallback, const ContinuousOptions& options) {
    ContinuousResult result;
    uint32_t maxInnerOrder = 0;
    bool innerConverged = true;
    // Exact section integral of [L_R, L_G, L_B, V] * f_phi(theta).
    const auto section = [&](double phi) {
        SectionSums sums;
        const Vec3 t = frame.Tangent(phi);
        const double a = Dot(normal, frame.V);
        const double b = Dot(normal, t);
        if (!(a * a + b * b > 0.0)) return sums;
        std::vector<double> cuts{0.0};
        AppendLobeEdges(a, b, cuts);
        field.SectionBreakpoints(frame.V, t, cuts);
        const auto points = Pieces(-kPi, kPi, std::move(cuts));
        for (size_t piece = 0; piece + 1 < points.size(); ++piece) {
            const double p = points[piece];
            const double q = points[piece + 1];
            const double middle = 0.5 * (p + q);
            if (!(a * std::cos(middle) + b * std::sin(middle) > 0.0)) continue; // outside the lobe
            const auto integrate = [&](uint32_t order) {
                const auto& rule = GaussLegendre(order);
                const double half = 0.5 * (q - p);
                SectionSums piecewise;
                for (size_t i = 0; i < rule.Nodes.size(); ++i) {
                    const double theta = middle + half * rule.Nodes[i];
                    const double weight = rule.Weights[i] * half * KinkIntegrand(a, b, theta);
                    const FieldSample sample = field.Sample(Add(Scale(frame.V, std::cos(theta)),
                                                                Scale(t, std::sin(theta))));
                    const bool hit = sample.State == DirectionState::Hit;
                    const Rgb& radiance = hit ? sample.Radiance : fallback;
                    piecewise.R += weight * radiance.R;
                    piecewise.G += weight * radiance.G;
                    piecewise.B += weight * radiance.B;
                    piecewise.A += hit ? 0.0 : weight;
                }
                return piecewise;
            };
            SectionSums previous = integrate(2);
            SectionSums current = previous;
            bool converged = false;
            uint32_t order = 4;
            for (; order <= options.Inner.MaxOrder; order *= 2) {
                current = integrate(order);
                const double change = std::max({std::abs(current.R - previous.R), std::abs(current.G - previous.G),
                                                std::abs(current.B - previous.B), std::abs(current.A - previous.A)});
                const double scale =
                    std::max({std::abs(current.R), std::abs(current.G), std::abs(current.B), std::abs(current.A)});
                if (change <= std::max(options.Inner.RelativeTolerance * scale, options.Inner.AbsoluteTolerance)) {
                    converged = true;
                    break;
                }
                previous = current;
            }
            maxInnerOrder = std::max(maxInnerOrder, std::min(order, options.Inner.MaxOrder));
            innerConverged = innerConverged && converged;
            sums.R += current.R;
            sums.G += current.G;
            sums.B += current.B;
            sums.A += current.A;
        }
        return sums;
    };
    const auto outer = [&](uint32_t panels) {
        const auto& rule = GaussLegendre(options.PanelOrder);
        SectionSums total;
        const double width = kPi / panels;
        for (uint32_t panel = 0; panel < panels; ++panel) {
            const double middle = (panel + 0.5) * width;
            for (size_t i = 0; i < rule.Nodes.size(); ++i) {
                const SectionSums s = section(middle + 0.5 * width * rule.Nodes[i]);
                const double w = rule.Weights[i] * 0.5 * width;
                total.R += w * s.R;
                total.G += w * s.G;
                total.B += w * s.B;
                total.A += w * s.A;
            }
        }
        return total;
    };
    uint32_t panels = 8;
    SectionSums previous = outer(panels);
    SectionSums current = previous;
    double change = 0.0;
    while (panels < options.MaxPanels) {
        panels *= 2;
        current = outer(panels);
        change = 0.0;
        bool converged = true;
        for (const auto [now, before] : {std::pair{current.R, previous.R}, std::pair{current.G, previous.G},
                                         std::pair{current.B, previous.B}, std::pair{current.A, previous.A}}) {
            const double difference = std::abs(now - before);
            change = std::max(change, difference / std::max(std::abs(now), 1.0e-300));
            converged = converged && difference <= options.Tolerance * std::abs(now);
        }
        if (converged) {
            result.Converged = innerConverged;
            break;
        }
        previous = current;
    }
    result.J = {current.R, current.G, current.B};
    result.A = current.A;
    result.Panels = panels;
    result.MaxInnerOrder = maxInnerOrder;
    result.LastCorrection = change;
    return result;
}

} // namespace Fast::Renderer::DiffuseGi
