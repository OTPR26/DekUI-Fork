#include "fast/renderer/diffuse_gi_gather.h"

// Gathering and sector masks of the diffuse screen-space GI (milestone M2).
// Every rule here is taken from the specification (sections 1, 4, 9, 10) or
// derived in docs/TRIAEVUM_SCREEN_SPACE_GI.md. The template core runs in
// double (reference) and in float (twin of the GPU kernel, same operation
// order); see diffuse_gi_gather_shader.cpp for the GLSL counterpart.
// M4 adds, behind switches that default to off, the metric radial
// distribution, hierarchy depth and radiance cache for sparse samples,
// guiding of phi and the blue-noise rotation; with every switch off the
// M1-M3 paths below run unchanged.

#include "fast/renderer/diffuse_gi_guiding.h"
#include "fast/renderer/diffuse_gi_hierarchy.h"
#include "fast/renderer/diffuse_gi_noise.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>
#include <type_traits>

namespace Fast::Renderer::DiffuseGi {
namespace {

struct Vec3F {
    float X = 0.0F, Y = 0.0F, Z = 0.0F;
};
template <typename T>
using VecT = std::conditional_t<std::is_same_v<T, double>, Vec3, Vec3F>;

template <typename V>
V VAdd(const V& a, const V& b) {
    return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
}
template <typename V>
V VSub(const V& a, const V& b) {
    return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
}
template <typename V, typename T>
V VScale(const V& a, T s) {
    return {a.X * s, a.Y * s, a.Z * s};
}
template <typename V>
auto VDot(const V& a, const V& b) {
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}
template <typename V>
V VCross(const V& a, const V& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
template <typename V>
auto VLength(const V& a) {
    return std::sqrt(VDot(a, a));
}
template <typename V>
V VNormalize(const V& a) {
    const auto length = VLength(a);
    using T = std::remove_const_t<decltype(length)>;
    return length > T(0) ? VScale(a, T(1) / length) : V{};
}

template <typename T>
constexpr T Pi() {
    return static_cast<T>(kPi);
}

bool IsGeometry(uint8_t c) {
    return c == static_cast<uint8_t>(TexelClass::OneSided) || c == static_cast<uint8_t>(TexelClass::TwoSided);
}

template <typename T>
T DecodeDepthT(T d, DepthEncoding encoding, T n, T f, bool infiniteFar) {
    switch (encoding) {
    case DepthEncoding::PerspectiveZeroToOne:
        return n * f / (f - d * (f - n));
    case DepthEncoding::ReversedPerspectiveZeroToOne:
        return infiniteFar ? n / d : n * f / (n + d * (f - n));
    case DepthEncoding::LinearWBuffer:
        return n + d * (f - n);
    case DepthEncoding::ReversedLinearWBuffer:
        return f - d * (f - n);
    default:
        return std::numeric_limits<T>::quiet_NaN();
    }
}

// Read-only view of the prepared geometry of one viewport.
template <typename T>
struct View {
    const GatherInput* In = nullptr;
    uint32_t W = 0, H = 0;
    T Ax{}, Bx{}, Cx{}, Ay{}, By{}, Cy{};
    const T* Z = nullptr;
    const uint8_t* Cls = nullptr;
    const VecT<T>* N = nullptr;
    const VecT<T>* S = nullptr;              // M8 oriented shading normals (null: none)
    const GatherAcceleration* Acc = nullptr; // M4 inputs (null: M1-M3)

    [[nodiscard]] VecT<T> Ray(T px, T py) const {
        const T u = px / static_cast<T>(W);
        const T v = py / static_cast<T>(H);
        return {Ax * u + Bx * v + Cx, Ay * u + By * v + Cy, T(-1)};
    }
    [[nodiscard]] bool Inside(int32_t x, int32_t y) const {
        return x >= 0 && y >= 0 && x < static_cast<int32_t>(W) && y < static_cast<int32_t>(H);
    }
    [[nodiscard]] size_t Index(int32_t x, int32_t y) const {
        return static_cast<size_t>(y) * W + static_cast<size_t>(x);
    }
    [[nodiscard]] size_t BufferIndex(int32_t x, int32_t y) const {
        return (static_cast<size_t>(In->Viewport.Y) + static_cast<size_t>(y)) * In->BufferWidth + In->Viewport.X +
               static_cast<size_t>(x);
    }
};

template <typename T>
View<T> MakeView(const GatherInput& in) {
    View<T> view;
    view.In = &in;
    view.W = in.Viewport.Width;
    view.H = in.Viewport.Height;
    view.Ax = static_cast<T>(in.Camera.ViewX[0]);
    view.Bx = static_cast<T>(in.Camera.ViewX[1]);
    view.Cx = static_cast<T>(in.Camera.ViewX[2]);
    view.Ay = static_cast<T>(in.Camera.ViewY[0]);
    view.By = static_cast<T>(in.Camera.ViewY[1]);
    view.Cy = static_cast<T>(in.Camera.ViewY[2]);
    return view;
}

template <typename T>
struct GeometryData {
    std::vector<T> Z;
    std::vector<uint8_t> Cls;
    std::vector<VecT<T>> N;
    std::vector<VecT<T>> S; // M8: empty without shading normals
};

// Geometric normal from depth (own derivation, see the document). On a plane
// w = 1/z is affine in screen space. A quadrant (sx, sy) around the receiver
// is scored by the planarity errors it can test: the 2x2 term
// |w(d) - w(x) - w(y) + w(p)| (d the diagonal) and the second differences
// |w(p) - 2 w(x) + w(2x)| and |w(p) - 2 w(y) + w(2y)| along its two sides.
// The best quadrant gives both tangents, so a texel on an edge never mixes
// the tangents of two planes: the 2x2 term catches creases oblique to the
// pixel grid, the second differences catch creases along a screen axis whose
// planes share that axis' 1/z gradient. Errors within kNormalTie * w(p) are
// ties (FP32 rounding must not decide between two planar quadrants); a tie
// goes to the quadrant with more verified second differences (a missing
// second neighbour verifies nothing), then to the fixed priority (+x+y, -x+y,
// +x-y, -x-y). A quadrant without its diagonal ranks after every full one and
// adds its depth steps. No quadrant: no normal.
inline constexpr double kNormalTie = 4.0e-6;

template <typename T>
VecT<T> ReconstructNormal(const View<T>& g, int32_t x, int32_t y) {
    const auto geometry = [&](int32_t xx, int32_t yy) { return g.Inside(xx, yy) && IsGeometry(g.Cls[g.Index(xx, yy)]); };
    const auto w = [&](int32_t xx, int32_t yy) { return T(1) / g.Z[g.Index(xx, yy)]; };
    const T w0 = w(x, y);
    const T tie = static_cast<T>(kNormalTie) * w0;
    constexpr int kOrder[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    int best = -1, bestRank = 2, bestVerified = 0;
    T bestError = T(0);
    for (int k = 0; k < 4; ++k) {
        const int sx = kOrder[k][0], sy = kOrder[k][1];
        if (!geometry(x + sx, y) || !geometry(x, y + sy)) continue;
        const T wx = w(x + sx, y);
        const T wy = w(x, y + sy);
        int rank = 1;
        int verified = 0;
        T error = std::abs(wx - w0) + std::abs(wy - w0);
        if (geometry(x + sx, y + sy)) {
            rank = 0;
            error = std::abs(w(x + sx, y + sy) - wx - wy + w0);
        }
        if (geometry(x + 2 * sx, y)) {
            error += std::abs(w0 - T(2) * wx + w(x + 2 * sx, y));
            ++verified;
        }
        if (geometry(x, y + 2 * sy)) {
            error += std::abs(w0 - T(2) * wy + w(x, y + 2 * sy));
            ++verified;
        }
        const bool better = best < 0 || rank < bestRank ||
                            (rank == bestRank && (error < bestError - tie ||
                                                  (!(error > bestError + tie) && verified > bestVerified)));
        if (better) {
            best = k;
            bestRank = rank;
            bestError = error;
            bestVerified = verified;
        }
    }
    if (best < 0) return {};
    const int sx = kOrder[best][0], sy = kOrder[best][1];
    const VecT<T> p0 = VScale(g.Ray(T(x) + T(0.5), T(y) + T(0.5)), g.Z[g.Index(x, y)]);
    const VecT<T> px = VScale(g.Ray(T(x + sx) + T(0.5), T(y) + T(0.5)), g.Z[g.Index(x + sx, y)]);
    const VecT<T> py = VScale(g.Ray(T(x) + T(0.5), T(y + sy) + T(0.5)), g.Z[g.Index(x, y + sy)]);
    const VecT<T> tx = VScale(VSub(px, p0), T(sx));
    const VecT<T> ty = VScale(VSub(py, p0), T(sy));
    VecT<T> n = VNormalize(VCross(tx, ty));
    if (VDot(n, p0) > T(0)) n = VScale(n, T(-1));
    return n;
}

template <typename T>
GeometryData<T> PrepareT(const GatherInput& in, View<T>& view) {
    view = MakeView<T>(in);
    GeometryData<T> data;
    const size_t count = static_cast<size_t>(view.W) * view.H;
    data.Z.assign(count, T(0));
    data.Cls.assign(count, static_cast<uint8_t>(TexelClass::Unavailable));
    data.N.assign(count, VecT<T>{});
    const bool infiniteFar = std::isinf(in.Camera.Far);
    const T n = static_cast<T>(in.Camera.Near);
    const T f = infiniteFar ? T(0) : static_cast<T>(in.Camera.Far);
    const T low = n * T(1.0 - 1.0e-5);
    const T high = infiniteFar ? std::numeric_limits<T>::infinity() : f * T(1.0 + 1.0e-5);
    for (uint32_t y = 0; y < view.H; ++y)
        for (uint32_t x = 0; x < view.W; ++x) {
            const size_t bi = view.BufferIndex(static_cast<int32_t>(x), static_cast<int32_t>(y));
            const size_t i = view.Index(static_cast<int32_t>(x), static_cast<int32_t>(y));
            const uint8_t c = in.Class[bi];
            if (IsGeometry(c)) {
                const T z = DecodeDepthT<T>(static_cast<T>(in.Depth[bi]), in.Camera.Encoding, n, f, infiniteFar);
                if (std::isfinite(z) && z >= low && z <= high) {
                    data.Z[i] = z;
                    data.Cls[i] = c;
                }
            } else if (c == static_cast<uint8_t>(TexelClass::Sky)) {
                data.Cls[i] = c;
            }
        }
    view.Z = data.Z.data();
    view.Cls = data.Cls.data();
    for (uint32_t y = 0; y < view.H; ++y)
        for (uint32_t x = 0; x < view.W; ++x) {
            const auto xi = static_cast<int32_t>(x), yi = static_cast<int32_t>(y);
            const size_t i = view.Index(xi, yi);
            if (!IsGeometry(data.Cls[i])) continue;
            if (in.NormalSource == GeometricNormalSource::RendererFaceNormal) {
                const size_t bi = view.BufferIndex(xi, yi);
                VecT<T> face = VNormalize(VecT<T>{static_cast<T>(in.FaceNormals[3 * bi]),
                                                  static_cast<T>(in.FaceNormals[3 * bi + 1]),
                                                  static_cast<T>(in.FaceNormals[3 * bi + 2])});
                const VecT<T> p = VScale(view.Ray(T(x) + T(0.5), T(y) + T(0.5)), data.Z[i]);
                if (VDot(face, p) > T(0)) face = VScale(face, T(-1));
                data.N[i] = face;
            } else {
                data.N[i] = ReconstructNormal(view, xi, yi);
            }
        }
    view.N = data.N.data();
    // M8: the shading normal of every geometry texel with a geometric normal,
    // unit and on the geometric normal's side (the twin of the normal kernel).
    if (in.ShadingNormals.size() == 3U * static_cast<size_t>(in.BufferWidth) * in.BufferHeight) {
        data.S.assign(count, VecT<T>{});
        for (uint32_t y = 0; y < view.H; ++y)
            for (uint32_t x = 0; x < view.W; ++x) {
                const auto xi = static_cast<int32_t>(x), yi = static_cast<int32_t>(y);
                const size_t i = view.Index(xi, yi);
                const VecT<T>& n = data.N[i];
                if (!IsGeometry(data.Cls[i]) || !(VDot(n, n) > T(0))) continue;
                const size_t bi = view.BufferIndex(xi, yi);
                VecT<T> s = VNormalize(VecT<T>{static_cast<T>(in.ShadingNormals[3 * bi]),
                                               static_cast<T>(in.ShadingNormals[3 * bi + 1]),
                                               static_cast<T>(in.ShadingNormals[3 * bi + 2])});
                if (!std::isfinite(s.X) || !std::isfinite(s.Y) || !std::isfinite(s.Z)) s = VecT<T>{};
                if (VDot(s, n) < T(0)) s = VScale(s, T(-1));
                data.S[i] = s;
            }
        view.S = data.S.data();
    }
    return data;
}

// ---------------------------------------------------------------------------
// Section geometry.

template <typename T>
struct SectionT {
    bool Valid = false;
    VecT<T> P, V, Tn, N;
    VecT<T> NS; // lobe normal: the shading normal (M8) or N
    T A{}, B{}, ZP{}, DirX{}, DirY{};
    T Extent[2]{};
    T Distance{}; // |P| (M4 metric radial distribution)
};

template <typename T>
SectionT<T> SetupT(const View<T>& g, uint32_t x, uint32_t y, T phi, T radius, bool shading = false) {
    SectionT<T> s;
    const size_t index = g.Index(static_cast<int32_t>(x), static_cast<int32_t>(y));
    if (!IsGeometry(g.Cls[index])) return s;
    const VecT<T> n = g.N[index];
    if (!(VDot(n, n) > T(0))) return s;
    s.ZP = g.Z[index];
    s.P = VScale(g.Ray(T(x) + T(0.5), T(y) + T(0.5)), s.ZP);
    s.V = VNormalize(VScale(s.P, T(-1)));
    // Deterministic frame completion of M1 (SectionFrame::FromView).
    const VecT<T> helper = std::abs(s.V.X) < T(0.9) ? VecT<T>{T(1), T(0), T(0)} : VecT<T>{T(0), T(1), T(0)};
    const VecT<T> e1 = VNormalize(VCross(helper, s.V));
    const VecT<T> e2 = VCross(s.V, e1);
    s.Tn = VAdd(VScale(e1, std::cos(phi)), VScale(e2, std::sin(phi)));
    s.N = n;
    // M8: the lobe of the shading normal (the geometric normal without one).
    s.NS = n;
    if (shading && g.S != nullptr && VDot(g.S[index], g.S[index]) > T(0)) s.NS = g.S[index];
    s.A = VDot(s.NS, s.V);
    s.B = VDot(s.NS, s.Tn);
    // Screen direction of +t: derivative of (X/z, Y/z) along P + mu t, mapped
    // through the inverse of the declared UV -> ray map.
    const T zp = -s.P.Z;
    const T gx = s.Tn.X * zp + s.P.X * s.Tn.Z;
    const T gy = s.Tn.Y * zp + s.P.Y * s.Tn.Z;
    const T det = g.Ax * g.By - g.Bx * g.Ay;
    const T du = (g.By * gx - g.Bx * gy) / det;
    const T dv = (-g.Ay * gx + g.Ax * gy) / det;
    const T pxDir = du * static_cast<T>(g.W);
    const T pyDir = dv * static_cast<T>(g.H);
    const T length = std::sqrt(pxDir * pxDir + pyDir * pyDir);
    if (!(length > T(0)) || !std::isfinite(length)) return s;
    s.DirX = pxDir / length;
    s.DirY = pyDir / length;
    // Projected radius per side: rays of the section plane at angle
    // asin(R / |P|) from the receiver's ray bound the R-ball (exact).
    const T cap = std::sqrt(static_cast<T>(g.W) * static_cast<T>(g.W) + static_cast<T>(g.H) * static_cast<T>(g.H));
    const T distance = VLength(s.P);
    s.Distance = distance;
    for (int side = 0; side < 2; ++side) {
        T extent = cap;
        if (radius < distance) {
            const T sa = radius / distance;
            const T ca = std::sqrt(std::max(T(1) - sa * sa, T(0)));
            const T sign = side == 0 ? T(1) : T(-1);
            const VecT<T> r = VAdd(VScale(s.V, -ca), VScale(s.Tn, sign * sa));
            if (r.Z < T(0)) {
                const T tx = r.X / -r.Z - s.P.X / zp;
                const T ty = r.Y / -r.Z - s.P.Y / zp;
                const T eu = (g.By * tx - g.Bx * ty) / det * static_cast<T>(g.W);
                const T ev = (-g.Ay * tx + g.Ax * ty) / det * static_cast<T>(g.H);
                extent = std::min(std::sqrt(eu * eu + ev * ev), cap);
            }
        }
        s.Extent[side] = extent;
    }
    s.Valid = true;
    return s;
}

// ---------------------------------------------------------------------------
// Candidate schedule.

template <typename T>
struct Position {
    T Lambda{};
    int Side = 1;
    int32_t CellX = 0, CellY = 0;
    // M4: footprint level after descent (0: point reference) and the
    // hierarchy cell of that level (absolute cell coordinates); the level
    // requested by the spacing before the descent, and the spacing (pixels).
    uint32_t Level = 0;
    int32_t HX = 0, HY = 0;
    uint32_t Requested = 0;
    T Spacing{};
};

// M4: radial samples use hierarchy levels when the hierarchy depth or the
// radiance cache is on (sparse schedules only; EveryTexel is the reference).
template <typename T>
bool UsesLevels(const View<T>& g, const GatherSettings& settings) {
    return g.Acc != nullptr && g.Acc->Hierarchy != nullptr &&
           (settings.HierarchyFootprint || settings.Cache != CacheMode::Off) &&
           settings.Schedule != CandidateSchedule::EveryTexel;
}

// M4 metric radial distribution: screen distance (pixels) of the camera ray
// of the section plane whose closest distance to the receiver is rho, i.e.
// the ray at angle asin(rho / |P|) from the receiver's ray. Same construction
// as the projected radius of SetupT with rho in place of R (rho = R gives the
// extent); the viewport diagonal when the ray leaves the screen.
template <typename T>
T MetricScreenDistance(const View<T>& g, const SectionT<T>& s, T rho, int side, T cap) {
    if (!(rho < s.Distance)) return cap;
    const T sa = rho / s.Distance;
    const T ca = std::sqrt(std::max(T(1) - sa * sa, T(0)));
    const T sign = side > 0 ? T(1) : T(-1);
    const VecT<T> r = VAdd(VScale(s.V, -ca), VScale(s.Tn, sign * sa));
    if (!(r.Z < T(0))) return cap;
    const T zp = -s.P.Z;
    const T det = g.Ax * g.By - g.Bx * g.Ay;
    const T tx = r.X / -r.Z - s.P.X / zp;
    const T ty = r.Y / -r.Z - s.P.Y / zp;
    const T eu = (g.By * tx - g.Bx * ty) / det * static_cast<T>(g.W);
    const T ev = (-g.Ay * tx + g.Ax * ty) / det * static_cast<T>(g.H);
    return std::min(std::sqrt(eu * eu + ev * ev), cap);
}

// M4 radial positions (Radial with hierarchy or cache, or MetricRadial):
// the natural sequence lambda_k, then per sample the footprint level from its
// spacing along the line and the descent to the first planar cell (level 0:
// the M2 point reference). One texel (level 0) or one cell (level l) is one
// surface sample: a repeat of the previous key on this side is skipped.
template <typename T>
void EnumerateRadialM4(const View<T>& g, const SectionT<T>& s, uint32_t x, uint32_t y, int side,
                       const GatherSettings& settings, std::vector<Position<T>>& out, GatherCounters& counters) {
    const T dx = s.DirX * T(side);
    const T dy = s.DirY * T(side);
    const T adx = std::abs(dx);
    const T ady = std::abs(dy);
    const T infinity = std::numeric_limits<T>::infinity();
    const T invX = adx > T(0) ? T(1) / adx : infinity;
    const T invY = ady > T(0) ? T(1) / ady : infinity;
    const T extent = s.Extent[side > 0 ? 0 : 1];
    const uint32_t count = std::max(settings.CandidatesPerSide, 1U);
    const T jitter = static_cast<T>(settings.RadialJitter);
    const T exponent = static_cast<T>(settings.RadialExponent);
    T lambdas[256];
    uint32_t total = 0;
    if (settings.Schedule == CandidateSchedule::Radial) {
        const T lambda0 = std::min(T(0.5) * invX, T(0.5) * invY);
        if (!(extent > lambda0)) return;
        for (uint32_t k = 0; k < count && total < 256U; ++k) {
            const T u = (T(k) + jitter) / T(count);
            const T shaped = u > T(0) ? std::pow(u, exponent) : T(0);
            lambdas[total++] = lambda0 + (extent - lambda0) * shaped;
        }
    } else {
        // Contact sample: the midpoint of the line's chord through the first
        // texel it enters after the receiver's (the first texel of the
        // EveryTexel traversal; an exact corner enters the diagonal texel).
        const T tx0 = T(0.5) * invX, ty0 = T(0.5) * invY;
        const T enter = std::min(tx0, ty0);
        const T exit = tx0 < ty0 ? std::min(T(3) * tx0, ty0) : (ty0 < tx0 ? std::min(tx0, T(3) * ty0) : T(3) * tx0);
        const T contact = T(0.5) * (enter + exit);
        if (!(extent > contact)) return;
        const T cap = std::sqrt(static_cast<T>(g.W) * static_cast<T>(g.W) + static_cast<T>(g.H) * static_cast<T>(g.H));
        const T radius = static_cast<T>(settings.Radius);
        for (uint32_t k = 0; k < count && total < 256U; ++k) {
            const T u = (T(k) + jitter) / T(count);
            const T shaped = u > T(0) ? std::pow(u, exponent) : T(0);
            T lambda = MetricScreenDistance(g, s, radius * shaped, side, cap);
            if (lambda < contact) {
                lambda = contact;
                ++counters.Contact;
            }
            lambdas[total++] = lambda;
        }
    }
    const bool levels = UsesLevels(g, settings);
    const DepthHierarchy* hierarchy = levels ? g.Acc->Hierarchy : nullptr;
    const uint32_t maxLevel =
        levels ? std::min(settings.HierarchyMaxLevel, static_cast<uint32_t>(hierarchy->Levels.size()) - 1U) : 0U;
    int32_t previousX = 0, previousY = 0;
    uint32_t previousLevel = 0;
    for (uint32_t k = 0; k < total; ++k) {
        const T lambda = lambdas[k];
        if (lambda > extent) break; // monotone sequence: the rest lies beyond the R-ball
        const auto cx = static_cast<int32_t>(std::floor(lambda * dx + T(0.5)));
        const auto cy = static_cast<int32_t>(std::floor(lambda * dy + T(0.5)));
        uint32_t level = 0, requested = 0;
        int32_t hx = cx, hy = cy; // level 0 keys: texel offsets (the receiver is (0, 0))
        const int32_t tx = static_cast<int32_t>(x) + cx, ty = static_cast<int32_t>(y) + cy;
        const T spacing = k + 1U < total ? lambdas[k + 1U] - lambda : (k > 0U ? lambda - lambdas[k - 1U] : extent - lambda);
        if (levels) requested = FootprintLevel<T>(spacing * static_cast<T>(settings.HierarchySpacingScale), maxLevel);
        if (levels && g.Inside(tx, ty) && !(cx == 0 && cy == 0)) {
            level = requested;
            while (level > 0U) {
                const auto ix = static_cast<uint32_t>(tx) >> level;
                const auto iy = static_cast<uint32_t>(ty) >> level;
                // The receiver's own footprint is never a coarse candidate.
                const bool own = (x >> level) == ix && (y >> level) == iy;
                if (!own && (hierarchy->Cell(level, ix, iy).Flags & kCellPlanar) != 0U) {
                    hx = static_cast<int32_t>(ix);
                    hy = static_cast<int32_t>(iy);
                    break;
                }
                ++counters.Descended;
                --level;
            }
            if (level == 0U) hx = cx, hy = cy;
        }
        // Duplicate key: the strip's cell with the footprint, else the texel
        // (the cache alone changes only the radiance of a point sample).
        const uint32_t keyLevel = settings.HierarchyFootprint ? level : 0U;
        const int32_t keyX = settings.HierarchyFootprint ? hx : cx, keyY = settings.HierarchyFootprint ? hy : cy;
        if ((cx == 0 && cy == 0) || (keyLevel == previousLevel && keyX == previousX && keyY == previousY)) {
            ++counters.Duplicates;
            continue;
        }
        previousLevel = keyLevel;
        previousX = keyX;
        previousY = keyY;
        Position<T> position{lambda, side, cx, cy};
        position.Level = level;
        position.HX = hx;
        position.HY = hy;
        position.Requested = requested;
        position.Spacing = spacing;
        out.push_back(position);
    }
}

template <typename T>
void EnumerateSide(const View<T>& g, const SectionT<T>& s, int side, const GatherSettings& settings,
                   std::vector<Position<T>>& out, GatherCounters& counters) {
    out.clear();
    const T dx = s.DirX * T(side);
    const T dy = s.DirY * T(side);
    const T adx = std::abs(dx);
    const T ady = std::abs(dy);
    const T infinity = std::numeric_limits<T>::infinity();
    const T invX = adx > T(0) ? T(1) / adx : infinity;
    const T invY = ady > T(0) ? T(1) / ady : infinity;
    const int32_t sx = dx > T(0) ? 1 : (dx < T(0) ? -1 : 0);
    const int32_t sy = dy > T(0) ? 1 : (dy < T(0) ? -1 : 0);
    const T extent = s.Extent[side > 0 ? 0 : 1];
    if (settings.Schedule == CandidateSchedule::EveryTexel) {
        // Grid traversal from the receiver centre; boundary parameters are
        // (k + 1/2) / |d| exactly (no accumulated increments).
        int32_t cx = 0, cy = 0;
        uint32_t kx = 0, ky = 0;
        T enter = T(0);
        const T epsilon = static_cast<T>(settings.ChordEpsilon);
        const uint32_t limit = 2U * (g.W + g.H) + 16U;
        for (uint32_t iteration = 0; iteration < limit; ++iteration) {
            const T tx = adx > T(0) ? (T(kx) + T(0.5)) * invX : infinity;
            const T ty = ady > T(0) ? (T(ky) + T(0.5)) * invY : infinity;
            const T exit = std::min(tx, ty);
            if (cx != 0 || cy != 0) {
                if (!(enter < extent)) break;
                if (exit - enter > epsilon) {
                    // Line point nearest to the texel centre, within the chord.
                    const T foot = std::clamp(T(cx) * dx + T(cy) * dy, enter, exit);
                    out.push_back({foot, side, cx, cy});
                }
            }
            if (tx < ty) {
                cx += sx;
                enter = tx;
                ++kx;
            } else if (ty < tx) {
                cy += sy;
                enter = ty;
                ++ky;
            } else {
                // Exact corner: both corner-touching texels have empty chords.
                cx += sx;
                cy += sy;
                enter = tx;
                ++kx;
                ++ky;
            }
        }
        return;
    }
    const T lambda0 = std::min(T(0.5) * invX, T(0.5) * invY);
    if (!(extent > lambda0)) return;
    const uint32_t count = std::max(settings.CandidatesPerSide, 1U);
    int32_t previousX = 0, previousY = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const T u = (T(k) + static_cast<T>(settings.RadialJitter)) / T(count);
        const T shaped = u > T(0) ? std::pow(u, static_cast<T>(settings.RadialExponent)) : T(0);
        const T lambda = lambda0 + (extent - lambda0) * shaped;
        const auto cx = static_cast<int32_t>(std::floor(lambda * dx + T(0.5)));
        const auto cy = static_cast<int32_t>(std::floor(lambda * dy + T(0.5)));
        if ((cx == 0 && cy == 0) || (cx == previousX && cy == previousY)) {
            ++counters.Duplicates; // one texel is one surface sample
            continue;
        }
        previousX = cx;
        previousY = cy;
        out.push_back({lambda, side, cx, cy});
    }
}

template <typename T>
void MergeSides(const std::vector<Position<T>>& plus, const std::vector<Position<T>>& minus, bool interleave,
                std::vector<Position<T>>& out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < plus.size() || j < minus.size()) {
        bool takePlus = false;
        if (j >= minus.size()) takePlus = true;
        else if (i >= plus.size()) takePlus = false;
        else takePlus = !interleave || plus[i].Lambda <= minus[j].Lambda; // tie: + side first
        out.push_back(takePlus ? plus[i++] : minus[j++]);
    }
}

// ---------------------------------------------------------------------------
// Candidate evaluation.

template <typename T>
struct CandidateT {
    T Lambda{};
    int Side = 1;
    int32_t CellX = 0, CellY = 0;
    uint32_t Pieces = 0;
    T Lo[2]{}, Hi[2]{};
    T R{}, G{}, B{};
    VecT<T> Front, Back;
    uint32_t Level = 0; // M4 (diagnostics)
    int32_t HX = 0, HY = 0;
};

// M8: a candidate on the receiver's own smooth surface (GatherSettings): the
// shading normals agree, the shading turns less than the faces (no hard edge)
// and the two face planes meet between the points.
template <typename T>
bool OwnSmoothSurfaceT(const SectionT<T>& s, const VecT<T>& d, T length, const VecT<T>& nq, const VecT<T>& sq,
                       const GatherSettings& settings) {
    if (!(VDot(sq, sq) > T(0)) || !(VDot(nq, nq) > T(0))) return false;
    const T cf = VDot(s.N, nq);
    const T cs = VDot(s.NS, sq);
    if (!(cs >= static_cast<T>(settings.OwnSurfaceCos))) return false;
    if (!(T(1) - cs < static_cast<T>(settings.HardEdgeRatio) * (T(1) - cf))) return false;
    const T sf = std::sqrt(std::max(T(1) - cf * cf, T(0)));
    const T tolerance = (static_cast<T>(settings.SelfIntersectionSine) + sf) * length +
                        static_cast<T>(settings.SelfIntersectionDepth) * s.ZP;
    return std::abs(VDot(s.N, d)) <= tolerance && std::abs(VDot(nq, d)) <= tolerance;
}

// M8 visibility of a point e of a candidate (face normal nq, shading normal sq
// of its texel): hidden on the receiver's facet (face plane), below its
// shading tangent plane, or on its own smooth surface. With the lobe normal
// equal to the geometric normal and flat-shaded candidates (sq = nq) this is
// the M2 elevation test.
template <typename T>
bool HiddenM8T(const SectionT<T>& s, const VecT<T>& e, const VecT<T>& nq, const VecT<T>& sq,
               const GatherSettings& settings) {
    const VecT<T> d = VSub(e, s.P);
    const T length = VLength(d);
    const T tolerance = static_cast<T>(settings.SelfIntersectionSine) * length +
                        static_cast<T>(settings.SelfIntersectionDepth) * s.ZP;
    if (std::abs(VDot(d, s.N)) <= tolerance) return true;
    if (VDot(d, s.NS) <= tolerance) return true;
    return OwnSmoothSurfaceT(s, d, length, nq, sq, settings);
}

template <typename T>
bool EvaluateCandidate(const View<T>& g, const SectionT<T>& s, uint32_t x, uint32_t y, const Position<T>& p,
                       const GatherSettings& settings, GatherCounters& counters, CandidateT<T>& out) {
    ++counters.Scheduled;
    const int32_t xx = static_cast<int32_t>(x) + p.CellX;
    const int32_t yy = static_cast<int32_t>(y) + p.CellY;
    if (!g.Inside(xx, yy)) return false; // outside the viewport: unavailable
    const size_t index = g.Index(xx, yy);
    const uint8_t cls = g.Cls[index];
    if (cls == static_cast<uint8_t>(TexelClass::Unavailable)) return false;
    ++counters.Valid;
    if (cls == static_cast<uint8_t>(TexelClass::Sky)) {
        ++counters.Sky; // observed and empty: open
        return false;
    }
    const T zq = g.Z[index];
    const T radius = static_cast<T>(settings.Radius);
    if (settings.RejectSelfIntersection) {
        // Coplanarity of the exactly reconstructed texel-centre point with the
        // receiver's tangent plane (the line point carries the footprint error).
        const VecT<T> centre = VScale(g.Ray(T(xx) + T(0.5), T(yy) + T(0.5)), zq);
        if (settings.ShadingNormals && g.S != nullptr) {
            if (HiddenM8T(s, centre, g.N[index], g.S[index], settings)) {
                ++counters.SelfRejected;
                return false;
            }
        } else {
            const VecT<T> d = VSub(centre, s.P);
            const T elevation = VDot(d, s.N);
            if (elevation <= static_cast<T>(settings.SelfIntersectionSine) * VLength(d) +
                                 static_cast<T>(settings.SelfIntersectionDepth) * s.ZP) {
                ++counters.SelfRejected;
                return false;
            }
        }
    }
    const T sign = T(p.Side);
    const T px = T(x) + T(0.5) + p.Lambda * s.DirX * sign;
    const T py = T(y) + T(0.5) + p.Lambda * s.DirY * sign;
    const VecT<T> q = VScale(g.Ray(px, py), zq);
    const VecT<T> rel = VSub(q, s.P);
    const T dist = VLength(rel);
    if (dist > radius) {
        ++counters.BeyondRadius;
        return false;
    }
    // Back segment along the camera ray toward q, clipped to the R-sphere.
    const VecT<T> dir = VNormalize(q);
    const T along = VDot(rel, dir);
    const T clip = -along + std::sqrt(std::max(along * along - (dist * dist - radius * radius), T(0)));
    const T length = std::max(std::min(static_cast<T>(settings.Thickness), clip), T(0));
    const VecT<T> back = VAdd(q, VScale(dir, length));
    // Policy: a segment that meets the receiver is degenerate and rejected.
    const T closestAt = std::clamp(-along, T(0), length);
    if (VLength(VAdd(rel, VScale(dir, closestAt))) <= T(1.0e-6) * s.ZP) {
        ++counters.Degenerate;
        return false;
    }
    const VecT<T> relBack = VSub(back, s.P);
    const T thetaF = std::atan2(VDot(rel, s.Tn), VDot(rel, s.V));
    const T thetaB = std::atan2(VDot(relBack, s.Tn), VDot(relBack, s.V));
    const T pi = Pi<T>();
    const T twoPi = T(2) * pi;
    T delta = thetaB - thetaF;
    if (delta > pi) delta -= twoPi;
    else if (delta <= -pi) delta += twoPi;
    const T lo = delta >= T(0) ? thetaF : thetaF + delta;
    const T hi = delta >= T(0) ? thetaF + delta : thetaF;
    out.Pieces = 0;
    if (hi > lo) {
        if (lo < -pi) {
            out.Lo[0] = lo + twoPi, out.Hi[0] = pi, out.Lo[1] = -pi, out.Hi[1] = hi;
            out.Pieces = 2;
        } else if (hi > pi) {
            out.Lo[0] = lo, out.Hi[0] = pi, out.Lo[1] = -pi, out.Hi[1] = hi - twoPi;
            out.Pieces = 2;
        } else {
            out.Lo[0] = lo, out.Hi[0] = hi;
            out.Pieces = 1;
        }
        ++counters.Occupying;
    }
    // One-sided source radiance toward the receiver (no source cosine).
    const size_t bi = g.BufferIndex(xx, yy);
    T r = static_cast<T>(g.In->Radiance[3 * bi]);
    T gg = static_cast<T>(g.In->Radiance[3 * bi + 1]);
    T b = static_cast<T>(g.In->Radiance[3 * bi + 2]);
    if (cls == static_cast<uint8_t>(TexelClass::OneSided)) {
        const VecT<T>& nq = g.N[index];
        if (VDot(nq, nq) > T(0)) {
            if (!(VDot(nq, VSub(s.P, q)) > T(0))) {
                r = gg = b = T(0);
                ++counters.BackFacing;
            }
        } else {
            ++counters.MissingSourceNormal;
        }
    }
    out.Lambda = p.Lambda;
    out.Side = p.Side;
    out.CellX = p.CellX;
    out.CellY = p.CellY;
    out.R = r, out.G = gg, out.B = b;
    out.Front = q;
    out.Back = back;
    return true;
}

// M4: a radial sample whose footprint level is above 0 after the descent. Its
// cell is planar (one surface of one class, all geometry), does not hold the
// receiver's texel, and the sample's level-0 texel lies inside the viewport and
// inside the cell. Depth and radiance always describe that one surface.
//
// HierarchyFootprint: the sample stands for the cell's footprint along the
// line. The chord [la, lb] of the section line through the cell (clipped to
// the viewport) is lifted onto the cell's plane: Qa, Qb are exact surface
// points (w affine in pixels), the 3D front line Qa-Qb lies in the section
// plane. It is clipped to the R-ball (|Q - P| <= R is an interval of the line,
// the M2 front policy applied along the strip); each end gets its thickness
// segment along its camera ray (clipped to the R-sphere as in M2). The strip
// occupies the shortest arc holding the angles of its four corners (front and
// back of both ends); an arc of pi or more means the strip surrounds the
// receiver (degenerate, rejected), as does any of its three edges passing
// within 1e-6 z_P of the receiver. Self intersection: a cell on the receiver's
// own plane (normals within NormalCos, plane through the receiver within
// PlaneTolerance), or whose two front ends both fail the M2 elevation test,
// is the receiver's surface. The one-sided test uses the cell's normal (one
// sign for the whole plane). Otherwise (cache only) depth, normal and self
// test are the texel's (M2 point) and only the radiance is the cell's.
// Cache Mean / SphericalHarmonics: the cell's radiance; otherwise the texel's.
template <typename T>
bool EvaluateCoarseCandidate(const View<T>& g, const SectionT<T>& s, uint32_t x, uint32_t y, const Position<T>& p,
                             const GatherSettings& settings, GatherCounters& counters, CandidateT<T>& out) {
    ++counters.Scheduled;
    const int32_t xx = static_cast<int32_t>(x) + p.CellX;
    const int32_t yy = static_cast<int32_t>(y) + p.CellY;
    const size_t index = g.Index(xx, yy);
    const uint8_t cls = g.Cls[index];
    ++counters.Valid;
    ++counters.Coarse;
    const DepthHierarchy& hierarchy = *g.Acc->Hierarchy;
    const HierarchyCell& cell = hierarchy.Cell(p.Level, static_cast<uint32_t>(p.HX), static_cast<uint32_t>(p.HY));
    const T radius = static_cast<T>(settings.Radius);
    const T sign = T(p.Side);
    const T pi = Pi<T>();
    const T twoPi = T(2) * pi;
    VecT<T> q;          // representative front point (radiance direction)
    VecT<T> corners[4]; // front a, front b, back a, back b (footprint) or front, back (point)
    uint32_t cornerCount = 0;
    VecT<T> nq;
    if (settings.HierarchyFootprint) {
        const T size = static_cast<T>(1U << p.Level);
        const T x0 = static_cast<T>(p.HX) * size, y0 = static_cast<T>(p.HY) * size;
        const T x1 = std::min(x0 + size, static_cast<T>(g.W)), y1 = std::min(y0 + size, static_cast<T>(g.H));
        const T xc = x0 + T(0.5) * size, yc = y0 + T(0.5) * size;
        const T cw = static_cast<T>(cell.W), cgx = static_cast<T>(cell.Gx), cgy = static_cast<T>(cell.Gy);
        nq = {static_cast<T>(cell.Normal.X), static_cast<T>(cell.Normal.Y), static_cast<T>(cell.Normal.Z)};
        if (settings.RejectSelfIntersection) {
            // The receiver's own plane.
            const T wp = cw + cgx * (T(x) + T(0.5) - xc) + cgy * (T(y) + T(0.5) - yc);
            const T w0 = T(1) / s.ZP;
            if (VDot(nq, s.N) >= static_cast<T>(hierarchy.Settings.NormalCos) &&
                std::abs(wp - w0) <= static_cast<T>(hierarchy.Settings.PlaneTolerance) * w0) {
                ++counters.SelfRejected;
                return false;
            }
        }
        // Chord of the line o + lambda d through [x0, x1] x [y0, y1].
        const T ox = T(x) + T(0.5), oy = T(y) + T(0.5);
        const T dx = s.DirX * sign, dy = s.DirY * sign;
        T la = T(0), lb = std::numeric_limits<T>::infinity();
        const auto slab = [&](T o, T d, T b0, T b1) {
            if (std::abs(d) > T(0)) {
                T t0 = (b0 - o) / d, t1 = (b1 - o) / d;
                if (t0 > t1) std::swap(t0, t1);
                la = std::max(la, t0);
                lb = std::min(lb, t1);
            } else if (o < b0 || o > b1) {
                lb = -std::numeric_limits<T>::infinity();
            }
        };
        slab(ox, dx, x0, x1);
        slab(oy, dy, y0, y1);
        if (!(lb >= la)) la = lb = p.Lambda; // rounding at a corner: the sample point alone
        const auto lift = [&](T lambda) {
            const T px = ox + lambda * dx, py = oy + lambda * dy;
            const T w = cw + cgx * (px - xc) + cgy * (py - yc);
            return VScale(g.Ray(px, py), T(1) / w);
        };
        const VecT<T> qa = lift(la), qb = lift(lb);
        // Front line within the R-ball: |qa + t (qb - qa) - P|^2 <= R^2, t in [0, 1].
        const VecT<T> d = VSub(qb, qa), f = VSub(qa, s.P);
        const T a2 = VDot(d, d), b2 = T(2) * VDot(f, d), c2 = VDot(f, f) - radius * radius;
        T t0 = T(0), t1 = T(1);
        if (a2 > T(0)) {
            const T disc = b2 * b2 - T(4) * a2 * c2;
            if (disc < T(0)) {
                ++counters.BeyondRadius;
                return false;
            }
            const T root = std::sqrt(disc);
            t0 = std::max(T(0), (-b2 - root) / (T(2) * a2));
            t1 = std::min(T(1), (-b2 + root) / (T(2) * a2));
        } else if (c2 > T(0)) {
            t1 = T(-1);
        }
        if (!(t1 >= t0)) {
            ++counters.BeyondRadius;
            return false;
        }
        const VecT<T> fa = VAdd(qa, VScale(d, t0)), fb = VAdd(qa, VScale(d, t1));
        if (settings.RejectSelfIntersection) {
            const auto below = [&](const VecT<T>& e) {
                const VecT<T> rel = VSub(e, s.P);
                return VDot(rel, s.N) <= static_cast<T>(settings.SelfIntersectionSine) * VLength(rel) +
                                             static_cast<T>(settings.SelfIntersectionDepth) * s.ZP;
            };
            // M8: the cell's plane normal with the shading normal of the
            // sample's texel (the cell is one plane of one surface).
            const bool m8 = settings.ShadingNormals && g.S != nullptr;
            const bool hidden = m8 ? HiddenM8T(s, fa, nq, g.S[index], settings) &&
                                         HiddenM8T(s, fb, nq, g.S[index], settings)
                                   : below(fa) && below(fb);
            if (hidden) {
                ++counters.SelfRejected;
                return false;
            }
        }
        // Thickness segments of both ends, clipped to the R-sphere (M2).
        const auto backOf = [&](const VecT<T>& e) {
            const VecT<T> rel = VSub(e, s.P);
            const T dist = VLength(rel);
            const VecT<T> dir = VNormalize(e);
            const T along = VDot(rel, dir);
            const T clip = -along + std::sqrt(std::max(along * along - (dist * dist - radius * radius), T(0)));
            const T length = std::max(std::min(static_cast<T>(settings.Thickness), clip), T(0));
            return VAdd(e, VScale(dir, length));
        };
        const VecT<T> ba = backOf(fa), bb = backOf(fb);
        // Degenerate: an edge of the strip meets the receiver.
        const auto nearReceiver = [&](const VecT<T>& u, const VecT<T>& v) {
            const VecT<T> e = VSub(v, u), r = VSub(s.P, u);
            const T ee = VDot(e, e);
            const T t = ee > T(0) ? std::clamp(VDot(r, e) / ee, T(0), T(1)) : T(0);
            return VLength(VSub(r, VScale(e, t))) <= T(1.0e-6) * s.ZP;
        };
        if (nearReceiver(fa, ba) || nearReceiver(fb, bb) || nearReceiver(fa, fb)) {
            ++counters.Degenerate;
            return false;
        }
        corners[0] = fa, corners[1] = fb, corners[2] = ba, corners[3] = bb;
        cornerCount = 4;
        q = lift(p.Lambda);
    } else {
        // Cache only: the M2 point candidate of the sample's texel.
        const T zq = g.Z[index];
        nq = g.N[index];
        if (settings.RejectSelfIntersection) {
            const VecT<T> centre = VScale(g.Ray(T(xx) + T(0.5), T(yy) + T(0.5)), zq);
            if (settings.ShadingNormals && g.S != nullptr) {
                if (HiddenM8T(s, centre, nq, g.S[index], settings)) {
                    ++counters.SelfRejected;
                    return false;
                }
            } else {
                const VecT<T> dd = VSub(centre, s.P);
                const T elevation = VDot(dd, s.N);
                if (elevation <= static_cast<T>(settings.SelfIntersectionSine) * VLength(dd) +
                                     static_cast<T>(settings.SelfIntersectionDepth) * s.ZP) {
                    ++counters.SelfRejected;
                    return false;
                }
            }
        }
        const T px = T(x) + T(0.5) + p.Lambda * s.DirX * sign;
        const T py = T(y) + T(0.5) + p.Lambda * s.DirY * sign;
        q = VScale(g.Ray(px, py), zq);
        const VecT<T> rel = VSub(q, s.P);
        const T dist = VLength(rel);
        if (dist > radius) {
            ++counters.BeyondRadius;
            return false;
        }
        const VecT<T> dir = VNormalize(q);
        const T along = VDot(rel, dir);
        const T clip = -along + std::sqrt(std::max(along * along - (dist * dist - radius * radius), T(0)));
        const T length = std::max(std::min(static_cast<T>(settings.Thickness), clip), T(0));
        const VecT<T> back = VAdd(q, VScale(dir, length));
        const T closestAt = std::clamp(-along, T(0), length);
        if (VLength(VAdd(rel, VScale(dir, closestAt))) <= T(1.0e-6) * s.ZP) {
            ++counters.Degenerate;
            return false;
        }
        corners[0] = q, corners[1] = back;
        cornerCount = 2;
    }
    // Shortest arc holding the corner angles (two corners: the M2 short arc).
    const VecT<T> rel0 = VSub(corners[0], s.P);
    const T theta0 = std::atan2(VDot(rel0, s.Tn), VDot(rel0, s.V));
    T lowDelta = T(0), highDelta = T(0);
    for (uint32_t c = 1; c < cornerCount; ++c) {
        const VecT<T> rel = VSub(corners[c], s.P);
        T delta = std::atan2(VDot(rel, s.Tn), VDot(rel, s.V)) - theta0;
        if (delta > pi) delta -= twoPi;
        else if (delta <= -pi) delta += twoPi;
        lowDelta = std::min(lowDelta, delta);
        highDelta = std::max(highDelta, delta);
    }
    if (highDelta - lowDelta >= pi) {
        ++counters.Degenerate; // the strip surrounds the receiver
        return false;
    }
    const T lo = theta0 + lowDelta;
    const T hi = theta0 + highDelta;
    out.Pieces = 0;
    if (hi > lo) {
        if (lo < -pi) {
            out.Lo[0] = lo + twoPi, out.Hi[0] = pi, out.Lo[1] = -pi, out.Hi[1] = hi;
            out.Pieces = 2;
        } else if (hi > pi) {
            out.Lo[0] = lo, out.Hi[0] = pi, out.Lo[1] = -pi, out.Hi[1] = hi - twoPi;
            out.Pieces = 2;
        } else {
            out.Lo[0] = lo, out.Hi[0] = hi;
            out.Pieces = 1;
        }
        ++counters.Occupying;
    }
    T r, gg, b;
    if (settings.Cache == CacheMode::Mean) {
        r = static_cast<T>(cell.Mean.R), gg = static_cast<T>(cell.Mean.G), b = static_cast<T>(cell.Mean.B);
    } else if (settings.Cache == CacheMode::SphericalHarmonics) {
        const VecT<T> omega = VNormalize(VSub(s.P, q)); // from the source toward the receiver
        const Rgb value = EvaluateShRadianceOrder2<T>(
            hierarchy.CellSh(p.Level, static_cast<uint32_t>(p.HX), static_cast<uint32_t>(p.HY)),
            hierarchy.Settings.ShOrder, omega.X, omega.Y, omega.Z);
        r = static_cast<T>(value.R), gg = static_cast<T>(value.G), b = static_cast<T>(value.B);
    } else {
        const size_t bi = g.BufferIndex(xx, yy);
        r = static_cast<T>(g.In->Radiance[3 * bi]);
        gg = static_cast<T>(g.In->Radiance[3 * bi + 1]);
        b = static_cast<T>(g.In->Radiance[3 * bi + 2]);
    }
    // One-sided source (side test kept separate from the cache).
    if (cls == static_cast<uint8_t>(TexelClass::OneSided)) {
        if (VDot(nq, nq) > T(0)) {
            if (!(VDot(nq, VSub(s.P, q)) > T(0))) {
                r = gg = b = T(0);
                ++counters.BackFacing;
            }
        } else {
            ++counters.MissingSourceNormal;
        }
    }
    out.Lambda = p.Lambda;
    out.Side = p.Side;
    out.CellX = p.CellX;
    out.CellY = p.CellY;
    out.R = r, out.G = gg, out.B = b;
    out.Front = corners[0];
    out.Back = cornerCount == 4U ? corners[3] : corners[1];
    out.Level = p.Level;
    out.HX = p.HX;
    out.HY = p.HY;
    return true;
}

// ---------------------------------------------------------------------------
// Masks.

uint32_t RangeBits(uint32_t start, uint32_t count) {
    // count in [1, 32], start + count <= 32: no shift by the word width.
    return (count >= 32U ? 0xFFFFFFFFU : ((1U << count) - 1U)) << start;
}

uint32_t ValidBits(uint32_t word, uint32_t sectors) {
    const uint32_t lo = word * 32U;
    const uint32_t hi = std::min(sectors, lo + 32U);
    return hi > lo ? RangeBits(0U, hi - lo) : 0U;
}

template <typename T>
std::pair<uint32_t, uint32_t> CentreRangeT(T lo, T hi, uint32_t sectors) {
    if (!(hi > lo) || sectors == 0U) return {0U, 0U};
    const T step = T(2) * Pi<T>() / static_cast<T>(sectors);
    const T x0 = (lo + Pi<T>()) / step - T(0.5);
    const T x1 = (hi + Pi<T>()) / step - T(0.5);
    const auto index = [sectors](T value) -> uint32_t {
        if (!(value > T(0))) return 0U;
        if (value >= static_cast<T>(sectors)) return sectors;
        return static_cast<uint32_t>(value);
    };
    return {index(std::ceil(x0)), index(std::ceil(x1))};
}

void SetRangeWords(uint32_t* words, uint32_t first, uint32_t end) {
    for (uint32_t w = first / 32U; w * 32U < end; ++w) {
        const uint32_t lo = std::max(first, w * 32U);
        const uint32_t hi = std::min(end, w * 32U + 32U);
        if (hi > lo) words[w] |= RangeBits(lo - w * 32U, hi - lo);
    }
}

template <typename Function>
void ForEachRun(const uint32_t* words, uint32_t sectors, Function&& function) {
    for (uint32_t w = 0; w * 32U < sectors; ++w) {
        uint32_t bits = words[w];
        while (bits != 0U) {
            const auto start = static_cast<uint32_t>(std::countr_zero(bits));
            const uint32_t shifted = bits >> start;
            const uint32_t length =
                shifted == 0xFFFFFFFFU ? 32U - start : static_cast<uint32_t>(std::countr_zero(~shifted));
            function(w * 32U + start, w * 32U + start + length);
            bits &= ~RangeBits(start, length);
        }
    }
}

template <typename T>
T BoundaryT(uint32_t j, uint32_t sectors) {
    if (j >= sectors) return Pi<T>();
    return -Pi<T>() + static_cast<T>(j) * (T(2) * Pi<T>() / static_cast<T>(sectors));
}

template <typename T>
T WeightT(T a, T b, T lo, T hi) {
    if constexpr (std::is_same_v<T, double>) return SectorWeightAnalytic(a, b, lo, hi);
    else return SectorWeightAnalyticF32(a, b, lo, hi);
}

template <typename T>
struct SectionSums {
    T R{}, G{}, B{}, FA{};
    uint64_t Occupied = 0;
};

// Centre rule. Each sector is claimed once per section, by the first
// candidate whose interval covers its centre; occupied sectors take that
// candidate's radiance even when it is zero.
template <typename T>
SectionSums<T> AccumulateCentre(const SectionT<T>& s, const CandidateT<T>* candidates, size_t count,
                                uint32_t sectors, uint32_t* maskOut, int32_t* claimant) {
    SectionSums<T> sums;
    uint32_t mask[kMaskWords] = {};
    for (size_t c = 0; c < count; ++c) {
        const CandidateT<T>& candidate = candidates[c];
        uint32_t cover[kMaskWords] = {};
        for (uint32_t piece = 0; piece < candidate.Pieces; ++piece) {
            const auto [first, end] = CentreRangeT<T>(candidate.Lo[piece], candidate.Hi[piece], sectors);
            SetRangeWords(cover, first, end);
        }
        uint32_t fresh[kMaskWords] = {};
        for (uint32_t w = 0; w < kMaskWords; ++w) {
            fresh[w] = cover[w] & ~mask[w];
            mask[w] |= cover[w];
        }
        ForEachRun(fresh, sectors, [&](uint32_t j0, uint32_t j1) {
            const T weight = WeightT<T>(s.A, s.B, BoundaryT<T>(j0, sectors), BoundaryT<T>(j1, sectors));
            sums.R += weight * candidate.R;
            sums.G += weight * candidate.G;
            sums.B += weight * candidate.B;
            if (claimant != nullptr)
                for (uint32_t j = j0; j < j1; ++j) claimant[j] = static_cast<int32_t>(c);
        });
    }
    uint32_t open[kMaskWords] = {};
    for (uint32_t w = 0; w < kMaskWords; ++w) open[w] = ~mask[w] & ValidBits(w, sectors);
    ForEachRun(open, sectors, [&](uint32_t j0, uint32_t j1) {
        sums.FA += WeightT<T>(s.A, s.B, BoundaryT<T>(j0, sectors), BoundaryT<T>(j1, sectors));
    });
    for (uint32_t w = 0; w < kMaskWords; ++w) sums.Occupied += static_cast<uint64_t>(std::popcount(mask[w]));
    if (maskOut != nullptr) std::copy(mask, mask + kMaskWords, maskOut);
    return sums;
}

// Continuous intervals (reference only): the exact union of occupied
// intervals; new pieces take the candidate radiance with exact weights.
SectionSums<double> AccumulateContinuousIntervals(const SectionT<double>& s, const CandidateT<double>* candidates,
                                                  size_t count, std::vector<std::pair<double, double>>& occupied) {
    SectionSums<double> sums;
    occupied.clear();
    for (size_t c = 0; c < count; ++c) {
        const auto& candidate = candidates[c];
        for (uint32_t piece = 0; piece < candidate.Pieces; ++piece) {
            const double lo = candidate.Lo[piece];
            const double hi = candidate.Hi[piece];
            double cursor = lo;
            const auto addFresh = [&](double from, double to) {
                if (!(to > from)) return;
                const double weight = SectorWeightAnalytic(s.A, s.B, from, to);
                sums.R += weight * candidate.R;
                sums.G += weight * candidate.G;
                sums.B += weight * candidate.B;
            };
            for (const auto& [o0, o1] : occupied) {
                if (o1 <= cursor) continue;
                if (o0 >= hi) break;
                addFresh(cursor, std::min(o0, hi));
                cursor = std::max(cursor, o1);
                if (cursor >= hi) break;
            }
            if (cursor < hi) addFresh(cursor, hi);
            // Insert and merge.
            occupied.emplace_back(lo, hi);
            std::sort(occupied.begin(), occupied.end());
            std::vector<std::pair<double, double>> merged;
            for (const auto& interval : occupied) {
                if (!merged.empty() && interval.first <= merged.back().second)
                    merged.back().second = std::max(merged.back().second, interval.second);
                else merged.push_back(interval);
            }
            occupied.swap(merged);
        }
    }
    double cursor = -kPi;
    for (const auto& [o0, o1] : occupied) {
        if (o0 > cursor) sums.FA += SectorWeightAnalytic(s.A, s.B, cursor, o0);
        cursor = std::max(cursor, o1);
    }
    if (cursor < kPi) sums.FA += SectorWeightAnalytic(s.A, s.B, cursor, kPi);
    return sums;
}

// ---------------------------------------------------------------------------
// Pixel and image gather.

template <typename T>
struct Scratch {
    std::vector<Position<T>> Plus, Minus, Merged;
    std::vector<CandidateT<T>> Candidates;
    std::vector<std::pair<double, double>> Occupied;
};

template <typename T>
void BuildCandidates(const View<T>& g, const SectionT<T>& s, uint32_t x, uint32_t y, const GatherSettings& settings,
                     Scratch<T>& scratch, GatherCounters& counters) {
    if (settings.Schedule == CandidateSchedule::MetricRadial || UsesLevels(g, settings)) {
        // M4 radial schedules (metric distribution, hierarchy levels).
        scratch.Plus.clear();
        scratch.Minus.clear();
        EnumerateRadialM4(g, s, x, y, 1, settings, scratch.Plus, counters);
        EnumerateRadialM4(g, s, x, y, -1, settings, scratch.Minus, counters);
    } else {
        EnumerateSide(g, s, 1, settings, scratch.Plus, counters);
        EnumerateSide(g, s, -1, settings, scratch.Minus, counters);
    }
    MergeSides(scratch.Plus, scratch.Minus, settings.InterleaveSides, scratch.Merged);
    scratch.Candidates.clear();
    CandidateT<T> candidate;
    for (const auto& position : scratch.Merged) {
        candidate.Level = position.Level;
        candidate.HX = position.HX;
        candidate.HY = position.HY;
        const bool kept = position.Level > 0U
                              ? EvaluateCoarseCandidate(g, s, x, y, position, settings, counters, candidate)
                              : EvaluateCandidate(g, s, x, y, position, settings, counters, candidate);
        if (kept) scratch.Candidates.push_back(candidate);
    }
}

// M3 rotation sequence: double divides exactly, float multiplies by the
// float reciprocal of M (same operations as the GPU kernel).
template <typename T>
T PixelRotationT(const GatherSettings& settings, uint32_t x, uint32_t y) {
    if (settings.Rotations.Mode != RotationMode::Stratified) return static_cast<T>(settings.Rotation);
    const uint32_t k = StratumIndex(settings.Rotations, x, y);
    if constexpr (std::is_same_v<T, double>)
        return (static_cast<double>(k) + 0.5) / static_cast<double>(StratumCount(settings.Rotations));
    else
        return (static_cast<float>(k) + 0.5F) * StratumScaleF32(settings.Rotations);
}

template <typename T>
T PixelRadialJitterT(const GatherSettings& settings, uint32_t x, uint32_t y) {
    if (settings.Rotations.Mode != RotationMode::Stratified || !settings.SequenceRadialJitter)
        return static_cast<T>(settings.RadialJitter);
    const uint32_t strata = StratumCount(settings.Rotations);
    const uint32_t k = StratumIndex(settings.Rotations, x, y);
    const uint32_t j = static_cast<uint32_t>((static_cast<uint64_t>(k) * LatticeMultiplier(strata)) % strata);
    if constexpr (std::is_same_v<T, double>)
        return (static_cast<double>(j) + 0.5) / static_cast<double>(strata);
    else
        return (static_cast<float>(j) + 0.5F) * StratumScaleF32(settings.Rotations);
}

template <typename T>
PixelGather GatherPixelT(const View<T>& g, uint32_t x, uint32_t y, const GatherSettings& settings,
                         Scratch<T>& scratch, GuideSample* records = nullptr) {
    PixelGather out;
    const size_t index = g.Index(static_cast<int32_t>(x), static_cast<int32_t>(y));
    if (records != nullptr)
        for (uint32_t section = 0; section < settings.Sections; ++section) records[section] = GuideSample{};
    if (!IsGeometry(g.Cls[index]) || !(VDot(g.N[index], g.N[index]) > T(0))) return out;
    T sumR = T(0), sumG = T(0), sumB = T(0), sumA = T(0);
    const T inversePdf = Pi<T>();
    T rotation = PixelRotationT<T>(settings, x, y);
    const NoiseMask* noise = settings.Rotations.Mode == RotationMode::BlueNoise && g.Acc != nullptr ? g.Acc->Noise : nullptr;
    if (noise != nullptr) rotation = NoiseValue<T>(*noise, x, y, settings.Rotations.Frame, false);
    // Per-pixel radial jitter (M3): a local copy keeps the schedule code unchanged.
    GatherSettings jittered;
    const GatherSettings* active = &settings;
    if (settings.Rotations.Mode == RotationMode::Stratified && settings.SequenceRadialJitter) {
        jittered = settings;
        jittered.RadialJitter = static_cast<double>(PixelRadialJitterT<T>(settings, x, y));
        active = &jittered;
    } else if (noise != nullptr) {
        jittered = settings;
        jittered.RadialJitter = static_cast<double>(NoiseValue<T>(*noise, x, y, settings.Rotations.Frame, true));
        active = &jittered;
    }
    // M4 guiding: the region's frozen table; records also of unguided frames
    // (density 1 / pi) when a table and a record buffer are given.
    const GuideTable* table = g.Acc != nullptr ? g.Acc->Guide : nullptr;
    const bool guided = settings.Guiding && table != nullptr;
    const uint32_t region = table != nullptr ? table->Region(x, y) : 0U;
    for (uint32_t section = 0; section < settings.Sections; ++section) {
        T phi;
        T sectionInversePdf = inversePdf;
        T density = T(1) / Pi<T>();
        uint32_t bin = kNoGuideBin;
        if (guided) {
            const T u = (T(section) + rotation) / static_cast<T>(settings.Sections);
            phi = GuideDraw<T>(*table, region, u, density, bin);
            sectionInversePdf = T(1) / density;
        } else {
            phi = Pi<T>() * (T(section) + rotation) / static_cast<T>(settings.Sections);
            if (table != nullptr)
                bin = std::min(static_cast<uint32_t>(phi * static_cast<T>(table->Bins) / Pi<T>()), table->Bins - 1U);
        }
        const SectionT<T> s = SetupT(g, x, y, phi, static_cast<T>(settings.Radius), settings.ShadingNormals);
        if (!s.Valid) {
            if (records != nullptr)
                for (uint32_t k = 0; k < settings.Sections; ++k) records[k] = GuideSample{};
            return PixelGather{};
        }
        BuildCandidates(g, s, x, y, *active, scratch, out.Counters);
        SectionSums<T> sums;
        if constexpr (std::is_same_v<T, double>) {
            if (settings.Mask == MaskMode::ContinuousIntervals)
                sums = AccumulateContinuousIntervals(s, scratch.Candidates.data(), scratch.Candidates.size(),
                                                     scratch.Occupied);
            else
                sums = AccumulateCentre<T>(s, scratch.Candidates.data(), scratch.Candidates.size(), settings.Sectors,
                                           nullptr, nullptr);
        } else {
            sums = AccumulateCentre<T>(s, scratch.Candidates.data(), scratch.Candidates.size(), settings.Sectors,
                                       nullptr, nullptr);
        }
        sumR += sums.R * sectionInversePdf;
        sumG += sums.G * sectionInversePdf;
        sumB += sums.B * sectionInversePdf;
        if (guided && settings.GuidingAccessibility == GuidedAccessibility::IndependentUniform) {
            // A: the unguided section of the same stratum (uniform density).
            const T phiA = Pi<T>() * (T(section) + rotation) / static_cast<T>(settings.Sections);
            const SectionT<T> sa = SetupT(g, x, y, phiA, static_cast<T>(settings.Radius), settings.ShadingNormals);
            if (!sa.Valid) {
                if (records != nullptr)
                    for (uint32_t k = 0; k < settings.Sections; ++k) records[k] = GuideSample{};
                return PixelGather{};
            }
            BuildCandidates(g, sa, x, y, *active, scratch, out.Counters);
            SectionSums<T> sumsA;
            if constexpr (std::is_same_v<T, double>) {
                if (settings.Mask == MaskMode::ContinuousIntervals)
                    sumsA = AccumulateContinuousIntervals(sa, scratch.Candidates.data(), scratch.Candidates.size(),
                                                          scratch.Occupied);
                else
                    sumsA = AccumulateCentre<T>(sa, scratch.Candidates.data(), scratch.Candidates.size(),
                                                settings.Sectors, nullptr, nullptr);
            } else {
                sumsA = AccumulateCentre<T>(sa, scratch.Candidates.data(), scratch.Candidates.size(), settings.Sectors,
                                            nullptr, nullptr);
            }
            sumA += sumsA.FA * inversePdf;
        } else {
            sumA += sums.FA * sectionInversePdf;
        }
        out.OccupiedSectors += sums.Occupied;
        if (records != nullptr && table != nullptr) {
            GuideSample& record = records[section];
            record.Contribution = static_cast<double>(T(0.2126) * sums.R + T(0.7152) * sums.G + T(0.0722) * sums.B);
            record.Pdf = static_cast<double>(density);
            record.Phi = static_cast<double>(phi);
            record.Bin = bin;
        }
    }
    const T inverse = T(1) / static_cast<T>(settings.Sections);
    out.Valid = true;
    out.J = {static_cast<double>(sumR * inverse), static_cast<double>(sumG * inverse),
             static_cast<double>(sumB * inverse)};
    out.ARaw = static_cast<double>(sumA * inverse);
    out.A = std::clamp(out.ARaw, 0.0, 1.0);
    out.Confidence = out.Counters.Scheduled == 0U ? 0.0
                                                  : static_cast<double>(out.Counters.Valid) /
                                                        static_cast<double>(out.Counters.Scheduled);
    return out;
}

template <typename T>
std::vector<PixelGather> GatherImageT(const View<T>& g, const GatherSettings& settings) {
    std::vector<PixelGather> results(static_cast<size_t>(g.W) * g.H);
    const uint32_t threads = std::clamp(settings.Threads, 1U, 16U);
    std::vector<GuideSample>* records = g.Acc != nullptr ? g.Acc->Samples : nullptr;
    if (records != nullptr) records->assign(results.size() * settings.Sections, GuideSample{});
    const auto worker = [&](uint32_t first) {
        Scratch<T> scratch;
        for (uint32_t y = first; y < g.H; y += threads)
            for (uint32_t x = 0; x < g.W; ++x) {
                const size_t i = static_cast<size_t>(y) * g.W + x;
                results[i] = GatherPixelT(g, x, y, settings, scratch,
                                          records != nullptr ? &(*records)[i * settings.Sections] : nullptr);
            }
    };
    if (threads == 1U) {
        worker(0U);
    } else {
        std::vector<std::thread> pool;
        for (uint32_t t = 0; t < threads; ++t) pool.emplace_back(worker, t);
        for (auto& thread : pool) thread.join();
    }
    return results;
}

View<double> ViewOf(const GatherInput& input, const PreparedGeometry& geometry) {
    View<double> view = MakeView<double>(input);
    view.Z = geometry.ViewDepth.data();
    view.Cls = geometry.Class.data();
    view.N = geometry.Normal.data();
    if (!geometry.Shading.empty()) view.S = geometry.Shading.data();
    return view;
}

template <typename V>
Vec3 Widen(const V& v) {
    return {static_cast<double>(v.X), static_cast<double>(v.Y), static_cast<double>(v.Z)};
}

template <typename T>
SectionSetup ToPublic(const SectionT<T>& s) {
    SectionSetup out;
    out.Valid = s.Valid;
    out.P = Widen(s.P);
    out.V = Widen(s.V);
    out.T = Widen(s.Tn);
    out.Normal = Widen(s.NS);
    out.Face = Widen(s.N);
    out.NV = s.A;
    out.NT = s.B;
    out.DirX = s.DirX;
    out.DirY = s.DirY;
    out.Extent[0] = s.Extent[0];
    out.Extent[1] = s.Extent[1];
    return out;
}

SectionT<double> FromPublic(const SectionSetup& s) {
    SectionT<double> out;
    out.Valid = s.Valid;
    out.P = s.P;
    out.V = s.V;
    out.Tn = s.T;
    out.N = s.Face.X != 0.0 || s.Face.Y != 0.0 || s.Face.Z != 0.0 ? s.Face : s.Normal;
    out.NS = s.Normal;
    out.A = s.NV;
    out.B = s.NT;
    out.ZP = -s.P.Z;
    out.DirX = s.DirX;
    out.DirY = s.DirY;
    out.Extent[0] = s.Extent[0];
    out.Extent[1] = s.Extent[1];
    return out;
}

template <typename T>
Candidate ToPublic(const CandidateT<T>& c) {
    Candidate out;
    out.Lambda = c.Lambda;
    out.Side = c.Side;
    out.CellX = c.CellX;
    out.CellY = c.CellY;
    out.Pieces = c.Pieces;
    out.Lo[0] = c.Lo[0], out.Lo[1] = c.Lo[1], out.Hi[0] = c.Hi[0], out.Hi[1] = c.Hi[1];
    out.Radiance = {c.R, c.G, c.B};
    out.Front = Widen(c.Front);
    out.Back = Widen(c.Back);
    out.Level = c.Level;
    out.HX = c.HX;
    out.HY = c.HY;
    return out;
}

CandidateT<double> FromPublic(const Candidate& c) {
    CandidateT<double> out;
    out.Lambda = c.Lambda;
    out.Side = c.Side;
    out.CellX = c.CellX;
    out.CellY = c.CellY;
    out.Pieces = std::min(c.Pieces, 2U);
    out.Lo[0] = c.Lo[0], out.Lo[1] = c.Lo[1], out.Hi[0] = c.Hi[0], out.Hi[1] = c.Hi[1];
    out.R = c.Radiance.R, out.G = c.Radiance.G, out.B = c.Radiance.B;
    out.Front = c.Front;
    out.Back = c.Back;
    return out;
}

} // namespace

GatherCounters& GatherCounters::operator+=(const GatherCounters& o) noexcept {
    Scheduled += o.Scheduled;
    Valid += o.Valid;
    Sky += o.Sky;
    SelfRejected += o.SelfRejected;
    BeyondRadius += o.BeyondRadius;
    Degenerate += o.Degenerate;
    BackFacing += o.BackFacing;
    MissingSourceNormal += o.MissingSourceNormal;
    Duplicates += o.Duplicates;
    Occupying += o.Occupying;
    Coarse += o.Coarse;
    Descended += o.Descended;
    Contact += o.Contact;
    return *this;
}

const char* ValidateGatherInput(const GatherInput& in) noexcept {
    const size_t texels = static_cast<size_t>(in.BufferWidth) * in.BufferHeight;
    if (texels == 0U) return "empty buffers";
    if (in.Viewport.Width == 0U || in.Viewport.Height == 0U) return "empty viewport";
    if (static_cast<uint64_t>(in.Viewport.X) + in.Viewport.Width > in.BufferWidth ||
        static_cast<uint64_t>(in.Viewport.Y) + in.Viewport.Height > in.BufferHeight)
        return "viewport outside the buffers";
    if (in.Depth.size() != texels || in.Class.size() != texels || in.Radiance.size() != 3U * texels)
        return "buffer sizes do not match the declared extent";
    if (in.NormalSource == GeometricNormalSource::RendererFaceNormal && in.FaceNormals.size() != 3U * texels)
        return "renderer face normals declared but not provided";
    if (!in.ShadingNormals.empty() && in.ShadingNormals.size() != 3U * texels)
        return "shading normals do not match the declared extent";
    return ValidateDepthCamera(in.Camera);
}

const char* ValidateDepthCamera(const DepthCamera& c) noexcept {
    if (c.Encoding == DepthEncoding::Unknown) return "depth encoding is undeclared";
    if (!(c.Near > 0.0) || !std::isfinite(c.Near)) return "near plane must be finite and positive";
    if (std::isinf(c.Far)) {
        if (c.Encoding != DepthEncoding::ReversedPerspectiveZeroToOne || c.Far < 0.0)
            return "an infinite far plane needs reversed perspective depth";
    } else if (!(c.Far > c.Near)) {
        return "far plane must lie beyond the near plane";
    }
    const double det = c.ViewX[0] * c.ViewY[1] - c.ViewX[1] * c.ViewY[0];
    if (!(std::abs(det) > 0.0) || !std::isfinite(det)) return "UV to ray map is singular";
    return "";
}

const char* ValidateGatherSettings(const GatherSettings& s) noexcept {
    if (!(s.Radius > 0.0) || !std::isfinite(s.Radius)) return "radius R must be finite and positive";
    if (!(s.Thickness >= 0.0) || !std::isfinite(s.Thickness)) return "thickness must be finite and non-negative";
    if (s.Sections == 0U) return "at least one section";
    if (s.Sectors == 0U || s.Sectors > kMaxSectors) return "sector count outside [1, 256]";
    if (!(s.Rotation >= 0.0 && s.Rotation < 1.0)) return "rotation outside [0, 1)";
    if (s.Schedule == CandidateSchedule::Radial || s.Schedule == CandidateSchedule::MetricRadial) {
        if (s.CandidatesPerSide == 0U) return "radial schedule needs candidates";
        if (!(s.RadialExponent >= 1.0) || !std::isfinite(s.RadialExponent)) return "radial exponent must be >= 1";
        if (!(s.RadialJitter >= 0.0 && s.RadialJitter < 1.0)) return "radial jitter outside [0, 1)";
    }
    // M4 switches.
    const bool levels = s.HierarchyFootprint || s.Cache != CacheMode::Off;
    if (levels && s.Schedule == CandidateSchedule::EveryTexel)
        return "hierarchy and cache act on radial schedules (EveryTexel is the reference)";
    if ((levels || s.Schedule == CandidateSchedule::MetricRadial) && s.CandidatesPerSide > 256U)
        return "M4 radial schedules take at most 256 candidates per side";
    if (levels && (s.HierarchyMaxLevel == 0U || s.HierarchyMaxLevel >= kMaxHierarchyLevels))
        return "hierarchy footprint level outside [1, 7]";
    if (levels && !(s.HierarchySpacingScale >= 0.5 && s.HierarchySpacingScale <= 4.0))
        return "hierarchy spacing scale outside [0.5, 4]";
    if (!(s.SelfIntersectionSine >= 0.0) || !(s.SelfIntersectionDepth >= 0.0)) return "negative self tolerance";
    if (!(s.ChordEpsilon >= 0.0 && s.ChordEpsilon < 0.5)) return "chord epsilon outside [0, 0.5)";
    if (!(s.OwnSurfaceCos >= -1.0 && s.OwnSurfaceCos <= 1.0)) return "own-surface cosine outside [-1, 1]";
    if (!(s.HardEdgeRatio >= 0.0 && s.HardEdgeRatio <= 1.0)) return "hard-edge ratio outside [0, 1]";
    if (s.Rotations.Mode == RotationMode::Stratified) {
        if (s.Rotations.TileLog2 > kMaxRotationTileLog2) return "rotation tile larger than 8 x 8";
        if (s.Rotations.Cycle == 0U || s.Rotations.Cycle > kMaxRotationCycle) return "rotation cycle outside [1, 64]";
    }
    return "";
}

uint32_t TileOrder(uint32_t x, uint32_t y, uint32_t tileLog2) noexcept {
    // Bit pair (x_k, y_k) -> 0, 2, 3, 1; pair k weighted by 4^(L - 1 - k).
    uint32_t index = 0U;
    for (uint32_t k = 0; k < tileLog2; ++k) {
        const uint32_t bx = (x >> k) & 1U, by = (y >> k) & 1U;
        const uint32_t code = bx == 0U ? (by == 0U ? 0U : 3U) : (by == 0U ? 2U : 1U);
        index |= code << (2U * (tileLog2 - 1U - k));
    }
    return index;
}

uint32_t StratumCount(const RotationSequence& sequence) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    return (1U << (2U * log2)) * std::clamp(sequence.Cycle, 1U, kMaxRotationCycle);
}

uint32_t FrameShift(const RotationSequence& sequence) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    if (log2 == 0U) return 0U;
    const uint32_t tiles = 1U << (2U * log2);
    // Pair i of the T^2-frame shift period; even shifts 2 bitrev(i) over
    // log2(T^2 / 2) bits, so that with the XOR flip every stratum visits every
    // tile position once per period.
    const auto pair = static_cast<uint32_t>(sequence.Frame % tiles) >> 1U;
    const uint32_t bits = 2U * log2 - 1U;
    uint32_t reversed = 0U;
    for (uint32_t b = 0; b < bits; ++b) reversed |= ((pair >> b) & 1U) << (bits - 1U - b);
    return 2U * reversed;
}

uint32_t FrameFlip(const RotationSequence& sequence) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    return log2 == 0U ? 0U : static_cast<uint32_t>(sequence.Frame & 1U);
}

uint32_t StratumIndex(const RotationSequence& sequence, uint32_t x, uint32_t y) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    const uint32_t tiles = 1U << (2U * log2);
    const uint32_t cycle = std::clamp(sequence.Cycle, 1U, kMaxRotationCycle);
    const auto phase = static_cast<uint32_t>(sequence.Frame % cycle);
    const uint32_t coarse = ((TileOrder(x, y, log2) ^ FrameFlip(sequence)) + FrameShift(sequence)) % tiles;
    return cycle * coarse + phase;
}

uint32_t LatticeMultiplier(uint32_t strata) noexcept {
    if (strata <= 1U) return 1U;
    const double target = static_cast<double>(strata) * 0.6180339887498949;
    const auto base = static_cast<int64_t>(std::floor(target));
    // Nearest coprime integer in [1, strata - 1]; ties go to the smaller.
    for (int64_t distance = 0; distance < static_cast<int64_t>(strata); ++distance) {
        int64_t best = -1;
        double bestGap = 0.0;
        for (const int64_t candidate : {base - distance, base + 1 - distance, base + distance, base + 1 + distance}) {
            if (candidate < 1 || candidate >= static_cast<int64_t>(strata)) continue;
            if (std::gcd(static_cast<uint64_t>(candidate), static_cast<uint64_t>(strata)) != 1U) continue;
            const double gap = std::abs(static_cast<double>(candidate) - target);
            if (best < 0 || gap < bestGap || (gap == bestGap && candidate < best)) {
                best = candidate;
                bestGap = gap;
            }
        }
        if (best >= 0) return static_cast<uint32_t>(best);
    }
    return 1U;
}

float StratumScaleF32(const RotationSequence& sequence) noexcept {
    return 1.0F / static_cast<float>(StratumCount(sequence));
}

double PixelRotation(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRotationT<double>(settings, x, y);
}

float PixelRotationF32(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRotationT<float>(settings, x, y);
}

double PixelRadialJitter(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRadialJitterT<double>(settings, x, y);
}

float PixelRadialJitterF32(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRadialJitterT<float>(settings, x, y);
}

double DecodeViewDepth(double encoded, const DepthCamera& camera) noexcept {
    const bool infiniteFar = std::isinf(camera.Far);
    return DecodeDepthT<double>(encoded, camera.Encoding, camera.Near, infiniteFar ? 0.0 : camera.Far, infiniteFar);
}

Vec3 PreparedGeometry::Ray(const DepthCamera& camera, double px, double py) const noexcept {
    const double u = px / Width;
    const double v = py / Height;
    return {camera.ViewX[0] * u + camera.ViewX[1] * v + camera.ViewX[2],
            camera.ViewY[0] * u + camera.ViewY[1] * v + camera.ViewY[2], -1.0};
}

Vec3 PreparedGeometry::Position(const DepthCamera& camera, uint32_t x, uint32_t y) const noexcept {
    return Scale(Ray(camera, x + 0.5, y + 0.5), ViewDepth[static_cast<size_t>(y) * Width + x]);
}

PreparedGeometry PrepareGeometry(const GatherInput& input) {
    View<double> view;
    auto data = PrepareT<double>(input, view);
    PreparedGeometry out;
    out.Width = input.Viewport.Width;
    out.Height = input.Viewport.Height;
    out.ViewDepth = std::move(data.Z);
    out.Class = std::move(data.Cls);
    out.Normal = std::move(data.N);
    out.Shading = std::move(data.S);
    return out;
}

PreparedGeometry PrepareGeometryF32(const GatherInput& input) {
    View<float> view;
    const auto data = PrepareT<float>(input, view);
    PreparedGeometry out;
    out.Width = input.Viewport.Width;
    out.Height = input.Viewport.Height;
    out.ViewDepth.assign(data.Z.begin(), data.Z.end());
    out.Class = data.Cls;
    out.Normal.reserve(data.N.size());
    for (const auto& n : data.N) out.Normal.push_back(Widen(n));
    out.Shading.reserve(data.S.size());
    for (const auto& n : data.S) out.Shading.push_back(Widen(n));
    return out;
}

SectionCandidates BuildSectionF32(const GatherInput& input, uint32_t x, uint32_t y, float phi,
                                  const GatherSettings& settings) {
    SectionCandidates out;
    View<float> view;
    const auto data = PrepareT<float>(input, view);
    const SectionT<float> s = SetupT(view, x, y, phi, static_cast<float>(settings.Radius), settings.ShadingNormals);
    out.Setup = ToPublic(s);
    if (!s.Valid) return out;
    Scratch<float> scratch;
    BuildCandidates(view, s, x, y, settings, scratch, out.Counters);
    for (const auto& c : scratch.Candidates) out.Ordered.push_back(ToPublic(c));
    return out;
}

SectionSetup SetupSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                          double phi, const GatherSettings& settings) {
    return ToPublic(SetupT(ViewOf(input, geometry), x, y, phi, settings.Radius, settings.ShadingNormals));
}

SectionCandidates BuildSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                               double phi, const GatherSettings& settings) {
    SectionCandidates out;
    const View<double> view = ViewOf(input, geometry);
    const SectionT<double> s = SetupT(view, x, y, phi, settings.Radius, settings.ShadingNormals);
    out.Setup = ToPublic(s);
    if (!s.Valid) return out;
    Scratch<double> scratch;
    BuildCandidates(view, s, x, y, settings, scratch, out.Counters);
    out.Ordered.reserve(scratch.Candidates.size());
    for (const auto& c : scratch.Candidates) out.Ordered.push_back(ToPublic(c));
    return out;
}

std::pair<uint32_t, uint32_t> CentreRuleRange(double lo, double hi, uint32_t sectors) noexcept {
    return CentreRangeT<double>(lo, hi, std::min(sectors, kMaxSectors));
}

std::pair<uint32_t, uint32_t> CentreRuleRangeF32(float lo, float hi, uint32_t sectors) noexcept {
    return CentreRangeT<float>(lo, hi, std::min(sectors, kMaxSectors));
}

void SetSectorRange(SectorMask& mask, uint32_t first, uint32_t end) noexcept {
    SetRangeWords(mask.Words.data(), std::min(first, kMaxSectors), std::min(end, kMaxSectors));
}

SectorMask CoverInterval(double lo, double hi, uint32_t sectors) noexcept {
    SectorMask mask;
    if (!(hi > lo) || !(hi - lo < 2.0 * kPi)) {
        if (hi - lo >= 2.0 * kPi) SetSectorRange(mask, 0U, sectors);
        return mask;
    }
    const double turns = std::floor((lo + kPi) / (2.0 * kPi));
    lo -= 2.0 * kPi * turns;
    hi -= 2.0 * kPi * turns;
    const auto set = [&](double a, double b) {
        const auto [first, end] = CentreRuleRange(a, b, sectors);
        SetSectorRange(mask, first, end);
    };
    if (hi <= kPi) {
        set(lo, hi);
    } else {
        set(lo, kPi);
        set(-kPi, hi - 2.0 * kPi);
    }
    return mask;
}

uint32_t CountSectors(const SectorMask& mask) noexcept {
    uint32_t count = 0;
    for (const uint32_t word : mask.Words) count += static_cast<uint32_t>(std::popcount(word));
    return count;
}

double SectorBoundary(uint32_t j, uint32_t sectors) noexcept { return BoundaryT<double>(j, sectors); }

double MaskWeight(const SectorMask& mask, uint32_t sectors, double nv, double nt) noexcept {
    double total = 0.0;
    SectorMask clipped = mask;
    for (uint32_t w = 0; w < kMaskWords; ++w) clipped.Words[w] &= ValidBits(w, sectors);
    ForEachRun(clipped.Words.data(), sectors, [&](uint32_t j0, uint32_t j1) {
        total += SectorWeightAnalytic(nv, nt, SectorBoundary(j0, sectors), SectorBoundary(j1, sectors));
    });
    return total;
}

SectionResult AccumulateSection(const SectionSetup& setup, const std::vector<Candidate>& ordered, uint32_t sectors,
                                MaskMode mode) {
    SectionResult result;
    const SectionT<double> s = FromPublic(setup);
    std::vector<CandidateT<double>> candidates;
    candidates.reserve(ordered.size());
    for (const auto& c : ordered) candidates.push_back(FromPublic(c));
    SectionSums<double> sums;
    if (mode == MaskMode::ContinuousIntervals) {
        sums = AccumulateContinuousIntervals(s, candidates.data(), candidates.size(), result.Occupied);
    } else {
        sectors = std::clamp(sectors, 1U, kMaxSectors);
        result.Claimant.assign(sectors, -1);
        sums = AccumulateCentre<double>(s, candidates.data(), candidates.size(), sectors, result.Mask.Words.data(),
                                        result.Claimant.data());
    }
    result.F = {sums.R, sums.G, sums.B};
    result.FA = sums.FA;
    return result;
}

PixelGather GatherPixel(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                        const GatherSettings& settings) {
    Scratch<double> scratch;
    return GatherPixelT(ViewOf(input, geometry), x, y, settings, scratch);
}

std::vector<PixelGather> GatherImage(const GatherInput& input, const GatherSettings& settings) {
    const PreparedGeometry geometry = PrepareGeometry(input);
    return GatherImageT(ViewOf(input, geometry), settings);
}

std::vector<PixelGather> GatherImageF32(const GatherInput& input, const GatherSettings& settings) {
    View<float> view;
    const auto data = PrepareT<float>(input, view);
    return GatherImageT(view, settings);
}

// ---------------------------------------------------------------------------
// M4 entry points.

const char* ValidateGatherAcceleration(const GatherInput& input, const GatherSettings& settings,
                                       const GatherAcceleration& acceleration) noexcept {
    const uint32_t width = input.Viewport.Width, height = input.Viewport.Height;
    if (settings.HierarchyFootprint || settings.Cache != CacheMode::Off) {
        const DepthHierarchy* h = acceleration.Hierarchy;
        if (h == nullptr) return "hierarchy or cache switched on without a hierarchy";
        if (h->Width != width || h->Height != height) return "hierarchy built for another viewport";
        if (h->Levels.size() < 2U || settings.HierarchyMaxLevel + 1U > h->Levels.size())
            return "footprint level beyond the hierarchy";
        if (settings.Cache == CacheMode::SphericalHarmonics &&
            (h->ShCoefficients == 0U || h->Settings.ShOrder > kMaxShOrder || h->Sh.size() != h->Cells.size() * 4U * h->ShCoefficients))
            return "SH cache switched on without SH data of order <= 2";
    }
    if (settings.Guiding || acceleration.Samples != nullptr) {
        const GuideTable* t = acceleration.Guide;
        if (t == nullptr) return "guiding (or its records) switched on without a guide table";
        if (t->Width != width || t->Height != height) return "guide table built for another viewport";
        if (t->Bins == 0U || t->Bins > kMaxGuideBins) return "guide bins outside [1, 32]";
        const size_t regions = static_cast<size_t>(t->RegionsX) * t->RegionsY;
        if (t->RegionsX != ((width + (1U << t->RegionLog2) - 1U) >> t->RegionLog2) ||
            t->RegionsY != ((height + (1U << t->RegionLog2) - 1U) >> t->RegionLog2))
            return "guide regions do not cover the viewport";
        if (t->Cdf.size() != regions * (t->Bins + 1U) || t->Pdf.size() != regions * t->Bins)
            return "guide table sizes do not match its regions";
        for (const double p : t->Pdf)
            if (!(p > 0.0) || !std::isfinite(p)) return "guide densities must be finite and positive";
    }
    if (settings.Rotations.Mode == RotationMode::BlueNoise) {
        const NoiseMask* n = acceleration.Noise;
        if (n == nullptr || n->Count() == 0U || n->Rank.size() != n->Count()) return "blue-noise rotation without a mask";
    }
    return "";
}

std::vector<PixelGather> GatherImage(const GatherInput& input, const GatherSettings& settings,
                                     const GatherAcceleration& acceleration) {
    const PreparedGeometry geometry = PrepareGeometry(input);
    View<double> view = ViewOf(input, geometry);
    view.Acc = &acceleration;
    return GatherImageT(view, settings);
}

std::vector<PixelGather> GatherImageF32(const GatherInput& input, const GatherSettings& settings,
                                        const GatherAcceleration& acceleration) {
    View<float> view;
    const auto data = PrepareT<float>(input, view);
    view.Acc = &acceleration;
    return GatherImageT(view, settings);
}

SectionCandidates BuildSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                               double phi, const GatherSettings& settings, const GatherAcceleration& acceleration) {
    SectionCandidates out;
    View<double> view = ViewOf(input, geometry);
    view.Acc = &acceleration;
    const SectionT<double> s = SetupT(view, x, y, phi, settings.Radius, settings.ShadingNormals);
    out.Setup = ToPublic(s);
    if (!s.Valid) return out;
    Scratch<double> scratch;
    BuildCandidates(view, s, x, y, settings, scratch, out.Counters);
    out.Ordered.reserve(scratch.Candidates.size());
    for (const auto& c : scratch.Candidates) out.Ordered.push_back(ToPublic(c));
    return out;
}

std::vector<ScheduledPosition> ScheduleSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                               uint32_t y, double phi, const GatherSettings& settings,
                                               const GatherAcceleration& acceleration) {
    std::vector<ScheduledPosition> out;
    View<double> view = ViewOf(input, geometry);
    view.Acc = &acceleration;
    const SectionT<double> s = SetupT(view, x, y, phi, settings.Radius, settings.ShadingNormals);
    if (!s.Valid) return out;
    Scratch<double> scratch;
    GatherCounters counters;
    if (settings.Schedule == CandidateSchedule::MetricRadial || UsesLevels(view, settings)) {
        EnumerateRadialM4(view, s, x, y, 1, settings, scratch.Plus, counters);
        EnumerateRadialM4(view, s, x, y, -1, settings, scratch.Minus, counters);
    } else {
        EnumerateSide(view, s, 1, settings, scratch.Plus, counters);
        EnumerateSide(view, s, -1, settings, scratch.Minus, counters);
    }
    MergeSides(scratch.Plus, scratch.Minus, settings.InterleaveSides, scratch.Merged);
    for (const auto& p : scratch.Merged)
        out.push_back({p.Lambda, p.Side, p.CellX, p.CellY, p.Level, p.Requested, p.Spacing});
    return out;
}

PixelGather GatherPixel(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                        const GatherSettings& settings, const GatherAcceleration& acceleration) {
    Scratch<double> scratch;
    View<double> view = ViewOf(input, geometry);
    view.Acc = &acceleration;
    GuideSample* records = nullptr;
    if (acceleration.Samples != nullptr) {
        acceleration.Samples->assign(settings.Sections, GuideSample{});
        records = acceleration.Samples->data();
    }
    return GatherPixelT(view, x, y, settings, scratch, records);
}

} // namespace Fast::Renderer::DiffuseGi
