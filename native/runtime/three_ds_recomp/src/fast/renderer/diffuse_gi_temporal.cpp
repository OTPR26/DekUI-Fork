#include "fast/renderer/diffuse_gi_temporal.h"

#include "fast/renderer/diffuse_gi_normal_code.h"

// Stratum reconstruction, temporal accumulation and a-trous filter of the
// diffuse screen-space GI (milestone M3). Own code from the specification
// (sections 7-9, 11) and the public papers cited in the document ([B3]
// sections 4.1-4.4, [B6] section 2.2); no implementation of any temporal
// filter or denoiser was consulted. The template core runs in double (reference)
// and in float (twin of diffuse_gi_temporal_shader.cpp, same operation order,
// constants read from the same std140 blocks).

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <thread>
#include <type_traits>

namespace Fast::Renderer::DiffuseGi {
namespace {

template <typename T>
struct V3 {
    T X{}, Y{}, Z{};
};
template <typename T>
V3<T> Sub(const V3<T>& a, const V3<T>& b) {
    return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
}
template <typename T>
V3<T> Mul(const V3<T>& a, T s) {
    return {a.X * s, a.Y * s, a.Z * s};
}
template <typename T>
T Dot3(const V3<T>& a, const V3<T>& b) {
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}
template <typename T>
T Length3(const V3<T>& a) {
    return std::sqrt(Dot3(a, a));
}

template <typename T>
T Luma(T r, T g, T b) {
    return static_cast<T>(kLumaR) * r + static_cast<T>(kLumaG) * g + static_cast<T>(kLumaB) * b;
}

template <typename T>
bool Finite(T v) {
    return std::isfinite(v);
}

template <typename T>
bool IsReceiver(T z, T nx, T ny, T nz) {
    return z > T(0) && Finite(z) && Finite(nx) && Finite(ny) && Finite(nz) && nx * nx + ny * ny + nz * nz > T(0);
}

// Affine UV -> ray map of one frame (diffuse_gi_gather.h).
template <typename T>
struct RayMap {
    T Ax{}, Bx{}, Cx{}, Ay{}, By{}, Cy{};
    [[nodiscard]] V3<T> Ray(T px, T py, uint32_t w, uint32_t h) const {
        const T u = px / static_cast<T>(w);
        const T v = py / static_cast<T>(h);
        return {Ax * u + Bx * v + Cx, Ay * u + By * v + Cy, T(-1)};
    }
};

// Window of the stratum reconstruction: radius T/2 with half weights at the
// ends, so that every residue modulo T (every stratum of the tile) has the
// same total weight in a flat region and the window is symmetric (no
// half-pixel shift). Tile 1: a 3 x 3 window used for the spatial variance only.
template <typename T>
T WindowWeight(int32_t d, uint32_t tile) {
    const int32_t a = d < 0 ? -d : d;
    if (tile >= 2U) {
        const auto half = static_cast<int32_t>(tile / 2U);
        return a < half ? T(1) : (a == half ? T(0.5) : T(0));
    }
    return a <= 1 ? T(1) : T(0);
}

// Hard surface compatibility of q with the receiver p (plane distance relative
// to the separation, depth floor, normals).
template <typename T>
bool Compatible(const V3<T>& p, const V3<T>& n, T zp, const V3<T>& q, const V3<T>& nq, T sine, T depth, T cosn) {
    const V3<T> d = Sub(q, p);
    const T plane = std::abs(Dot3(n, d));
    return plane <= sine * Length3(d) + depth * zp && Dot3(n, nq) >= cosn;
}

// M8: shading normals that agree and turn less than the face normals.
template <typename T>
bool SmoothNormalsT(const V3<T>& na, const V3<T>& sa, const V3<T>& nb, const V3<T>& sb, T cosn, T ratio) {
    if (!(Dot3(sa, sa) > T(0)) || !(Dot3(sb, sb) > T(0))) return false;
    const T cs = Dot3(sa, sb);
    const T cf = Dot3(na, nb);
    return cs >= cosn && T(1) - cs < ratio * (T(1) - cf);
}

// M8: a smooth crease between two receiver samples (TemporalSettings).
template <typename T>
bool SmoothCreaseT(const V3<T>& pa, const V3<T>& na, const V3<T>& sa, T zp, const V3<T>& qb, const V3<T>& nb,
                   const V3<T>& sb, T sine, T depth, T cosn, T ratio) {
    if (!SmoothNormalsT(na, sa, nb, sb, cosn, ratio)) return false;
    const T cf = Dot3(na, nb);
    const V3<T> d = Sub(qb, pa);
    const T sf = std::sqrt(std::max(T(1) - cf * cf, T(0)));
    const T tolerance = (sine + sf) * Length3(d) + depth * zp;
    return std::abs(Dot3(na, d)) <= tolerance && std::abs(Dot3(nb, d)) <= tolerance;
}

// M8: the shading normal's code in history value 11 (float bits on the GPU
// and in the FP32 twin, the integer in the double reference).
template <typename T>
T PackShadingCode(const V3<T>& s) {
    const uint32_t code =
        EncodeDiffuseGiNormal({static_cast<float>(s.X), static_cast<float>(s.Y), static_cast<float>(s.Z)});
    if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(code);
    else return static_cast<T>(code);
}
template <typename T>
V3<T> UnpackShadingCode(T value) {
    uint32_t code = 0U;
    if constexpr (std::is_same_v<T, float>) code = std::bit_cast<uint32_t>(value);
    else code = value >= T(0) && value <= T(4294967295.0) ? static_cast<uint32_t>(value) : 0U;
    const auto n = DecodeDiffuseGiNormal(code);
    return {static_cast<T>(n[0]), static_cast<T>(n[1]), static_cast<T>(n[2])};
}

template <typename T>
struct Constants {
    uint32_t W = 0, H = 0, Radius = 1, Tile = 1;
    bool Reset = true, Reconstruct = false, Flow = true, Ids = false;
    RayMap<T> Current, Previous;
    T R[9]{};
    T Tr[3]{};
    T MotionX{}, MotionY{}, JitterX{}, JitterY{};
    T AlphaMin{}, VarianceFrames{}, ExposureRatio{}, LengthCap{};
    T ZLow{}, ZHigh{}, FloorJ{}, FloorA{};
    T PlaneSine{}, PlaneDepth{}, NormalCos{};
    T MovingFlow{}, MovingDepth{}, MovingConfidence{};
    bool Shading = false; // M8
    T Ratio{};
};

template <typename T>
Constants<T> FromParams(const TemporalShaderParams& p) {
    Constants<T> c;
    c.W = p.Extent[0];
    c.H = p.Extent[1];
    c.Reset = (p.Extent[2] & kTemporalFlagReset) != 0U;
    c.Radius = p.Extent[3];
    c.Tile = p.Modes[0];
    c.Reconstruct = p.Modes[1] != 0U;
    c.Flow = p.Modes[2] != 0U;
    c.Ids = p.Modes[3] != 0U;
    c.Current = {p.ViewX[0], p.ViewX[1], p.ViewX[2], p.ViewY[0], p.ViewY[1], p.ViewY[2]};
    c.Previous = {p.PreviousX[0], p.PreviousX[1], p.PreviousX[2], p.PreviousY[0], p.PreviousY[1], p.PreviousY[2]};
    const std::array<float, 4>* rows[3] = {&p.Rotation0, &p.Rotation1, &p.Rotation2};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) c.R[3 * r + k] = (*rows[r])[k];
        c.Tr[r] = (*rows[r])[3];
    }
    c.MotionX = p.Motion[0], c.MotionY = p.Motion[1], c.JitterX = p.Motion[2], c.JitterY = p.Motion[3];
    c.AlphaMin = p.Accumulate[0], c.VarianceFrames = p.Accumulate[1];
    c.ExposureRatio = p.Accumulate[2], c.LengthCap = p.Accumulate[3];
    c.ZLow = p.Change[0], c.ZHigh = p.Change[1], c.FloorJ = p.Change[2], c.FloorA = p.Change[3];
    c.PlaneSine = p.Geometry[0], c.PlaneDepth = p.Geometry[1], c.NormalCos = p.Geometry[2];
    c.MovingFlow = p.Moving[0], c.MovingDepth = p.Moving[1], c.MovingConfidence = p.Moving[2];
    c.Shading = (p.Extent[2] & kTemporalFlagShading) != 0U;
    c.Ratio = p.Geometry[3];
    return c;
}

template <typename T>
struct FrameData {
    const T* Depth = nullptr;
    const T* Normal = nullptr;
    const T* Raw = nullptr;
    const T* Motion = nullptr;
    const T* PrevPos = nullptr;
    const uint32_t* Ids = nullptr;
    const T* Shading = nullptr; // M8: 3 per pixel
};

template <typename T>
struct PixelOut {
    bool Receiver = false, Valid = false, Accepted = false, Moving = false, CurrentValid = false, Changed = false;
    T J[3]{}, A = T(1), VarJ{}, VarA{}, Alpha = T(1), AlphaA = T(1), N{}, NA{}, ZJ{}, ZA{}, Confidence{};
    T Cur[3]{}, CurA = T(1);
    T PrevX = std::numeric_limits<T>::quiet_NaN(), PrevY = std::numeric_limits<T>::quiet_NaN();
    int32_t TapX = 0, TapY = 0;
    uint32_t TapMask = 0U;
};

template <typename T>
void WriteNeutral(T* h) {
    for (uint32_t k = 0; k < kHistoryValues; ++k) h[k] = T(0);
    h[4] = T(1); // A open
    h[5] = T(1);
    h[8] = T(1);
    h[9] = T(1);
}

// One pixel of the temporal pass. `prev` is read only when !Reset; `next` is
// written for every pixel (also non-receivers), so no history value is ever
// uninitialized.
template <typename T>
void TemporalPixelT(const Constants<T>& k, const FrameData<T>& f, const T* prev, const uint32_t* prevIds, T* next,
                    uint32_t* nextIds, uint32_t x, uint32_t y, PixelOut<T>& out) {
    const size_t i = static_cast<size_t>(y) * k.W + x;
    T* h = next + static_cast<size_t>(kHistoryValues) * i;
    nextIds[i] = 0U;
    out = {};
    const T z = f.Depth[i];
    const V3<T> n{f.Normal[3 * i], f.Normal[3 * i + 1], f.Normal[3 * i + 2]};
    if (!IsReceiver(z, n.X, n.Y, n.Z)) {
        WriteNeutral(h);
        return;
    }
    out.Receiver = true;
    // M8: the oriented shading normal (zero: none).
    const auto shadingAt = [&](size_t j) {
        return f.Shading != nullptr ? V3<T>{f.Shading[3 * j], f.Shading[3 * j + 1], f.Shading[3 * j + 2]} : V3<T>{};
    };
    const V3<T> sn = k.Shading ? shadingAt(i) : V3<T>{};
    const V3<T> p = Mul(k.Current.Ray(T(x) + T(0.5), T(y) + T(0.5), k.W, k.H), z);

    // --- Current sample: stratum reconstruction and spatial variance.
    T sw = T(0), sw2 = T(0), sr = T(0), sg = T(0), sb = T(0), sa = T(0), sl = T(0), sl2 = T(0), sa2 = T(0);
    const auto radius = static_cast<int32_t>(k.Radius);
    for (int32_t dy = -radius; dy <= radius; ++dy)
        for (int32_t dx = -radius; dx <= radius; ++dx) {
            const T w = WindowWeight<T>(dx, k.Tile) * WindowWeight<T>(dy, k.Tile);
            if (!(w > T(0))) continue;
            const int32_t qx = static_cast<int32_t>(x) + dx, qy = static_cast<int32_t>(y) + dy;
            if (qx < 0 || qy < 0 || qx >= static_cast<int32_t>(k.W) || qy >= static_cast<int32_t>(k.H)) continue;
            const size_t q = static_cast<size_t>(qy) * k.W + static_cast<size_t>(qx);
            const T zq = f.Depth[q];
            const V3<T> nq{f.Normal[3 * q], f.Normal[3 * q + 1], f.Normal[3 * q + 2]};
            if (!IsReceiver(zq, nq.X, nq.Y, nq.Z)) continue;
            if (dx != 0 || dy != 0) {
                const V3<T> pq = Mul(k.Current.Ray(T(qx) + T(0.5), T(qy) + T(0.5), k.W, k.H), zq);
                if (!Compatible(p, n, z, pq, nq, k.PlaneSine, k.PlaneDepth, k.NormalCos) &&
                    !(k.Shading && SmoothCreaseT(p, n, sn, z, pq, nq, shadingAt(q), k.PlaneSine, k.PlaneDepth,
                                                 k.NormalCos, k.Ratio)))
                    continue;
            }
            const T r = f.Raw[4 * q], g = f.Raw[4 * q + 1], b = f.Raw[4 * q + 2], a = f.Raw[4 * q + 3];
            if (!Finite(r) || !Finite(g) || !Finite(b) || !Finite(a)) continue;
            const T l = Luma(r, g, b);
            sw += w;
            sw2 += w * w;
            sr += w * r;
            sg += w * g;
            sb += w * b;
            sa += w * a;
            sl += w * l;
            sl2 += w * l * l;
            sa2 += w * a * a;
        }
    const T cr = f.Raw[4 * i], cg = f.Raw[4 * i + 1], cb = f.Raw[4 * i + 2], ca = f.Raw[4 * i + 3];
    const bool currentValid = Finite(cr) && Finite(cg) && Finite(cb) && Finite(ca) && sw > T(0);
    T cur[3] = {T(0), T(0), T(0)};
    T curA = T(1), varL = T(0), varA = T(0);
    if (currentValid) {
        const T inv = T(1) / sw;
        const T meanL = sl * inv;
        const T meanA = sa * inv;
        const T vl = std::max(sl2 * inv - meanL * meanL, T(0));
        const T va = std::max(sa2 * inv - meanA * meanA, T(0));
        if (k.Reconstruct) {
            cur[0] = sr * inv, cur[1] = sg * inv, cur[2] = sb * inv;
            curA = meanA;
            const T shrink = sw2 * inv * inv; // variance of a weighted mean of independent samples
            varL = vl * shrink;
            varA = va * shrink;
        } else {
            cur[0] = cr, cur[1] = cg, cur[2] = cb;
            curA = ca;
            varL = vl;
            varA = va;
        }
    }

    // --- History: reprojection, surface compatibility, bilinear taps.
    bool accepted = false, moving = false;
    T confidence = T(1);
    T hJ[3] = {T(0), T(0), T(0)};
    T hm2 = T(0), hA = T(0), ha2 = T(0), hN = T(0), hNA = T(0), hv = T(0), hvA = T(0);
    if (!k.Reset) {
        const T mx = f.Motion[2 * i], my = f.Motion[2 * i + 1];
        V3<T> pp{};
        bool expected = Finite(mx) && Finite(my);
        if (!k.Flow) {
            pp = {f.PrevPos[3 * i], f.PrevPos[3 * i + 1], f.PrevPos[3 * i + 2]};
            expected = expected && Finite(pp.X) && Finite(pp.Y) && Finite(pp.Z);
        } else {
            pp = {k.R[0] * p.X + k.R[1] * p.Y + k.R[2] * p.Z + k.Tr[0],
                  k.R[3] * p.X + k.R[4] * p.Y + k.R[5] * p.Z + k.Tr[1],
                  k.R[6] * p.X + k.R[7] * p.Y + k.R[8] * p.Z + k.Tr[2]};
        }
        const V3<T> np{k.R[0] * n.X + k.R[1] * n.Y + k.R[2] * n.Z, k.R[3] * n.X + k.R[4] * n.Y + k.R[5] * n.Z,
                       k.R[6] * n.X + k.R[7] * n.Y + k.R[8] * n.Z};
        // M8: the shading normal in the previous view; the history's own in value 11.
        const bool historyShading = k.Shading && !k.Ids;
        const V3<T> sp{k.R[0] * sn.X + k.R[1] * sn.Y + k.R[2] * sn.Z, k.R[3] * sn.X + k.R[4] * sn.Y + k.R[5] * sn.Z,
                       k.R[6] * sn.X + k.R[7] * sn.Y + k.R[8] * sn.Z};
        // Previous pixel coordinate (continuous; pixel centres at i + 1/2).
        const T qx = (T(x) + T(0.5)) + mx * k.MotionX + k.JitterX;
        const T qy = (T(y) + T(0.5)) + my * k.MotionY + k.JitterY;
        out.PrevX = qx;
        out.PrevY = qy;
        if (expected && pp.Z < T(0) && Finite(qx) && Finite(qy)) {
            const T zexp = -pp.Z;
            if (k.Flow) {
                // Static prediction projected through the previous camera.
                const T tx = pp.X / zexp - k.Previous.Cx;
                const T ty = pp.Y / zexp - k.Previous.Cy;
                const T det = k.Previous.Ax * k.Previous.By - k.Previous.Bx * k.Previous.Ay;
                const T su = (k.Previous.By * tx - k.Previous.Bx * ty) / det * static_cast<T>(k.W);
                const T sv = (-k.Previous.Ay * tx + k.Previous.Ax * ty) / det * static_cast<T>(k.H);
                const T ex = su - qx, ey = sv - qy;
                moving = !(ex * ex + ey * ey <= k.MovingFlow * k.MovingFlow);
            }
            confidence = moving ? k.MovingConfidence : T(1);
            const T fx0 = qx - T(0.5), fy0 = qy - T(0.5);
            const T ix = std::floor(fx0), iy = std::floor(fy0);
            const T fx = fx0 - ix, fy = fy0 - iy;
            if (std::abs(ix) < T(1.0e9) && std::abs(iy) < T(1.0e9)) {
                out.TapX = static_cast<int32_t>(ix);
                out.TapY = static_cast<int32_t>(iy);
            }
            T sumW = T(0);
            for (int32_t b = 0; b < 2; ++b)
                for (int32_t a = 0; a < 2; ++a) {
                    const T w = (a == 0 ? T(1) - fx : fx) * (b == 0 ? T(1) - fy : fy);
                    if (!(w > T(0))) continue;
                    const T tx = ix + T(a), ty = iy + T(b);
                    if (!(tx >= T(0) && ty >= T(0) && tx < static_cast<T>(k.W) && ty < static_cast<T>(k.H))) continue;
                    const size_t t = static_cast<size_t>(ty) * k.W + static_cast<size_t>(tx);
                    const T* hp = prev + static_cast<size_t>(kHistoryValues) * t;
                    const T zh = hp[15];
                    if (!(zh > T(0)) || !Finite(zh)) continue;
                    if (k.Ids) {
                        const uint32_t id = f.Ids[i], hid = prevIds[t];
                        if (id != 0U && hid != 0U && id != hid) continue;
                    }
                    const V3<T> nh{hp[12], hp[13], hp[14]};
                    bool ok = false;
                    if (moving) {
                        ok = std::abs(zh - zexp) <= k.MovingDepth * zexp && Dot3(nh, np) >= k.NormalCos;
                    } else {
                        const V3<T> xh = Mul(k.Previous.Ray(tx + T(0.5), ty + T(0.5), k.W, k.H), zh);
                        ok = Compatible(pp, nh, zexp, xh, np, k.PlaneSine, k.PlaneDepth, k.NormalCos);
                    }
                    if (!ok && historyShading) {
                        const V3<T> sh = UnpackShadingCode(hp[11]);
                        if (moving) {
                            ok = std::abs(zh - zexp) <= k.MovingDepth * zexp &&
                                 SmoothNormalsT(np, sp, nh, sh, k.NormalCos, k.Ratio);
                        } else {
                            const V3<T> xh = Mul(k.Previous.Ray(tx + T(0.5), ty + T(0.5), k.W, k.H), zh);
                            ok = SmoothCreaseT(pp, np, sp, zexp, xh, nh, sh, k.PlaneSine, k.PlaneDepth, k.NormalCos,
                                               k.Ratio);
                        }
                    }
                    if (!ok) continue;
                    bool finite = true;
                    for (uint32_t v = 0; v < 10; ++v) finite = finite && Finite(hp[v]);
                    if (!finite) continue;
                    out.TapMask |= 1U << static_cast<uint32_t>(a + 2 * b);
                    sumW += w;
                    hJ[0] += w * hp[0];
                    hJ[1] += w * hp[1];
                    hJ[2] += w * hp[2];
                    hm2 += w * hp[3];
                    hA += w * hp[4];
                    ha2 += w * hp[5];
                    hN += w * hp[6];
                    hNA += w * hp[7];
                    hv += w * hp[8];
                    hvA += w * hp[9];
                }
            if (sumW > T(0)) {
                accepted = true;
                const T inv = T(1) / sumW;
                hJ[0] *= inv, hJ[1] *= inv, hJ[2] *= inv;
                hm2 *= inv, hA *= inv, ha2 *= inv, hN *= inv, hNA *= inv, hv *= inv, hvA *= inv;
                // Pre-exposure change: J scales, its second moment by the square.
                hJ[0] *= k.ExposureRatio, hJ[1] *= k.ExposureRatio, hJ[2] *= k.ExposureRatio;
                hm2 *= k.ExposureRatio * k.ExposureRatio;
                // Weakly validated histories keep a shorter memory.
                const T cap = confidence * k.LengthCap;
                hN = std::min(hN, cap);
                hNA = std::min(hNA, cap);
            }
        }
    }
    if (!accepted) {
        // A rejected history is the neutral state: nothing of it survives.
        hJ[0] = hJ[1] = hJ[2] = T(0);
        hm2 = hA = ha2 = hN = hNA = T(0);
        hv = hvA = T(1);
        confidence = T(1);
    }

    // --- Accumulation weights: cumulative mean up to 1 / AlphaMin, raised by
    // the light response (z statistic of the current sample against the history).
    T alpha = T(1), alphaA = T(1), zj = T(0), za = T(0);
    if (accepted && currentValid) {
        const T base = std::max(k.AlphaMin, T(1) / (hN + T(1)));
        const T hl = Luma(hJ[0], hJ[1], hJ[2]);
        const T cl = Luma(cur[0], cur[1], cur[2]);
        const T sigma2 = hN >= k.VarianceFrames ? std::max(hm2 - hl * hl, T(0)) : varL;
        const T floorJ = k.FloorJ * std::max(cl, hl);
        const T den2 = sigma2 * (T(1) + hv) + floorJ * floorJ;
        const T diff = std::abs(cl - hl);
        zj = den2 > T(0) ? diff / std::sqrt(den2) : (diff > T(0) ? std::numeric_limits<T>::infinity() : T(0));
        const T rise = std::clamp((zj - k.ZLow) / (k.ZHigh - k.ZLow), T(0), T(1));
        alpha = std::max(base, rise);
        out.Changed = rise > base;
        const T baseA = std::max(k.AlphaMin, T(1) / (hNA + T(1)));
        const T sigmaA2 = hNA >= k.VarianceFrames ? std::max(ha2 - hA * hA, T(0)) : varA;
        const T denA2 = sigmaA2 * (T(1) + hvA) + k.FloorA * k.FloorA;
        const T diffA = std::abs(curA - hA);
        za = denA2 > T(0) ? diffA / std::sqrt(denA2) : (diffA > T(0) ? std::numeric_limits<T>::infinity() : T(0));
        const T riseA = std::clamp((za - k.ZLow) / (k.ZHigh - k.ZLow), T(0), T(1));
        alphaA = std::max(baseA, riseA);
    }

    // --- Update (the history is this pre-filter estimate).
    T J[3], m2, A, a2, N, NA, v, vA;
    if (currentValid) {
        const T cl = Luma(cur[0], cur[1], cur[2]);
        J[0] = hJ[0] + alpha * (cur[0] - hJ[0]);
        J[1] = hJ[1] + alpha * (cur[1] - hJ[1]);
        J[2] = hJ[2] + alpha * (cur[2] - hJ[2]);
        m2 = hm2 + alpha * (cl * cl - hm2);
        v = (T(1) - alpha) * (T(1) - alpha) * hv + alpha * alpha;
        N = T(1) / alpha;
        A = hA + alphaA * (curA - hA);
        a2 = ha2 + alphaA * (curA * curA - ha2);
        vA = (T(1) - alphaA) * (T(1) - alphaA) * hvA + alphaA * alphaA;
        NA = T(1) / alphaA;
    } else if (accepted) {
        J[0] = hJ[0], J[1] = hJ[1], J[2] = hJ[2];
        m2 = hm2, A = hA, a2 = ha2, N = hN, NA = hNA, v = hv, vA = hvA;
        alpha = alphaA = T(0);
    } else {
        // Receiver without a finite sample and without history: neutral.
        WriteNeutral(h);
        out.Confidence = T(0);
        return;
    }
    const T l = Luma(J[0], J[1], J[2]);
    const T sigma2 = N >= k.VarianceFrames ? std::max(m2 - l * l, T(0)) : varL;
    const T sigmaA2 = NA >= k.VarianceFrames ? std::max(a2 - A * A, T(0)) : varA;
    h[0] = J[0], h[1] = J[1], h[2] = J[2], h[3] = m2;
    h[4] = A, h[5] = a2, h[6] = N, h[7] = NA;
    h[8] = v, h[9] = vA, h[10] = confidence;
    h[11] = static_cast<T>(k.Ids ? f.Ids[i] : 0U); // exact below 2^24
    if (k.Shading && !k.Ids) h[11] = PackShadingCode(sn);
    h[12] = n.X, h[13] = n.Y, h[14] = n.Z, h[15] = z;
    nextIds[i] = k.Ids ? f.Ids[i] : 0U;

    out.Valid = true;
    out.Accepted = accepted;
    out.Moving = moving;
    out.CurrentValid = currentValid;
    out.J[0] = J[0], out.J[1] = J[1], out.J[2] = J[2];
    out.A = A;
    out.VarJ = sigma2 * v;
    out.VarA = sigmaA2 * vA;
    out.Alpha = alpha, out.AlphaA = alphaA, out.N = N, out.NA = NA, out.ZJ = zj, out.ZA = za;
    out.Confidence = confidence;
    out.Cur[0] = cur[0], out.Cur[1] = cur[1], out.Cur[2] = cur[2];
    out.CurA = curA;
}

template <typename F>
void ParallelRows(uint32_t height, uint32_t threads, F&& body) {
    threads = std::clamp(threads, 1U, 16U);
    if (threads == 1U) {
        for (uint32_t y = 0; y < height; ++y) body(y);
        return;
    }
    std::vector<std::thread> pool;
    for (uint32_t t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (uint32_t y = t; y < height; y += threads) body(y);
        });
    for (auto& thread : pool) thread.join();
}

template <typename T>
TemporalPixel Widen(const PixelOut<T>& o) {
    TemporalPixel p;
    p.Receiver = o.Receiver;
    p.Valid = o.Valid;
    p.J = {static_cast<double>(o.J[0]), static_cast<double>(o.J[1]), static_cast<double>(o.J[2])};
    p.A = o.A;
    p.VarianceJ = o.VarJ;
    p.VarianceA = o.VarA;
    p.Alpha = o.Alpha;
    p.AlphaA = o.AlphaA;
    p.Length = o.N;
    p.LengthA = o.NA;
    p.ChangeJ = o.ZJ;
    p.ChangeA = o.ZA;
    p.HistoryAccepted = o.Accepted;
    p.Moving = o.Moving;
    p.ChangeResponse = o.Changed;
    p.Confidence = o.Confidence;
    p.CurrentValid = o.CurrentValid;
    p.Current = {static_cast<double>(o.Cur[0]), static_cast<double>(o.Cur[1]), static_cast<double>(o.Cur[2])};
    p.CurrentA = o.CurA;
    p.PreviousX = static_cast<double>(o.PrevX);
    p.PreviousY = static_cast<double>(o.PrevY);
    p.TapX = o.TapX;
    p.TapY = o.TapY;
    p.TapMask = o.TapMask;
    return p;
}

// ---------------------------------------------------------------------------
// A-trous filter level.

template <typename T>
struct FilterConstants {
    uint32_t W = 0, H = 0, Step = 1;
    RayMap<T> Camera;
    T Sigma{}, Epsilon{}, PlaneSine{}, PlaneDepth{}, NormalCos{};
    bool Shading = false; // M8
    T Ratio{};
};

template <typename T>
FilterConstants<T> FromParams(const FilterShaderParams& p) {
    FilterConstants<T> c;
    c.W = p.Extent[0], c.H = p.Extent[1], c.Step = p.Extent[2];
    c.Camera = {p.ViewX[0], p.ViewX[1], p.ViewX[2], p.ViewY[0], p.ViewY[1], p.ViewY[2]};
    c.Sigma = p.Weights[0], c.Epsilon = p.Weights[1], c.PlaneSine = p.Weights[2], c.PlaneDepth = p.Weights[3];
    c.NormalCos = p.Normals[0];
    c.Ratio = p.Normals[1];
    c.Shading = p.Normals[2] != 0.0F;
    return c;
}

// [B3] eq. 1 / [B6] eq. 2 kernel: B3 spline (1, 4, 6, 4, 1) / 16.
template <typename T>
T Kernel(int32_t d) {
    switch (d) {
    case -2:
    case 2:
        return T(0.0625);
    case -1:
    case 1:
        return T(0.25);
    default:
        return T(0.375);
    }
}

template <typename T>
T ColourWeight(T distance, T den) {
    if (!(distance > T(0))) return T(1);
    return den > T(0) ? std::exp(-distance / den) : T(0);
}

// geo: n.xyz, z per pixel; colour: J.rgb, A; variance: Var J, Var A;
// shading (M8): the oriented shading normal, 3 per pixel.
template <typename T>
void FilterPixelT(const FilterConstants<T>& k, const T* geo, const T* colour, const T* variance, T* colourOut,
                  T* varianceOut, uint32_t x, uint32_t y, const T* shading = nullptr) {
    const size_t i = static_cast<size_t>(y) * k.W + x;
    T* co = colourOut + 4 * i;
    T* vo = varianceOut + 2 * i;
    const T z = geo[4 * i + 3];
    const V3<T> n{geo[4 * i], geo[4 * i + 1], geo[4 * i + 2]};
    const T* c = colour + 4 * i;
    const T* v = variance + 2 * i;
    const bool centre = IsReceiver(z, n.X, n.Y, n.Z) && Finite(c[0]) && Finite(c[1]) && Finite(c[2]) &&
                        Finite(c[3]) && Finite(v[0]) && Finite(v[1]);
    if (!centre) {
        co[0] = co[1] = co[2] = T(0);
        co[3] = T(1);
        vo[0] = vo[1] = T(0);
        return;
    }
    const V3<T> p = Mul(k.Camera.Ray(T(x) + T(0.5), T(y) + T(0.5), k.W, k.H), z);
    const auto shadingAt = [&](size_t j) {
        return shading != nullptr ? V3<T>{shading[3 * j], shading[3 * j + 1], shading[3 * j + 2]} : V3<T>{};
    };
    const V3<T> sn = k.Shading ? shadingAt(i) : V3<T>{};
    const T lp = Luma(c[0], c[1], c[2]);
    T sumJ = T(0), sumA = T(0), r = T(0), g = T(0), b = T(0), a = T(0), varJ = T(0), varA = T(0);
    const auto step = static_cast<int32_t>(k.Step);
    for (int32_t dy = -2; dy <= 2; ++dy)
        for (int32_t dx = -2; dx <= 2; ++dx) {
            const int32_t qx = static_cast<int32_t>(x) + dx * step, qy = static_cast<int32_t>(y) + dy * step;
            // Invalid coordinates are discarded before any read.
            if (qx < 0 || qy < 0 || qx >= static_cast<int32_t>(k.W) || qy >= static_cast<int32_t>(k.H)) continue;
            const size_t q = static_cast<size_t>(qy) * k.W + static_cast<size_t>(qx);
            const T zq = geo[4 * q + 3];
            const V3<T> nq{geo[4 * q], geo[4 * q + 1], geo[4 * q + 2]};
            if (!IsReceiver(zq, nq.X, nq.Y, nq.Z)) continue;
            if (dx != 0 || dy != 0) {
                const V3<T> pq = Mul(k.Camera.Ray(T(qx) + T(0.5), T(qy) + T(0.5), k.W, k.H), zq);
                if (!Compatible(p, n, z, pq, nq, k.PlaneSine, k.PlaneDepth, k.NormalCos) &&
                    !(k.Shading && SmoothCreaseT(p, n, sn, z, pq, nq, shadingAt(q), k.PlaneSine, k.PlaneDepth,
                                                 k.NormalCos, k.Ratio)))
                    continue;
            }
            const T* cq = colour + 4 * q;
            const T* vq = variance + 2 * q;
            if (!Finite(cq[0]) || !Finite(cq[1]) || !Finite(cq[2]) || !Finite(cq[3]) || !Finite(vq[0]) ||
                !Finite(vq[1]))
                continue;
            const T hk = Kernel<T>(dx) * Kernel<T>(dy);
            const T distance = static_cast<T>(kLumaR) * std::abs(cq[0] - c[0]) +
                               static_cast<T>(kLumaG) * std::abs(cq[1] - c[1]) +
                               static_cast<T>(kLumaB) * std::abs(cq[2] - c[2]);
            const T lq = Luma(cq[0], cq[1], cq[2]);
            const T den = k.Sigma * std::sqrt(std::max(v[0] + vq[0], T(0))) + k.Epsilon * (T(0.5) * (lp + lq));
            const T wj = hk * ColourWeight(distance, den);
            const T denA =
                k.Sigma * std::sqrt(std::max(v[1] + vq[1], T(0))) + k.Epsilon * (T(0.5) * (c[3] + cq[3]));
            const T wa = hk * ColourWeight(std::abs(cq[3] - c[3]), denA);
            sumJ += wj;
            r += wj * cq[0];
            g += wj * cq[1];
            b += wj * cq[2];
            varJ += wj * wj * vq[0];
            sumA += wa;
            a += wa * cq[3];
            varA += wa * wa * vq[1];
        }
    // The centre always contributes h(0, 0) = 9/64 with weight 1: sums > 0.
    const T inv = T(1) / sumJ;
    const T invA = T(1) / sumA;
    co[0] = r * inv, co[1] = g * inv, co[2] = b * inv, co[3] = a * invA;
    vo[0] = varJ * inv * inv;
    vo[1] = varA * invA * invA;
}

template <typename T>
std::vector<FilteredPixel> FilterImageT(const FilterInput& in, const FilterSettings& settings) {
    const size_t count = static_cast<size_t>(in.Width) * in.Height;
    std::vector<T> geo(4 * count), colour(4 * count), variance(2 * count), shading(3 * count, T(0));
    for (size_t i = 0; i < count; ++i) {
        if (in.ShadingNormal.size() == 3 * count)
            for (int c = 0; c < 3; ++c) shading[3 * i + c] = static_cast<T>(in.ShadingNormal[3 * i + c]);
        for (int c = 0; c < 3; ++c) geo[4 * i + c] = static_cast<T>(in.Normal[3 * i + c]);
        geo[4 * i + 3] = static_cast<T>(in.Depth[i]);
        for (int c = 0; c < 3; ++c) colour[4 * i + c] = static_cast<T>(in.J[3 * i + c]);
        colour[4 * i + 3] = static_cast<T>(in.A[i]);
        variance[2 * i] = static_cast<T>(in.VarianceJ[i]);
        variance[2 * i + 1] = static_cast<T>(in.VarianceA[i]);
    }
    std::vector<T> colourOut(4 * count), varianceOut(2 * count);
    for (uint32_t level = 0; level < settings.Levels; ++level) {
        FilterConstants<T> k;
        if constexpr (std::is_same_v<T, float>) {
            k = FromParams<float>(MakeFilterShaderParams(in, settings, level));
        } else {
            k.W = in.Width, k.H = in.Height, k.Step = 1U << level;
            k.Camera = {in.ViewX[0], in.ViewX[1], in.ViewX[2], in.ViewY[0], in.ViewY[1], in.ViewY[2]};
            k.Sigma = settings.LuminanceSigma, k.Epsilon = settings.RelativeEpsilon;
            k.PlaneSine = settings.PlaneSine, k.PlaneDepth = settings.PlaneDepth, k.NormalCos = settings.NormalCos;
            k.Shading = settings.ShadingNormals, k.Ratio = settings.HardEdgeRatio;
        }
        ParallelRows(in.Height, settings.Threads, [&](uint32_t y) {
            for (uint32_t x = 0; x < in.Width; ++x)
                FilterPixelT(k, geo.data(), colour.data(), variance.data(), colourOut.data(), varianceOut.data(), x,
                             y, shading.data());
        });
        colour.swap(colourOut);
        variance.swap(varianceOut);
    }
    std::vector<FilteredPixel> out(count);
    for (size_t i = 0; i < count; ++i) {
        FilteredPixel& p = out[i];
        p.Receiver = IsReceiver(geo[4 * i + 3], geo[4 * i], geo[4 * i + 1], geo[4 * i + 2]);
        if (!p.Receiver) continue; // neutral: J = 0, A = 1, variance 0
        p.J = {static_cast<double>(colour[4 * i]), static_cast<double>(colour[4 * i + 1]),
               static_cast<double>(colour[4 * i + 2])};
        p.ARaw = colour[4 * i + 3];
        p.A = std::clamp(p.ARaw, 0.0, 1.0);
        p.VarianceJ = variance[2 * i];
        p.VarianceA = variance[2 * i + 1];
    }
    return out;
}

} // namespace

Vec3 RigidTransform::Apply(const Vec3& x) const noexcept {
    return {R[0] * x.X + R[1] * x.Y + R[2] * x.Z + T[0], R[3] * x.X + R[4] * x.Y + R[5] * x.Z + T[1],
            R[6] * x.X + R[7] * x.Y + R[8] * x.Z + T[2]};
}

Vec3 RigidTransform::Rotate(const Vec3& x) const noexcept {
    return {R[0] * x.X + R[1] * x.Y + R[2] * x.Z, R[3] * x.X + R[4] * x.Y + R[5] * x.Z,
            R[6] * x.X + R[7] * x.Y + R[8] * x.Z};
}

double Luminance(const Rgb& c) noexcept { return Luma(c.R, c.G, c.B); }

const char* ValidateTemporalFrame(const TemporalFrame& f) noexcept {
    const size_t count = static_cast<size_t>(f.Width) * f.Height;
    if (count == 0U) return "empty viewport";
    if (f.Depth.size() != count || f.Normal.size() != 3U * count || f.Raw.size() != 4U * count ||
        f.MotionVectors.size() != 2U * count)
        return "per-pixel buffers do not match the extent";
    if (f.Validation == HistoryValidation::PreviousPosition && f.PreviousPosition.size() != 3U * count)
        return "previous positions declared but not provided";
    if (!f.SurfaceId.empty() && f.SurfaceId.size() != count) return "surface ids do not match the extent";
    return ValidateTemporalFrameState(TemporalStateOf(f));
}

const char* ValidateTemporalFrameState(const TemporalFrameState& f) noexcept {
    if (static_cast<size_t>(f.Width) * f.Height == 0U) return "empty viewport";
    if (f.Motion.Direction == MotionDirection::Unknown || f.Motion.Units == MotionUnits::Unknown ||
        f.Motion.Jitter == JitterInMotion::Unknown)
        return "motion convention is undeclared";
    if (!(f.PreExposure > 0.0) || !std::isfinite(f.PreExposure)) return "pre-exposure must be finite and positive";
    const double det = f.ViewX[0] * f.ViewY[1] - f.ViewX[1] * f.ViewY[0];
    if (!(std::abs(det) > 0.0) || !std::isfinite(det)) return "UV to ray map is singular";
    for (const double v : f.CurrentToPrevious.R)
        if (!std::isfinite(v)) return "camera transform is not finite";
    for (const double v : f.CurrentToPrevious.T)
        if (!std::isfinite(v)) return "camera transform is not finite";
    if (!std::isfinite(f.JitterX) || !std::isfinite(f.JitterY)) return "jitter is not finite";
    return "";
}

const char* ValidateTemporalSettings(const TemporalSettings& s) noexcept {
    if (s.TileLog2 > kMaxRotationTileLog2) return "tile larger than 8 x 8";
    if (!(s.AlphaMin > 0.0 && s.AlphaMin <= 1.0)) return "alpha min outside (0, 1]";
    if (!(s.VarianceFrames >= 1.0) || !std::isfinite(s.VarianceFrames)) return "variance frames below 1";
    if (!(s.ChangeLow >= 0.0) || !(s.ChangeHigh > s.ChangeLow) || !std::isfinite(s.ChangeHigh))
        return "change thresholds must satisfy 0 <= low < high";
    if (!(s.ChangeFloor >= 0.0) || !(s.ChangeFloorA >= 0.0)) return "negative change floor";
    if (!(s.PlaneSine >= 0.0) || !(s.PlaneDepth >= 0.0) || !(s.NormalCos >= -1.0 && s.NormalCos <= 1.0))
        return "surface tolerances out of range";
    if (!(s.MovingFlow >= 0.0) || !(s.MovingDepth >= 0.0) ||
        !(s.MovingConfidence > 0.0 && s.MovingConfidence <= 1.0))
        return "moving-surface tolerances out of range";
    if (!(s.HardEdgeRatio >= 0.0 && s.HardEdgeRatio <= 1.0)) return "hard-edge ratio outside [0, 1]";
    return "";
}

const char* ValidateFilterSettings(const FilterSettings& s) noexcept {
    if (s.Levels > kMaxFilterLevels) return "more than 8 filter levels";
    if (!(s.LuminanceSigma > 0.0) || !std::isfinite(s.LuminanceSigma)) return "luminance sigma must be positive";
    if (!(s.RelativeEpsilon >= 0.0) || !std::isfinite(s.RelativeEpsilon)) return "negative relative epsilon";
    if (!(s.PlaneSine >= 0.0) || !(s.PlaneDepth >= 0.0) || !(s.NormalCos >= -1.0 && s.NormalCos <= 1.0))
        return "surface tolerances out of range";
    if (!(s.HardEdgeRatio >= 0.0 && s.HardEdgeRatio <= 1.0)) return "hard-edge ratio outside [0, 1]";
    return "";
}

// ---------------------------------------------------------------------------
// Frame-level state and reset rules (shared by TemporalAccumulator and the M5
// GPU pass).

TemporalFrameState TemporalStateOf(const TemporalFrame& frame) noexcept {
    TemporalFrameState s;
    s.Width = frame.Width, s.Height = frame.Height;
    s.FrameIndex = frame.FrameIndex;
    s.ViewId = frame.ViewId;
    s.CameraCut = frame.CameraCut;
    s.ExplicitReset = frame.ExplicitReset;
    s.PreExposure = frame.PreExposure;
    s.TransportKey = frame.TransportKey;
    s.ViewX = frame.ViewX, s.ViewY = frame.ViewY;
    s.JitterX = frame.JitterX, s.JitterY = frame.JitterY;
    s.CurrentToPrevious = frame.CurrentToPrevious;
    s.Motion = frame.Motion;
    s.Validation = frame.Validation;
    s.SurfaceIds = !frame.SurfaceId.empty();
    return s;
}

TemporalHistoryState::Plan TemporalHistoryState::Prepare(const TemporalFrameState& frame,
                                                         const TemporalSettings& settings) const noexcept {
    // Reset rules (specification section 7): first frame, camera cut, view
    // change, resize (decision: reset, no resampling), explicit reset or
    // resource invalidation, transport settings, invalid or invalidated
    // exposure change; temporal off never uses a history.
    Plan plan;
    uint32_t reasons = 0U;
    double ratio = 1.0;
    if (!mInitialized) {
        reasons |= kResetFirstFrame;
    } else {
        if (mWidth != frame.Width || mHeight != frame.Height) reasons |= kResetResize;
        if (mViewId != frame.ViewId) reasons |= kResetViewChange;
        if (mTransportKey != frame.TransportKey) reasons |= kResetTransport;
        ratio = frame.PreExposure / mPreExposure;
        if (!(ratio > 0.0) || !std::isfinite(ratio) ||
            (settings.Exposure == ExposureChange::Invalidate && ratio != 1.0))
            reasons |= kResetExposure;
    }
    if (frame.CameraCut) reasons |= kResetCameraCut;
    if (frame.ExplicitReset || mInvalidated) reasons |= kResetExplicit;
    if (!settings.Enabled) reasons |= kResetDisabled;
    plan.ResetReasons = reasons;
    plan.Reset = reasons != 0U;
    plan.ExposureRatio = plan.Reset ? 1.0 : ratio;
    plan.PreviousViewX = plan.Reset ? frame.ViewX : mViewX;
    plan.PreviousViewY = plan.Reset ? frame.ViewY : mViewY;
    plan.PreviousJitterX = mJitterX;
    plan.PreviousJitterY = mJitterY;
    return plan;
}

void TemporalHistoryState::Commit(const TemporalFrameState& frame) noexcept {
    mInitialized = true;
    mInvalidated = false;
    mWidth = frame.Width, mHeight = frame.Height, mViewId = frame.ViewId;
    mTransportKey = frame.TransportKey;
    mPreExposure = frame.PreExposure;
    mViewX = frame.ViewX, mViewY = frame.ViewY;
    mJitterX = frame.JitterX, mJitterY = frame.JitterY;
}

void TemporalHistoryState::Invalidate() noexcept { mInvalidated = true; }

namespace {

// Constants (double) of a planned frame; the FP32 block is derived from them.
Constants<double> PlannedConstants(const TemporalFrameState& frame, const TemporalSettings& settings,
                                   const TemporalHistoryState::Plan& plan) {
    Constants<double> c;
    c.W = frame.Width, c.H = frame.Height;
    c.Reset = plan.Reset;
    c.Tile = 1U << std::min(settings.TileLog2, kMaxRotationTileLog2);
    c.Radius = std::max(c.Tile / 2U, 1U);
    c.Reconstruct = settings.Reconstruction && c.Tile >= 2U;
    c.Flow = frame.Validation == HistoryValidation::FlowAndDepth;
    c.Ids = frame.SurfaceIds;
    c.Current = {frame.ViewX[0], frame.ViewX[1], frame.ViewX[2], frame.ViewY[0], frame.ViewY[1], frame.ViewY[2]};
    c.Previous = {plan.PreviousViewX[0], plan.PreviousViewX[1], plan.PreviousViewX[2],
                  plan.PreviousViewY[0], plan.PreviousViewY[1], plan.PreviousViewY[2]};
    for (int k = 0; k < 9; ++k) c.R[k] = frame.CurrentToPrevious.R[k];
    for (int k = 0; k < 3; ++k) c.Tr[k] = frame.CurrentToPrevious.T[k];
    const double sign = frame.Motion.Direction == MotionDirection::CurrentToPrevious ? 1.0 : -1.0;
    const bool uv = frame.Motion.Units == MotionUnits::NormalizedViewportUv;
    c.MotionX = sign * (uv ? static_cast<double>(frame.Width) : 1.0);
    c.MotionY = sign * (uv ? static_cast<double>(frame.Height) : 1.0);
    // Jitter compensation, applied once: only motion that excludes jitter
    // needs the change of jitter between the two frames.
    const bool excluded = frame.Motion.Jitter == JitterInMotion::Excluded;
    c.JitterX = excluded && !plan.Reset ? plan.PreviousJitterX - frame.JitterX : 0.0;
    c.JitterY = excluded && !plan.Reset ? plan.PreviousJitterY - frame.JitterY : 0.0;
    c.AlphaMin = settings.AlphaMin, c.VarianceFrames = settings.VarianceFrames;
    c.ExposureRatio = plan.ExposureRatio, c.LengthCap = 1.0 / settings.AlphaMin;
    c.ZLow = settings.ChangeLow, c.ZHigh = settings.ChangeHigh;
    c.FloorJ = settings.ChangeFloor, c.FloorA = settings.ChangeFloorA;
    c.PlaneSine = settings.PlaneSine, c.PlaneDepth = settings.PlaneDepth, c.NormalCos = settings.NormalCos;
    c.MovingFlow = settings.MovingFlow, c.MovingDepth = settings.MovingDepth;
    c.MovingConfidence = settings.MovingConfidence;
    c.Shading = settings.ShadingNormals;
    c.Ratio = settings.HardEdgeRatio;
    return c;
}

TemporalShaderParams ShaderParamsOf(const Constants<double>& c) {
    TemporalShaderParams p{};
    const auto f = [](double v) { return static_cast<float>(v); };
    p.Extent = {c.W, c.H, (c.Reset ? kTemporalFlagReset : 0U) | (c.Shading ? kTemporalFlagShading : 0U), c.Radius};
    p.Modes = {c.Tile, c.Reconstruct ? 1U : 0U, c.Flow ? 1U : 0U, c.Ids ? 1U : 0U};
    p.ViewX = {f(c.Current.Ax), f(c.Current.Bx), f(c.Current.Cx), 0.0F};
    p.ViewY = {f(c.Current.Ay), f(c.Current.By), f(c.Current.Cy), 0.0F};
    p.PreviousX = {f(c.Previous.Ax), f(c.Previous.Bx), f(c.Previous.Cx), 0.0F};
    p.PreviousY = {f(c.Previous.Ay), f(c.Previous.By), f(c.Previous.Cy), 0.0F};
    p.Rotation0 = {f(c.R[0]), f(c.R[1]), f(c.R[2]), f(c.Tr[0])};
    p.Rotation1 = {f(c.R[3]), f(c.R[4]), f(c.R[5]), f(c.Tr[1])};
    p.Rotation2 = {f(c.R[6]), f(c.R[7]), f(c.R[8]), f(c.Tr[2])};
    p.Motion = {f(c.MotionX), f(c.MotionY), f(c.JitterX), f(c.JitterY)};
    p.Accumulate = {f(c.AlphaMin), f(c.VarianceFrames), f(c.ExposureRatio), f(c.LengthCap)};
    p.Change = {f(c.ZLow), f(c.ZHigh), f(c.FloorJ), f(c.FloorA)};
    p.Geometry = {f(c.PlaneSine), f(c.PlaneDepth), f(c.NormalCos), c.Shading ? f(c.Ratio) : 0.0F};
    p.Moving = {f(c.MovingFlow), f(c.MovingDepth), f(c.MovingConfidence), 0.0F};
    return p;
}

} // namespace

TemporalShaderParams MakeTemporalShaderParams(const TemporalFrameState& frame, const TemporalSettings& settings,
                                              const TemporalHistoryState::Plan& plan) {
    return ShaderParamsOf(PlannedConstants(frame, settings, plan));
}

// ---------------------------------------------------------------------------
// TemporalAccumulator.

struct TemporalAccumulator::Impl {
    Precision Mode = Precision::Double;
    TemporalHistoryState State;
    std::vector<double> HistoryD[2];
    std::vector<float> HistoryF[2];
    std::vector<uint32_t> Ids[2];
    int Current = 0; // index of the latest history
    TemporalShaderParams Params{};
};

TemporalAccumulator::TemporalAccumulator(Precision precision) : mImpl(std::make_unique<Impl>()) {
    mImpl->Mode = precision;
}
TemporalAccumulator::~TemporalAccumulator() = default;
TemporalAccumulator::TemporalAccumulator(TemporalAccumulator&&) noexcept = default;
TemporalAccumulator& TemporalAccumulator::operator=(TemporalAccumulator&&) noexcept = default;

void TemporalAccumulator::Invalidate() noexcept { mImpl->State.Invalidate(); }

bool TemporalAccumulator::HasHistory() const noexcept { return mImpl->State.HasHistory(); }

const TemporalShaderParams& TemporalAccumulator::LastShaderParams() const noexcept { return mImpl->Params; }

std::vector<double> TemporalAccumulator::History() const {
    const Impl& m = *mImpl;
    if (m.Mode == Precision::Double) return m.HistoryD[m.Current];
    std::vector<double> out(m.HistoryF[m.Current].begin(), m.HistoryF[m.Current].end());
    return out;
}

TemporalResult TemporalAccumulator::Process(const TemporalFrame& frame, const TemporalSettings& settings) {
    TemporalResult result;
    if (*ValidateTemporalFrame(frame) != '\0' || *ValidateTemporalSettings(settings) != '\0') return result;
    Impl& m = *mImpl;
    // --- Reset rules (TemporalHistoryState, the single owner).
    const TemporalFrameState state = TemporalStateOf(frame);
    const TemporalHistoryState::Plan plan = m.State.Prepare(state, settings);
    const uint32_t reasons = plan.ResetReasons;

    // --- Constants (double), and their FP32 block for the twin and the GPU.
    const Constants<double> c = PlannedConstants(state, settings, plan);
    TemporalShaderParams& p = m.Params;
    p = ShaderParamsOf(c);

    // --- Buffers: the next history is written for every pixel.
    const size_t count = static_cast<size_t>(frame.Width) * frame.Height;
    const int next = 1 - m.Current;
    m.Ids[next].assign(count, 0U);
    if (m.Ids[m.Current].size() != count) m.Ids[m.Current].assign(count, 0U); // never read on reset
    std::vector<uint32_t> idsDummy(count, 0U);
    result.ResetReasons = reasons;
    result.Pixels.resize(count);

    const auto run = [&](auto tag) {
        using T = decltype(tag);
        std::vector<T>* history = nullptr;
        if constexpr (std::is_same_v<T, double>)
            history = m.HistoryD;
        else
            history = m.HistoryF;
        history[next].assign(count * kHistoryValues, T(0));
        if (history[m.Current].size() != count * kHistoryValues)
            history[m.Current].assign(count * kHistoryValues, T(0)); // never read on reset
        Constants<T> k;
        std::vector<T> depth, normal, raw, motion, previous, shading;
        FrameData<T> data;
        if constexpr (std::is_same_v<T, double>) {
            k = c;
            data.Depth = frame.Depth.data();
            data.Normal = frame.Normal.data();
            data.Raw = frame.Raw.data();
            data.Motion = frame.MotionVectors.data();
            data.PrevPos = frame.PreviousPosition.empty() ? nullptr : frame.PreviousPosition.data();
            data.Shading = frame.ShadingNormal.size() == 3 * count ? frame.ShadingNormal.data() : nullptr;
        } else {
            k = FromParams<float>(p);
            const auto narrow = [](const std::vector<double>& in, std::vector<float>& out) {
                out.resize(in.size());
                for (size_t i = 0; i < in.size(); ++i) out[i] = static_cast<float>(in[i]);
            };
            narrow(frame.Depth, depth);
            narrow(frame.Normal, normal);
            narrow(frame.Raw, raw);
            narrow(frame.MotionVectors, motion);
            narrow(frame.PreviousPosition, previous);
            if (frame.ShadingNormal.size() == 3 * count) narrow(frame.ShadingNormal, shading);
            data.Shading = shading.empty() ? nullptr : shading.data();
            data.Depth = depth.data();
            data.Normal = normal.data();
            data.Raw = raw.data();
            data.Motion = motion.data();
            data.PrevPos = previous.empty() ? nullptr : previous.data();
        }
        data.Ids = frame.SurfaceId.empty() ? idsDummy.data() : frame.SurfaceId.data();
        const T* prev = history[m.Current].data();
        T* out = history[next].data();
        ParallelRows(frame.Height, settings.Threads, [&](uint32_t y) {
            PixelOut<T> o;
            for (uint32_t x = 0; x < frame.Width; ++x) {
                TemporalPixelT(k, data, prev, m.Ids[m.Current].data(), out, m.Ids[next].data(), x, y, o);
                result.Pixels[static_cast<size_t>(y) * frame.Width + x] = Widen(o);
            }
        });
    };
    if (m.Mode == Precision::Double)
        run(double{});
    else
        run(float{});

    for (const auto& px : result.Pixels) {
        if (!px.Receiver) continue;
        ++result.Counters.Receivers;
        if (px.HistoryAccepted)
            ++result.Counters.Accepted;
        else
            ++result.Counters.Rejected;
        if (px.Moving) ++result.Counters.Moving;
        if (!px.CurrentValid) ++result.Counters.InvalidSamples;
        if (px.ChangeResponse) ++result.Counters.ChangeResponses;
    }

    m.Current = next;
    m.State.Commit(state);
    return result;
}

// ---------------------------------------------------------------------------
// Filter.

FilterShaderParams MakeFilterShaderParams(const FilterInput& in, const FilterSettings& s, uint32_t level) {
    FilterShaderParams p;
    const auto f = [](double v) { return static_cast<float>(v); };
    p.Extent = {in.Width, in.Height, 1U << std::min(level, 30U), 0U};
    p.ViewX = {f(in.ViewX[0]), f(in.ViewX[1]), f(in.ViewX[2]), 0.0F};
    p.ViewY = {f(in.ViewY[0]), f(in.ViewY[1]), f(in.ViewY[2]), 0.0F};
    p.Weights = {f(s.LuminanceSigma), f(s.RelativeEpsilon), f(s.PlaneSine), f(s.PlaneDepth)};
    p.Normals = {f(s.NormalCos), s.ShadingNormals ? f(s.HardEdgeRatio) : 0.0F, s.ShadingNormals ? 1.0F : 0.0F, 0.0F};
    return p;
}

FilterInput MakeFilterInput(const TemporalFrame& frame, const TemporalResult& temporal) {
    FilterInput in;
    in.Width = frame.Width;
    in.Height = frame.Height;
    in.ViewX = frame.ViewX;
    in.ViewY = frame.ViewY;
    const size_t count = static_cast<size_t>(frame.Width) * frame.Height;
    in.Depth.assign(count, 0.0);
    in.Normal.assign(3 * count, 0.0);
    in.J.assign(3 * count, 0.0);
    in.A.assign(count, 1.0);
    in.VarianceJ.assign(count, 0.0);
    in.VarianceA.assign(count, 0.0);
    if (frame.ShadingNormal.size() == 3 * count) in.ShadingNormal.assign(3 * count, 0.0);
    for (size_t i = 0; i < count && i < temporal.Pixels.size(); ++i) {
        const TemporalPixel& t = temporal.Pixels[i];
        if (!t.Valid) continue; // neutral pixels never contribute
        in.Depth[i] = frame.Depth[i];
        for (int c = 0; c < 3; ++c) in.Normal[3 * i + c] = frame.Normal[3 * i + c];
        if (!in.ShadingNormal.empty())
            for (int c = 0; c < 3; ++c) in.ShadingNormal[3 * i + c] = frame.ShadingNormal[3 * i + c];
        in.J[3 * i] = t.J.R, in.J[3 * i + 1] = t.J.G, in.J[3 * i + 2] = t.J.B;
        in.A[i] = t.A;
        in.VarianceJ[i] = t.VarianceJ;
        in.VarianceA[i] = t.VarianceA;
    }
    return in;
}

std::vector<FilteredPixel> FilterImage(const FilterInput& input, const FilterSettings& settings, Precision precision) {
    const size_t count = static_cast<size_t>(input.Width) * input.Height;
    if (count == 0U || *ValidateFilterSettings(settings) != '\0' || input.Depth.size() != count ||
        input.Normal.size() != 3 * count || input.J.size() != 3 * count || input.A.size() != count ||
        input.VarianceJ.size() != count || input.VarianceA.size() != count)
        return {};
    return precision == Precision::Double ? FilterImageT<double>(input, settings) : FilterImageT<float>(input, settings);
}

TemporalFrame MakeTemporalFrame(const GatherInput& input, const PreparedGeometry& geometry,
                                const std::vector<PixelGather>& gather) {
    TemporalFrame frame;
    frame.Width = input.Viewport.Width;
    frame.Height = input.Viewport.Height;
    const size_t count = static_cast<size_t>(frame.Width) * frame.Height;
    frame.Depth.assign(count, 0.0);
    frame.Normal.assign(3 * count, 0.0);
    frame.Raw.assign(4 * count, 0.0);
    frame.MotionVectors.assign(2 * count, 0.0);
    frame.ViewX = input.Camera.ViewX;
    frame.ViewY = input.Camera.ViewY;
    if (geometry.Shading.size() == count) frame.ShadingNormal.assign(3 * count, 0.0);
    for (size_t i = 0; i < count && i < gather.size(); ++i) {
        const PixelGather& g = gather[i];
        frame.Raw[4 * i] = g.J.R, frame.Raw[4 * i + 1] = g.J.G, frame.Raw[4 * i + 2] = g.J.B;
        frame.Raw[4 * i + 3] = g.ARaw;
        if (!g.Valid) {
            frame.Raw[4 * i + 3] = 1.0;
            continue;
        }
        frame.Depth[i] = geometry.ViewDepth[i];
        frame.Normal[3 * i] = geometry.Normal[i].X;
        frame.Normal[3 * i + 1] = geometry.Normal[i].Y;
        frame.Normal[3 * i + 2] = geometry.Normal[i].Z;
        if (!frame.ShadingNormal.empty()) {
            frame.ShadingNormal[3 * i] = geometry.Shading[i].X;
            frame.ShadingNormal[3 * i + 1] = geometry.Shading[i].Y;
            frame.ShadingNormal[3 * i + 2] = geometry.Shading[i].Z;
        }
    }
    return frame;
}

} // namespace Fast::Renderer::DiffuseGi
