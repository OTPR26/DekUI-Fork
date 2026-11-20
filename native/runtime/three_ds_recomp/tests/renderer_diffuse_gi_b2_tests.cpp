// [B2] reintegration tests: Jimenez, Wu, Pesce, Jarabo, "Practical Realtime
// Strategies for Accurate Indirect Occlusion", Activision Technical Memo
// ATVI-TR-16-01 (file B2-PracticalRealtimeStrategiesTRfinal.pdf, SHA-256
// 33e48153c21e411a3c52081a917be61fe707fa531719999566ec94fde8ab9fb1).
//
// Every [B2] quantity in this file is implemented here from the paper's
// equations (eqs. 5, 6, 7, 8, 10, 17, 23), independently of the M1/M2 code
// paths, and then compared with M1's integrator and M2's gather. The horizon
// estimator below has its own depth decoding, section frame, screen direction
// and texel enumeration; it shares with M2 only the declared inputs and the
// specification's rules (q from the continuous line UV and the texel depth,
// self-intersection tolerances). Lines starting with DGI_REPORT are quoted in
// docs/TRIAEVUM_SCREEN_SPACE_GI.md, section "[B2] reintegration".
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_reference.h"
#include "fast/renderer/diffuse_gi_scene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

namespace {

constexpr double kHalfPi = 0.5 * kPi;
constexpr double kDegree = kPi / 180.0;

// ---------------------------------------------------------------------------
// [B2] closed forms, from the paper.

// Eq. 7 (p. 3), one horizon term: 1/4 (-cos(2h - gamma) + cos(gamma) + 2 h sin(gamma)).
double Eq7Term(double h, double gamma) {
    return 0.25 * (-std::cos(2.0 * h - gamma) + std::cos(gamma) + 2.0 * h * std::sin(gamma));
}

// Eq. 7: inner integral a-hat of eq. 5 between signed horizons h1 <= 0 <= h2.
double Eq7(double h1, double h2, double gamma) { return Eq7Term(h1, gamma) + Eq7Term(h2, gamma); }

// Own derivation: Eq7Term(h) is the antiderivative integral_0^h cos(t - gamma)
// sin(t) dt, so an interval inside one half circle is a difference of terms
// (|sin t| = -sin t for t < 0); an interval through 0 is split there. Valid
// where cos(t - gamma) >= 0 (inside the lobe).
double Eq7Interval(double p, double q, double gamma) {
    if (!(q > p)) return 0.0;
    if (p >= 0.0) return Eq7Term(q, gamma) - Eq7Term(p, gamma);
    if (q <= 0.0) return Eq7Term(p, gamma) - Eq7Term(q, gamma);
    return Eq7Term(p, gamma) + Eq7Term(q, gamma);
}

// Eq. 5 has cos(theta - gamma)^+; eq. 7 integrates cos without the positive
// part, so horizons beyond the lobe [gamma - pi/2, gamma + pi/2] are clamped
// to its edges first (own adaptation, required by eq. 5).
double Eq7LobeClamped(double h1, double h2, double gamma) {
    return Eq7(std::max(h1, gamma - kHalfPi), std::min(h2, gamma + kHalfPi), gamma);
}

// Eq. 23 (Appendix A, uniformly weighted variant): 2 - cos(h1) - cos(h2).
double Eq23(double h1, double h2) { return 2.0 - std::cos(h1) - std::cos(h2); }

// Eq. 10 (Section 5): G(A, rho) = a(rho) A^3 - b(rho) A^2 + c(rho) A.
double Eq10(double a, double rho) {
    const double ca = 2.0404 * rho - 0.3324, cb = 4.7951 * rho - 0.6417, cc = 2.7552 * rho + 0.6903;
    return ca * a * a * a - cb * a * a + cc * a;
}

// Composite Simpson rule on pieces split at the given kinks (independent of
// M1's Gauss-Legendre quadrature).
template <typename Function>
double Simpson(const Function& f, double lo, double hi, std::vector<double> cuts, int panels) {
    cuts.push_back(lo);
    cuts.push_back(hi);
    std::sort(cuts.begin(), cuts.end());
    double total = 0.0;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const double a = std::max(cuts[i], lo), b = std::min(cuts[i + 1], hi);
        if (!(b > a)) continue;
        const double h = (b - a) / panels;
        double sum = f(a) + f(b);
        for (int k = 1; k < panels; ++k) sum += (k % 2 != 0 ? 4.0 : 2.0) * f(a + k * h);
        total += sum * h / 3.0;
    }
    return total;
}

// Eq. 5 inner integral by quadrature: integral of cos(theta - gamma)^+ |sin theta|.
double Eq5Simpson(double lo, double hi, double gamma) {
    const auto f = [gamma](double t) { return std::max(0.0, std::cos(t - gamma)) * std::abs(std::sin(t)); };
    return Simpson(f, lo, hi, {0.0, gamma - kHalfPi, gamma + kHalfPi}, 2048);
}

uint32_t Lcg(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}
double Uniform(uint32_t& state, double lo, double hi) {
    return lo + (hi - lo) * static_cast<double>(Lcg(state) >> 8) / 16777216.0;
}

// ---------------------------------------------------------------------------
// [B2] horizon-based AO on a depth buffer (eqs. 5, 6, 7, 8), own
// implementation. For each section phi it searches both screen directions
// +-t_hat(phi) for the sample s maximizing <omega_s, omega_o> (eq. 6), turns
// the two horizons into a-hat with eq. 7 and weights it by the norm of the
// normal projected on the section plane (eq. 8). Sections phi_s = pi (s +
// rotation) / S with p = 1/pi, as M1/M2, so A = (1/S) sum_s ||n_p|| a-hat.

struct HorizonOptions {
    double Radius = 1.0;      // metric search radius: samples with |s - x| > R are ignored
    bool PositivePart = false; // eq. 6 as written: max <omega_s, omega_o>^+ (horizon at most pi/2 from v)
    bool RejectSelf = true;    // adaptation: the receiver's own surface is no occluder (spec section 4)
    bool PixelSteps = false;   // eq. 6 sampling s_hat = x_hat + t_hat s, s = 1, 2, ... (else every crossed texel)
    double SelfSine = 2.0e-3, SelfDepth = 1.0e-5; // the specification adapter's declared tolerances
};

struct HorizonResult {
    bool Valid = false;
    double A = 1.0;   // [B2] accessibility
    double Gap = 0.0; // own derivation: below a horizon but outside every infinite-thickness segment
    uint32_t ClampedSides = 0; // sides whose eq. 6 positive part moved the horizon (as written)
    uint32_t Sides = 0;
};

class HorizonAo {
  public:
    explicit HorizonAo(const GatherInput& in) : mIn(in), mW(in.Viewport.Width), mH(in.Viewport.Height) {
        const DepthCamera& c = in.Camera;
        const double n = c.Near, f = c.Far;
        mZ.assign(static_cast<size_t>(mW) * mH, 0.0);
        mGeometry.assign(mZ.size(), 0U);
        for (uint32_t y = 0; y < mH; ++y)
            for (uint32_t x = 0; x < mW; ++x) {
                const size_t b = Buffer(x, y);
                const auto cls = static_cast<TexelClass>(in.Class[b]);
                if (cls != TexelClass::OneSided && cls != TexelClass::TwoSided) continue;
                // Perspective window depth 0 at near, 1 at far (declared).
                const double z = n * f / (f - static_cast<double>(in.Depth[b]) * (f - n));
                if (!std::isfinite(z) || z < n * (1.0 - 1.0e-5) || z > f * (1.0 + 1.0e-5)) continue;
                mZ[Index(x, y)] = z;
                mGeometry[Index(x, y)] = 1U;
            }
    }

    [[nodiscard]] HorizonResult Pixel(uint32_t x, uint32_t y, uint32_t sections, double rotation,
                                      const HorizonOptions& o) const {
        HorizonResult out;
        if (mGeometry[Index(x, y)] == 0U) return out;
        const double zp = mZ[Index(x, y)];
        const Vec3 p = Scale(Ray(x + 0.5, y + 0.5), zp);
        const Vec3 v = Normalize(Scale(p, -1.0));
        const size_t b = Buffer(x, y);
        Vec3 n = Normalize({mIn.FaceNormals[3 * b], mIn.FaceNormals[3 * b + 1], mIn.FaceNormals[3 * b + 2]});
        if (!(Dot(n, n) > 0.0)) return out;
        if (Dot(n, p) > 0.0) n = Scale(n, -1.0); // the visible side faces the camera
        // Section frame: the convention declared by M1 (e1 = normalize(h x v), h = X unless |v.x| >= 0.9).
        const Vec3 helper = std::abs(v.X) < 0.9 ? Vec3{1.0, 0.0, 0.0} : Vec3{0.0, 1.0, 0.0};
        const Vec3 e1 = Normalize(Cross(helper, v));
        const Vec3 e2 = Cross(v, e1);
        const auto& c = mIn.Camera;
        const double det = c.ViewX[0] * c.ViewY[1] - c.ViewX[1] * c.ViewY[0];
        double sumA = 0.0, sumGap = 0.0;
        for (uint32_t s = 0; s < sections; ++s) {
            const double phi = kPi * (s + rotation) / sections;
            const Vec3 t = Add(Scale(e1, std::cos(phi)), Scale(e2, std::sin(phi)));
            const double a = Dot(n, v), bt = Dot(n, t);
            const double r = std::hypot(a, bt); // ||n_p||, eq. 8
            const double gamma = std::atan2(bt, a); // signed toward +t
            // Screen image of +t: derivative of (X/z, Y/z) along p + mu t, through
            // the inverse of the declared UV-to-ray map, in pixels.
            const double gx = t.X * zp + p.X * t.Z, gy = t.Y * zp + p.Y * t.Z;
            const double du = (c.ViewY[1] * gx - c.ViewX[1] * gy) / det * mW;
            const double dv = (-c.ViewY[0] * gx + c.ViewX[0] * gy) / det * mH;
            const double length = std::hypot(du, dv);
            if (!(length > 0.0) || !std::isfinite(length)) return HorizonResult{};
            double h[2] = {gamma + kHalfPi, gamma - kHalfPi}; // [+ side, - side]: lobe edges when open
            for (int side = 0; side < 2; ++side) {
                const double sign = side == 0 ? 1.0 : -1.0;
                const double edge = side == 0 ? gamma + kHalfPi : gamma - kHalfPi;
                double maxCos = -std::numeric_limits<double>::infinity(), horizon = 0.0;
                std::vector<std::pair<double, double>> segments; // infinite-thickness segments, theta
                const auto visit = [&](int32_t cx, int32_t cy, double lambda) {
                    const size_t i = Index(cx, cy);
                    if (mGeometry[i] == 0U) return; // sky or unavailable: open
                    const double zq = mZ[i];
                    if (o.RejectSelf) {
                        const Vec3 d = Add(Scale(Ray(cx + 0.5, cy + 0.5), zq), Scale(p, -1.0));
                        const double length3 = std::sqrt(Dot(d, d));
                        if (Dot(d, n) <= o.SelfSine * length3 + o.SelfDepth * zp) return;
                    }
                    const Vec3 q = Scale(Ray(x + 0.5 + lambda * sign * du / length, y + 0.5 + lambda * sign * dv / length), zq);
                    const Vec3 d = Add(q, Scale(p, -1.0));
                    const double distance = std::sqrt(Dot(d, d));
                    if (distance > o.Radius) return;
                    const double cosine = Dot(d, v) / distance; // <omega_s, omega_o>
                    const double theta = std::atan2(Dot(d, t), Dot(d, v));
                    if (cosine > maxCos) {
                        maxCos = cosine;
                        horizon = theta;
                    }
                    // Infinite thickness: the camera ray behind q, up to the R sphere.
                    const Vec3 ray = Normalize(q);
                    const double along = Dot(d, ray);
                    const double clip =
                        -along + std::sqrt(std::max(along * along - (distance * distance - o.Radius * o.Radius), 0.0));
                    const Vec3 back = Add(Add(q, Scale(ray, clip)), Scale(p, -1.0));
                    const double thetaBack = std::atan2(Dot(back, t), Dot(back, v));
                    segments.emplace_back(std::min(theta, thetaBack), std::max(theta, thetaBack));
                };
                const double sx = sign * du / length, sy = sign * dv / length;
                if (o.PixelSteps) PixelStepSamples(x, y, sx, sy, visit);
                else EveryTexelSamples(x, y, sx, sy, visit);
                ++out.Sides;
                if (!std::isfinite(maxCos)) continue; // no occluder: horizon at the lobe edge
                double horizonAngle = horizon;
                if (o.PositivePart && maxCos <= 0.0) {
                    horizonAngle = sign * kHalfPi; // arccos(max(...)^+) = pi/2
                    ++out.ClampedSides;
                }
                h[side] = horizonAngle;
                // Gap: occluded by the horizon, i.e. [horizon, edge] on this side,
                // minus the union of the segments.
                const double lo = side == 0 ? horizon : edge, hi = side == 0 ? edge : horizon;
                if (!(hi > lo)) continue;
                std::sort(segments.begin(), segments.end());
                double cursor = lo;
                for (const auto& [s0, s1] : segments) {
                    if (s1 <= cursor) continue;
                    if (s0 >= hi) break;
                    if (s0 > cursor) sumGap += r * Eq7Interval(cursor, s0, gamma);
                    cursor = std::max(cursor, s1);
                    if (cursor >= hi) break;
                }
                if (cursor < hi) sumGap += r * Eq7Interval(cursor, hi, gamma);
            }
            if (o.PositivePart) {
                // Without samples the as-written horizon is also pi/2 (empty max: 0^+).
                h[0] = std::min(h[0], kHalfPi);
                h[1] = std::max(h[1], -kHalfPi);
            }
            sumA += r * Eq7LobeClamped(h[1], h[0], gamma);
        }
        out.Valid = true;
        out.A = sumA / sections;
        out.Gap = sumGap / sections;
        return out;
    }

    [[nodiscard]] bool Geometry(uint32_t x, uint32_t y) const { return mGeometry[Index(x, y)] != 0U; }

  private:
    [[nodiscard]] size_t Index(int64_t x, int64_t y) const { return static_cast<size_t>(y) * mW + static_cast<size_t>(x); }
    [[nodiscard]] size_t Buffer(int64_t x, int64_t y) const {
        return (static_cast<size_t>(mIn.Viewport.Y) + static_cast<size_t>(y)) * mIn.BufferWidth + mIn.Viewport.X +
               static_cast<size_t>(x);
    }
    [[nodiscard]] Vec3 Ray(double px, double py) const {
        const auto& c = mIn.Camera;
        const double u = px / mW, v = py / mH;
        return {c.ViewX[0] * u + c.ViewX[1] * v + c.ViewX[2], c.ViewY[0] * u + c.ViewY[1] * v + c.ViewY[2], -1.0};
    }

    // Every texel crossed by the line from the receiver centre to the viewport
    // edge, by sorting all grid-line crossings (not a stepping traversal). A
    // texel is represented by the line point nearest to its centre, clamped
    // to its chord; chords of at most 1e-3 px (corner touches) are skipped.
    template <typename Visit>
    void EveryTexelSamples(uint32_t x, uint32_t y, double dx, double dy, const Visit& visit) const {
        const double inf = std::numeric_limits<double>::infinity();
        const double adx = std::abs(dx), ady = std::abs(dy);
        const double exitX = dx > 0.0 ? (mW - x - 0.5) / adx : (dx < 0.0 ? (x + 0.5) / adx : inf);
        const double exitY = dy > 0.0 ? (mH - y - 0.5) / ady : (dy < 0.0 ? (y + 0.5) / ady : inf);
        const double exit = std::min(exitX, exitY);
        std::vector<double> cuts;
        if (adx > 0.0)
            for (uint32_t k = 0;; ++k) {
                const double l = (k + 0.5) / adx;
                if (l > exit) break;
                cuts.push_back(l);
            }
        if (ady > 0.0)
            for (uint32_t k = 0;; ++k) {
                const double l = (k + 0.5) / ady;
                if (l > exit) break;
                cuts.push_back(l);
            }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        for (size_t i = 1; i < cuts.size(); ++i) {
            const double enter = cuts[i - 1], leave = cuts[i];
            if (!(leave - enter > 1.0e-3)) continue;
            const double middle = 0.5 * (enter + leave);
            const auto cx = static_cast<int32_t>(std::floor(x + 0.5 + middle * dx));
            const auto cy = static_cast<int32_t>(std::floor(y + 0.5 + middle * dy));
            if (cx < 0 || cy < 0 || cx >= static_cast<int32_t>(mW) || cy >= static_cast<int32_t>(mH)) continue;
            const double ox = cx - static_cast<double>(x), oy = cy - static_cast<double>(y);
            visit(cx, cy, std::clamp(ox * dx + oy * dy, enter, leave));
        }
    }

    // [B2] eq. 6 sampling: s_hat = x_hat + t_hat s for s = 1, 2, ... pixels,
    // with the depth of the texel containing s_hat.
    template <typename Visit>
    void PixelStepSamples(uint32_t x, uint32_t y, double dx, double dy, const Visit& visit) const {
        for (uint32_t k = 1;; ++k) {
            const double px = x + 0.5 + k * dx, py = y + 0.5 + k * dy;
            if (px < 0.0 || py < 0.0 || px >= mW || py >= mH) break;
            const auto cx = static_cast<int32_t>(std::floor(px)), cy = static_cast<int32_t>(std::floor(py));
            if (cx == static_cast<int32_t>(x) && cy == static_cast<int32_t>(y)) continue;
            visit(cx, cy, static_cast<double>(k));
        }
    }

    const GatherInput& mIn;
    uint32_t mW = 0, mH = 0;
    std::vector<double> mZ;
    std::vector<uint8_t> mGeometry;
};

GatherInput FaceNormalInput(const Scene& scene) {
    GatherInput input = Rasterize(scene);
    input.NormalSource = GeometricNormalSource::RendererFaceNormal; // the same exact normal on both sides
    return input;
}

struct Summary {
    double Max = 0.0, Sum = 0.0, Min = std::numeric_limits<double>::infinity();
    size_t Count = 0, Over = 0;
    void Add(double value, double threshold) {
        Max = std::max(Max, value);
        Min = std::min(Min, value);
        Sum += value;
        ++Count;
        if (value > threshold) ++Over;
    }
    [[nodiscard]] double Mean() const { return Count == 0U ? 0.0 : Sum / static_cast<double>(Count); }
};

} // namespace

TEST(DiffuseGiB2, SliceIntegralEq7AgainstM1AndQuadrature) {
    // (1) The paper's domain: h1 <= 0 <= h2, |h| <= pi/2 (eq. 6), inside the
    // lobe. [B2] per-section weight r a-hat / pi against M1's IntervalWeight
    // and against an independent Simpson quadrature of eq. 5.
    double inDomainM1 = 0.0, inDomainQuadrature = 0.0;
    int inDomainCases = 0;
    for (const double r : {1.0, 0.6, 0.15})
        for (double gammaDeg = -85.0; gammaDeg <= 85.0; gammaDeg += 5.0) {
            const double gamma = gammaDeg * kDegree, a = r * std::cos(gamma), b = r * std::sin(gamma);
            for (const double f1 : {0.0, 0.25, 0.5, 0.75, 1.0})
                for (const double f2 : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                    const double h1 = f1 * std::max(-kHalfPi, gamma - kHalfPi);
                    const double h2 = f2 * std::min(kHalfPi, gamma + kHalfPi);
                    const double b2 = r * Eq7(h1, h2, gamma) / kPi;
                    inDomainM1 = std::max(inDomainM1, std::abs(b2 - IntervalWeight(a, b, h1, h2)));
                    inDomainQuadrature = std::max(inDomainQuadrature, std::abs(b2 - r * Eq5Simpson(h1, h2, gamma) / kPi));
                    ++inDomainCases;
                }
        }
    // (2) Horizons inside eq. 6's range but beyond the lobe edge (possible
    // whenever gamma != 0, e.g. h1 = -pi/2 with gamma > 0): eq. 7 applied
    // as is integrates the negative half of cos(theta - gamma) and departs from
    // eq. 5; clamping the horizons to the lobe first restores it.
    double rawDeviation = 0.0, clampedM1 = 0.0, clampedQuadrature = 0.0;
    int beyondCases = 0;
    for (double gammaDeg = -85.0; gammaDeg <= 85.0; gammaDeg += 5.0) {
        if (gammaDeg == 0.0) continue;
        const double gamma = gammaDeg * kDegree, a = std::cos(gamma), b = std::sin(gamma);
        for (const double f : {0.0, 0.5, 1.0})
            for (const double g : {0.25, 0.75, 1.0}) {
                // The horizon on the side away from the normal lies beyond its lobe edge.
                double h1 = 0.0, h2 = 0.0;
                if (gamma > 0.0) {
                    h1 = -kHalfPi + f * (gamma - kHalfPi + kHalfPi) * 0.999; // in [-pi/2, gamma - pi/2)
                    h2 = g * kHalfPi;
                } else {
                    h1 = -g * kHalfPi;
                    h2 = kHalfPi - f * (kHalfPi - (gamma + kHalfPi)) * 0.999; // in (gamma + pi/2, pi/2]
                }
                const double eq5 = Eq5Simpson(h1, h2, gamma);
                rawDeviation = std::max(rawDeviation, std::abs(Eq7(h1, h2, gamma) - eq5));
                const double clamped = Eq7LobeClamped(h1, h2, gamma);
                clampedQuadrature = std::max(clampedQuadrature, std::abs(clamped - eq5));
                clampedM1 = std::max(clampedM1, std::abs(clamped / kPi - IntervalWeight(a, b, h1, h2)));
                ++beyondCases;
            }
    }
    // (3) Outside eq. 6's range but inside the lobe: |h| up to pi/2 + |gamma|
    // (eq. 7 stays valid there; as written, eq. 6 never produces such a horizon).
    double extendedM1 = 0.0;
    int extendedCases = 0;
    for (double gammaDeg = -85.0; gammaDeg <= 85.0; gammaDeg += 5.0) {
        const double gamma = gammaDeg * kDegree, a = std::cos(gamma), b = std::sin(gamma);
        for (const double f : {0.0, 0.3, 0.7, 1.0}) {
            const double h1 = gamma - kHalfPi + f * std::max(0.0, -gamma);       // reaches below -pi/2 when gamma < 0
            const double h2 = gamma + kHalfPi - f * std::max(0.0, gamma);        // reaches above pi/2 when gamma > 0
            extendedM1 = std::max(extendedM1, std::abs(Eq7(h1, h2, gamma) / kPi - IntervalWeight(a, b, h1, h2)));
            ++extendedCases;
        }
    }
    // (4) One-sided intervals (sector pieces, both ends on one side of v):
    // eq. 7's terms as an antiderivative, against M1's sector weight and
    // cumulative C_phi.
    uint32_t state = 2016U;
    double pieceM1 = 0.0, pieceCumulative = 0.0;
    int pieceCases = 0;
    for (int k = 0; k < 4000; ++k) {
        const double gamma = Uniform(state, -0.49 * kPi, 0.49 * kPi), r = Uniform(state, 0.05, 1.0);
        const double a = r * std::cos(gamma), b = r * std::sin(gamma);
        const bool plus = (k % 2) == 0;
        const double lo = plus ? 0.0 : gamma - kHalfPi, hi = plus ? gamma + kHalfPi : 0.0;
        double p = Uniform(state, lo, hi), q = Uniform(state, lo, hi);
        if (p > q) std::swap(p, q);
        const double b2 = r * Eq7Interval(p, q, gamma) / kPi;
        pieceM1 = std::max(pieceM1, std::abs(b2 - SectorWeightAnalytic(a, b, p, q)));
        pieceCumulative = std::max(pieceCumulative,
                                   std::abs(b2 - SectionMass(a, b) * (Cumulative(a, b, q) - Cumulative(a, b, p))));
        ++pieceCases;
    }
    // (5) Section mass: horizons at the lobe edges.
    double massM1 = 0.0;
    for (double gammaDeg = -89.0; gammaDeg <= 89.0; gammaDeg += 1.0) {
        const double gamma = gammaDeg * kDegree;
        massM1 = std::max(massM1, std::abs(Eq7(gamma - kHalfPi, gamma + kHalfPi, gamma) / kPi -
                                           SectionMass(std::cos(gamma), std::sin(gamma))));
    }
    std::printf("DGI_REPORT B2_eq7 paper_domain cases=%d vs_M1_max_abs=%.2e vs_simpson_eq5_max_abs=%.2e | "
                "beyond_lobe cases=%d raw_eq7_vs_eq5_max_abs=%.3f lobe_clamped_vs_eq5=%.2e lobe_clamped_vs_M1=%.2e | "
                "beyond_pi/2_inside_lobe cases=%d vs_M1=%.2e | one_sided_pieces cases=%d vs_M1=%.2e vs_cumulative=%.2e | "
                "mass_vs_M1=%.2e\n",
                inDomainCases, inDomainM1, inDomainQuadrature, beyondCases, rawDeviation, clampedQuadrature, clampedM1,
                extendedCases, extendedM1, pieceCases, pieceM1, pieceCumulative, massM1);
    EXPECT_LT(inDomainM1, 1.0e-15);
    EXPECT_LT(inDomainQuadrature, 1.0e-12);
    EXPECT_GT(rawDeviation, 1.0e-2); // eq. 7 is not eq. 5 outside the lobe
    EXPECT_LT(clampedQuadrature, 1.0e-12);
    EXPECT_LT(clampedM1, 1.0e-15);
    EXPECT_LT(extendedM1, 1.0e-15);
    EXPECT_LT(pieceM1, 1.0e-15);
    EXPECT_LT(pieceCumulative, 1.0e-14);
    EXPECT_LT(massM1, 1.0e-15);

    // (6) Eq. 8 over phi for an unoccluded receiver whose normal is tilted by
    // alpha from v (a = cos alpha, b = sin alpha cos phi). With horizons at
    // the lobe edges (M1's full-circle measure) A = 1 for every tilt. As
    // written, eq. 6's positive part caps both horizons at pi/2 from v, and A
    // becomes (1 + cos alpha) / 2 (own closed form: the lost lobe part beyond
    // pi/2 is half of the r gamma sin gamma term of M_phi, whose phi integral
    // is 1 - cos alpha).
    std::printf("DGI_REPORT B2_eq8_open_receiver");
    for (const double alphaDeg : {0.0, 30.0, 60.0, 85.0, 89.0}) {
        const double alpha = alphaDeg * kDegree;
        const auto open = [alpha](bool positivePart) {
            return [alpha, positivePart](double phi) {
                const double a = std::cos(alpha), b = std::sin(alpha) * std::cos(phi);
                const double r = std::hypot(a, b), gamma = std::atan2(b, a);
                double h1 = gamma - kHalfPi, h2 = gamma + kHalfPi;
                if (positivePart) {
                    h1 = std::max(h1, -kHalfPi);
                    h2 = std::min(h2, kHalfPi);
                }
                return r * Eq7(h1, h2, gamma) / kPi;
            };
        };
        // gamma turns by ~pi over ~2 cos(alpha) of phi near phi = pi/2: fine panels.
        const double adapted = Simpson(open(false), 0.0, kPi, {kHalfPi}, 16384);
        const double asWritten = Simpson(open(true), 0.0, kPi, {kHalfPi}, 16384);
        std::printf(" alpha=%.0f: lobe_edges=%.12f as_written=%.6f (1+cos)/2=%.6f", alphaDeg, adapted, asWritten,
                    0.5 * (1.0 + std::cos(alpha)));
        EXPECT_NEAR(adapted, 1.0, 1.0e-10);
        EXPECT_NEAR(asWritten, 0.5 * (1.0 + std::cos(alpha)), 1.0e-10);
    }
    std::printf("\n");

    // (7) Eq. 23 (Appendix A): uniformly weighted inner integral, normalized by
    // 1/(2 pi); rejected by the specification (cosine-weighted contract).
    double eq23Error = 0.0;
    for (const double h1 : {-kHalfPi, -1.0, -0.3, 0.0})
        for (const double h2 : {0.0, 0.2, 0.9, kHalfPi})
            eq23Error = std::max(eq23Error, std::abs(Eq23(h1, h2) - Simpson([](double t) { return std::abs(std::sin(t)); },
                                                                              h1, h2, {0.0}, 2048)));
    const double eq23Open = Eq23(-kHalfPi, kHalfPi) * kPi / (2.0 * kPi);
    std::printf("DGI_REPORT B2_eq23 uniform_inner_vs_simpson_max_abs=%.2e open_A=%.15f\n", eq23Error, eq23Open);
    EXPECT_LT(eq23Error, 1.0e-12);
    EXPECT_NEAR(eq23Open, 1.0, 1.0e-15);
}

TEST(DiffuseGiB2, ConeVisibilityEq17WithM1Integrator) {
    // Eq. 17: visibility homogeneous about the normal (both horizons alpha from
    // it) gives A = 1 - cos^2(alpha). Field: everything occluded except the
    // cone of half angle alpha about n; M1's continuous integrator with the
    // view vector away from n (so the cone is not centred on the section axis).
    const SectionFrame frame = SectionFrame::FromView(Normalize({0.2, 0.3, 0.93}));
    const FieldSample hit{DirectionState::Hit, {1.0, 1.0, 1.0}, {}};
    double worst = 0.0, worstCorrection = 0.0;
    uint32_t maxPanels = 0;
    std::printf("DGI_REPORT B2_eq17_cone");
    for (const double tiltDeg : {0.0, 40.0, 70.0})
        for (const double alphaDeg : {20.0, 50.0, 80.0}) {
            const Vec3 n = Normalize(Add(Scale(frame.V, std::cos(tiltDeg * kDegree)),
                                         Scale(frame.Tangent(0.8), std::sin(tiltDeg * kDegree))));
            const CapField field(hit, {{n, alphaDeg * kDegree, FieldSample{}}});
            const auto result = IntegrateContinuous(field, frame, n, {});
            const double expected = 1.0 - std::pow(std::cos(alphaDeg * kDegree), 2.0);
            worst = std::max(worst, std::abs(result.A - expected));
            worstCorrection = std::max(worstCorrection, result.LastCorrection);
            maxPanels = std::max(maxPanels, result.Panels);
            // Eq. 18 inverts eq. 17: cos(alpha) = sqrt(1 - A).
            EXPECT_NEAR(std::sqrt(1.0 - result.A), std::cos(alphaDeg * kDegree), 1.0e-6);
            std::printf(" tilt=%.0f,alpha=%.0f:A=%.9f", tiltDeg, alphaDeg, result.A);
        }
    std::printf(" max_abs_vs_eq17=%.2e max_last_correction=%.1e max_panels=%u\n", worst, worstCorrection, maxPanels);
    // M1's continuous reference reaches about 2e-7 on caps crossing the
    // sections (T02); the outer phi integrand has square-root kinks there.
    EXPECT_LT(worst, 1.0e-6);
}

TEST(DiffuseGiB2, MultiBounceFitEq10ScopeRecord) {
    // Section 5 is outside the current scope (specification section 13: an
    // occlusion/albedo correction, not colour transfer). Recorded properties
    // for the later multi-bounce milestone: G(1, rho), G(A, 0) against A
    // (no interreflection at zero albedo), monotonicity in A.
    double worstUnit = 0.0, worstZeroAlbedo = 0.0, worstAt = 0.0;
    bool monotone = true;
    for (int i = 0; i <= 100; ++i) {
        const double rho = i / 100.0;
        worstUnit = std::max(worstUnit, std::abs(Eq10(1.0, rho) - 1.0));
        for (int k = 0; k < 1000; ++k) monotone = monotone && Eq10((k + 1) / 1000.0, rho) >= Eq10(k / 1000.0, rho);
    }
    for (int k = 0; k <= 1000; ++k) {
        const double a = k / 1000.0, d = Eq10(a, 0.0) - a;
        if (std::abs(d) > worstZeroAlbedo) {
            worstZeroAlbedo = std::abs(d);
            worstAt = a;
        }
    }
    std::printf("DGI_REPORT B2_eq10 max_abs_G(1,rho)-1=%.2e max_abs_G(A,0)-A=%.4f at_A=%.3f monotone_in_A=%d\n",
                worstUnit, worstZeroAlbedo, worstAt, monotone ? 1 : 0);
    EXPECT_LT(worstUnit, 1.0e-3);
    EXPECT_GT(worstZeroAlbedo, 0.04); // the fit does not reduce to A at zero albedo
    EXPECT_TRUE(monotone);
}

TEST(DiffuseGiB2, HeightFieldHorizonAoEqualsInfiniteThicknessGather) {
    // M2's gather with unbounded thickness against [B2]'s horizon AO on the
    // same synthetic depth buffers. Own derivation: with thickness tau = inf
    // every sample occludes [theta_front, theta_back(R)], and theta_front is
    // never above the side's horizon, so the gather's occluded set is a subset
    // of [B2]'s ([horizon, lobe edge] per side) and
    //   A_gather = A_B2 + Gap,
    // Gap = weight of the directions below a horizon that no segment covers
    // (computed here from this file's own samples). With R unbounded the
    // segments reach theta_inf(q) = pi - psi(q) and Gap is zero unless the
    // union of segments has holes below the horizon.
    const std::vector<Scene> scenes{BlackOccluderScene(96, 64, true), ThickBlockScene(96, 64), CornerRoomScene(96, 64),
                                    PillarsScene(96, 64), ThinBarScene(96, 64)};
    constexpr uint32_t kSections = 8;
    Summary identity, gap, centre, inclusionFinite, excessFinite, identityFinite, gapFinite, asWritten, asWrittenFloor,
        noSelf, steps, stepsFine, equal;
    size_t compared = 0, validMismatch = 0, clampedSides = 0, sides = 0, pixelsWithGap = 0, gapPixelsAtSideBorder = 0;
    for (const Scene& scene : scenes) {
        const GatherInput input = FaceNormalInput(scene);
        const HorizonAo b2(input);
        GatherSettings settings;
        settings.Radius = 1.0e12;    // unbounded radius (finite, as required)
        settings.Thickness = 1.0e30; // unbounded thickness: the segment ends on the R sphere
        settings.Sections = kSections;
        settings.Sectors = 64;
        settings.Mask = MaskMode::ContinuousIntervals;
        settings.Threads = 4;
        const auto gather = GatherImage(input, settings);
        GatherSettings quantized = settings;
        quantized.Mask = MaskMode::CentreRule;
        const auto gatherCentre = GatherImage(input, quantized);
        HorizonOptions options;
        options.Radius = settings.Radius;
        HorizonOptions written = options;
        written.PositivePart = true;
        HorizonOptions keepSelf = options;
        keepSelf.RejectSelf = false;
        HorizonOptions stepped = options;
        stepped.PixelSteps = true;
        Summary sceneIdentity, sceneGap;
        size_t sceneGapPixels = 0;
        for (uint32_t y = 0; y < scene.Camera.Height; ++y)
            for (uint32_t x = 0; x < scene.Camera.Width; ++x) {
                const size_t i = static_cast<size_t>(y) * scene.Camera.Width + x;
                const HorizonResult h = b2.Pixel(x, y, kSections, settings.Rotation, options);
                if (h.Valid != gather[i].Valid) ++validMismatch;
                if (!h.Valid || !gather[i].Valid) continue;
                ++compared;
                const double residual = std::abs(gather[i].ARaw - h.A - h.Gap);
                identity.Add(residual, 1.0e-9);
                sceneIdentity.Add(residual, 1.0e-9);
                gap.Add(h.Gap, 1.0e-9);
                sceneGap.Add(h.Gap, 1.0e-9);
                if (h.Gap > 1.0e-9) {
                    ++pixelsWithGap;
                    ++sceneGapPixels;
                    // Traced (pillars): at the side borders the sections pass beside
                    // the lateral end of the back wall; below the wall's horizon the
                    // section plane is open over the floor (height field not closed).
                    if (x < 4 || x + 4 >= scene.Camera.Width) ++gapPixelsAtSideBorder;
                }
                equal.Add(std::abs(gather[i].ARaw - h.A), 1.0e-9);
                centre.Add(std::abs(gatherCentre[i].ARaw - h.A), 1.0e-2);
                const HorizonResult w = b2.Pixel(x, y, kSections, settings.Rotation, written);
                clampedSides += w.ClampedSides;
                sides += w.Sides;
                asWritten.Add(h.A - w.A, 1.0e-2);
                const size_t bi = static_cast<size_t>(y) * input.BufferWidth + x;
                if (input.FaceNormals[3 * bi + 1] > 0.99) asWrittenFloor.Add(h.A - w.A, 1.0e-2);
                noSelf.Add(h.A - b2.Pixel(x, y, kSections, settings.Rotation, keepSelf).A, 1.0e-3);
                steps.Add(std::abs(b2.Pixel(x, y, kSections, settings.Rotation, stepped).A - h.A), 1.0e-2);
            }
        std::printf("DGI_REPORT B2_heightfield scene=%s R=inf identity_max_abs=%.2e gap_max=%.2e gap_pixels=%zu/%zu\n",
                    scene.Name.c_str(), sceneIdentity.Max, sceneGap.Max, sceneGapPixels, sceneIdentity.Count);
        // Raster convergence of the paper's pixel-step sampling against every
        // crossed texel: the two point sets differ within one texel footprint.
        Scene fine = scene;
        fine.Camera.Width *= 2;
        fine.Camera.Height *= 2;
        const GatherInput fineInput = FaceNormalInput(fine);
        const HorizonAo fineB2(fineInput);
        for (uint32_t y = 0; y < fine.Camera.Height; y += 2)
            for (uint32_t x = 0; x < fine.Camera.Width; x += 2) {
                const HorizonResult h = fineB2.Pixel(x, y, kSections, settings.Rotation, options);
                if (!h.Valid) continue;
                stepsFine.Add(std::abs(fineB2.Pixel(x, y, kSections, settings.Rotation, stepped).A - h.A), 1.0e-2);
            }
    }
    // Finite radius: [B2] occludes every direction below the horizon even when
    // the ray leaves the R ball before reaching geometry; the finite-radius
    // model of the specification (and the gather) leaves it open.
    for (const Scene& scene : {BlackOccluderScene(96, 64, true), PillarsScene(96, 64), OverhangScene(96, 64)}) {
        const GatherInput input = FaceNormalInput(scene);
        const HorizonAo b2(input);
        GatherSettings settings;
        settings.Radius = 2.0;
        settings.Thickness = 1.0e30;
        settings.Sections = kSections;
        settings.Mask = MaskMode::ContinuousIntervals;
        settings.Threads = 4;
        const auto gather = GatherImage(input, settings);
        HorizonOptions options;
        options.Radius = settings.Radius;
        for (uint32_t y = 0; y < scene.Camera.Height; ++y)
            for (uint32_t x = 0; x < scene.Camera.Width; ++x) {
                const size_t i = static_cast<size_t>(y) * scene.Camera.Width + x;
                const HorizonResult h = b2.Pixel(x, y, kSections, settings.Rotation, options);
                if (!h.Valid || !gather[i].Valid) continue;
                identityFinite.Add(std::abs(gather[i].ARaw - h.A - h.Gap), 1.0e-9);
                gapFinite.Add(h.Gap, 1.0e-6);
                inclusionFinite.Add(h.A - gather[i].ARaw, 1.0e-12); // must stay <= 0 (rounding)
                excessFinite.Add(gather[i].ARaw - h.A, 1.0e-6);
            }
    }
    std::printf("DGI_REPORT B2_heightfield R=inf tau=inf pixels=%zu valid_mismatch=%zu | A_gather_cont-(A_B2+Gap) "
                "max_abs=%.2e | Gap max=%.2e mean=%.2e pixels_with_gap=%zu (side_border_columns=%zu) | "
                "|A_gather_cont-A_B2| max=%.2e mean=%.2e over_1e-9=%zu | centre_rule_N64 |A-A_B2| max=%.2e mean=%.2e\n",
                compared, validMismatch, identity.Max, gap.Max, gap.Mean(), pixelsWithGap, gapPixelsAtSideBorder, equal.Max,
                equal.Mean(), equal.Over, centre.Max, centre.Mean());
    std::printf("DGI_REPORT B2_heightfield R=2 tau=inf pixels=%zu | A_gather_cont-(A_B2+Gap) max_abs=%.2e | "
                "A_B2-A_gather max=%.2e (inclusion) | A_gather-A_B2 mean=%.2e max=%.2e pixels_over_1e-6=%zu\n",
                identityFinite.Count, identityFinite.Max, inclusionFinite.Max, excessFinite.Mean(), excessFinite.Max,
                excessFinite.Over);
    std::printf("DGI_REPORT B2_as_written eq6_positive_part A_adapted-A_as_written mean=%.4f max=%.4f "
                "floor_receivers mean=%.4f max=%.4f clamped_sides=%zu/%zu\n",
                asWritten.Mean(), asWritten.Max, asWrittenFloor.Mean(), asWrittenFloor.Max, clampedSides, sides);
    std::printf("DGI_REPORT B2_self_samples A_rejected-A_kept mean=%.2e max=%.2e pixels_over_1e-3=%zu/%zu\n", noSelf.Mean(),
                noSelf.Max, noSelf.Over, noSelf.Count);
    std::printf("DGI_REPORT B2_pixel_steps |A_steps-A_every_texel| 96x64 mean=%.2e max=%.2e | 192x128 mean=%.2e max=%.2e "
                "ratio_of_means=%.2f\n",
                steps.Mean(), steps.Max, stepsFine.Mean(), stepsFine.Max, steps.Mean() / std::max(stepsFine.Mean(), 1e-300));
    EXPECT_GT(compared, 20000U);
    EXPECT_EQ(validMismatch, 0U);
    // Same samples, same thickness model: the identity holds to rounding.
    EXPECT_LT(identity.Max, 1.0e-9);
    EXPECT_LT(identityFinite.Max, 1.0e-9);
    // Inclusion: the gather never occludes more than [B2] (rounding only).
    EXPECT_LT(inclusionFinite.Max, 1.0e-12);
    EXPECT_GE(gap.Min, -1.0e-15);
    // Height fields closed below the horizon: infinite thickness reproduces
    // [B2] on almost every receiver (the gap is the derived exception).
    EXPECT_LT(pixelsWithGap, compared / 20);
    EXPECT_EQ(pixelsWithGap, gapPixelsAtSideBorder);
    // As written, eq. 6 darkens tilted receivers; the positive part matters.
    EXPECT_GT(asWrittenFloor.Mean(), 0.1);
    // Raster: the paper's pixel steps converge to every-texel sampling at first order.
    EXPECT_LT(stepsFine.Mean(), 0.75 * steps.Mean());
}
