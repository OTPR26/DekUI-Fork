#include "fast/renderer/diffuse_gi_hierarchy.h"

// Depth hierarchy, radiance cache and SH backend of the diffuse screen-space
// GI (milestone M4). Own code from the specification (section 5) and the
// derivations of docs/TRIAEVUM_SCREEN_SPACE_GI.md ("M4"); the SH conventions
// and the clamped-cosine coefficients are those of [B5] eqs. 3, 6 and 8. The
// template core runs in double (reference) and float (twin of the GLSL
// kernels in diffuse_gi_m4_shader.cpp, same operation order).

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Fast::Renderer::DiffuseGi {
namespace {

bool IsGeometryClass(uint8_t c) {
    return c == static_cast<uint8_t>(TexelClass::OneSided) || c == static_cast<uint8_t>(TexelClass::TwoSided);
}

template <typename T>
struct CellT {
    uint32_t Flags = 0, Count = 0;
    T ZMin{}, ZMax{}, W{}, Gx{}, Gy{}, Nx{}, Ny{}, Nz{}, R{}, G{}, B{};
};

template <typename T>
HierarchyCell Widen(const CellT<T>& c) {
    HierarchyCell out;
    out.Flags = c.Flags;
    out.Count = c.Count;
    out.ZMin = static_cast<double>(c.ZMin);
    out.ZMax = static_cast<double>(c.ZMax);
    out.W = static_cast<double>(c.W);
    out.Gx = static_cast<double>(c.Gx);
    out.Gy = static_cast<double>(c.Gy);
    out.Normal = {static_cast<double>(c.Nx), static_cast<double>(c.Ny), static_cast<double>(c.Nz)};
    out.Mean = {static_cast<double>(c.R), static_cast<double>(c.G), static_cast<double>(c.B)};
    return out;
}

template <typename T>
DepthHierarchy BuildT(const GatherInput& input, const PreparedGeometry& geometry, const HierarchySettings& settings,
                      bool float32) {
    DepthHierarchy h;
    h.Width = geometry.Width;
    h.Height = geometry.Height;
    h.Float32 = float32;
    h.Settings = settings;
    const uint32_t levels = std::clamp(settings.Levels, 1U, kMaxHierarchyLevels);
    size_t offset = 0;
    for (uint32_t l = 0; l < levels; ++l) {
        HierarchyLevelInfo info;
        info.Width = (h.Width + (1U << l) - 1U) >> l;
        info.Height = (h.Height + (1U << l) - 1U) >> l;
        info.Offset = offset;
        offset += static_cast<size_t>(info.Width) * info.Height;
        h.Levels.push_back(info);
    }
    const uint32_t order = std::min(settings.ShOrder, kMaxShOrder);
    const uint32_t shCount = settings.ShOrder == 0U ? 0U : ShCount(order);
    h.ShCoefficients = shCount;
    std::vector<CellT<T>> cells(offset);
    std::vector<T> sh(offset * 4U * shCount, T(0));
    const T infinity = std::numeric_limits<T>::infinity();
    const T tolerance = static_cast<T>(settings.PlaneTolerance);
    const T normalCos = static_cast<T>(settings.NormalCos);

    // Level 0: one texel; its plane from the depth and the geometric normal.
    const T ax = static_cast<T>(input.Camera.ViewX[0]), bx = static_cast<T>(input.Camera.ViewX[1]),
            cx = static_cast<T>(input.Camera.ViewX[2]);
    const T ay = static_cast<T>(input.Camera.ViewY[0]), by = static_cast<T>(input.Camera.ViewY[1]),
            cy = static_cast<T>(input.Camera.ViewY[2]);
    const T width = static_cast<T>(h.Width), height = static_cast<T>(h.Height);
    for (uint32_t y = 0; y < h.Height; ++y)
        for (uint32_t x = 0; x < h.Width; ++x) {
            const size_t i = static_cast<size_t>(y) * h.Width + x;
            CellT<T>& c = cells[i];
            c.Count = 1;
            c.ZMin = infinity;
            c.ZMax = -infinity;
            const uint8_t cls = geometry.Class[i];
            if (cls == static_cast<uint8_t>(TexelClass::Sky)) {
                c.Flags = kCellSky;
                continue;
            }
            if (!IsGeometryClass(cls)) {
                c.Flags = kCellUnavailable;
                continue;
            }
            const T z = static_cast<T>(geometry.ViewDepth[i]);
            const T nx = static_cast<T>(geometry.Normal[i].X), ny = static_cast<T>(geometry.Normal[i].Y),
                    nz = static_cast<T>(geometry.Normal[i].Z);
            c.ZMin = z;
            c.ZMax = z;
            if (!(nx * nx + ny * ny + nz * nz > T(0))) {
                c.Flags = kCellUnavailable; // geometry without a geometric normal
                continue;
            }
            c.Flags = kCellGeometry;
            const size_t bi = (static_cast<size_t>(input.Viewport.Y) + y) * input.BufferWidth + input.Viewport.X + x;
            c.R = static_cast<T>(input.Radiance[3 * bi]);
            c.G = static_cast<T>(input.Radiance[3 * bi + 1]);
            c.B = static_cast<T>(input.Radiance[3 * bi + 2]);
            // w = (n . ray(x, y)) / (n . P): affine in pixels, gradients below.
            const T u = (static_cast<T>(x) + T(0.5)) / width;
            const T v = (static_cast<T>(y) + T(0.5)) / height;
            const T rx = ax * u + bx * v + cx;
            const T ry = ay * u + by * v + cy;
            const T d = (nx * rx + ny * ry - nz) * z; // n . P, P = z ray
            const T gx = (nx * ax + ny * ay) / (width * d);
            const T gy = (nx * bx + ny * by) / (height * d);
            // A non-finite source radiance never enters a cell: the texel is
            // not planar, so every footprint holding it descends to the point
            // reference (the cache spreads nothing the M2 gather would not).
            const bool finiteRadiance = std::isfinite(c.R) && std::isfinite(c.G) && std::isfinite(c.B);
            if (d < T(0) && std::isfinite(gx) && std::isfinite(gy) && finiteRadiance) {
                c.Flags |= kCellPlanar;
                if (cls == static_cast<uint8_t>(TexelClass::TwoSided)) c.Flags |= kCellTwoSided;
                c.W = T(1) / z;
                c.Gx = gx;
                c.Gy = gy;
                c.Nx = nx, c.Ny = ny, c.Nz = nz;
            }
            if (shCount > 0U) {
                T basis[9];
                ShBasisOrder2<T>(nx, ny, nz, basis);
                T* out = &sh[i * 4U * shCount];
                for (uint32_t k = 0; k < shCount; ++k) {
                    const T projected = ClampedCosineCoefficientOrder2<T>(ShBand(k)) * basis[k];
                    out[4 * k] = c.R * projected;
                    out[4 * k + 1] = c.G * projected;
                    out[4 * k + 2] = c.B * projected;
                    out[4 * k + 3] = projected;
                }
            }
        }

    // Levels 1 and above: four children (fewer at the viewport border).
    for (uint32_t l = 1; l < levels; ++l) {
        const HierarchyLevelInfo& info = h.Levels[l];
        const HierarchyLevelInfo& child = h.Levels[l - 1U];
        const T size = static_cast<T>(1U << l);
        const T childSize = static_cast<T>(1U << (l - 1U));
        for (uint32_t J = 0; J < info.Height; ++J)
            for (uint32_t I = 0; I < info.Width; ++I) {
                CellT<T>& c = cells[info.Offset + static_cast<size_t>(J) * info.Width + I];
                c.ZMin = infinity;
                c.ZMax = -infinity;
                const T xc = static_cast<T>(I) * size + T(0.5) * size;
                const T yc = static_cast<T>(J) * size + T(0.5) * size;
                bool allGeometry = true, allPlanar = true;
                uint32_t children = 0, twoSided = 0;
                T sumW = T(0), sumGx = T(0), sumGy = T(0), sumNx = T(0), sumNy = T(0), sumNz = T(0);
                T sumR = T(0), sumG = T(0), sumB = T(0);
                const CellT<T>* kids[4] = {};
                uint32_t kidX[4] = {}, kidY[4] = {};
                T* out = shCount > 0U ? &sh[(info.Offset + static_cast<size_t>(J) * info.Width + I) * 4U * shCount] : nullptr;
                for (uint32_t b = 0; b < 2U; ++b)
                    for (uint32_t a = 0; a < 2U; ++a) {
                        const uint32_t kx = 2U * I + a, ky = 2U * J + b;
                        if (kx >= child.Width || ky >= child.Height) continue;
                        const size_t ki = child.Offset + static_cast<size_t>(ky) * child.Width + kx;
                        const CellT<T>& k = cells[ki];
                        kids[children] = &k;
                        kidX[children] = kx;
                        kidY[children] = ky;
                        ++children;
                        c.Count += k.Count;
                        c.Flags |= k.Flags & (kCellSky | kCellUnavailable);
                        allGeometry = allGeometry && (k.Flags & kCellGeometry) != 0U;
                        allPlanar = allPlanar && (k.Flags & kCellPlanar) != 0U;
                        if ((k.Flags & kCellTwoSided) != 0U) ++twoSided;
                        c.ZMin = std::min(c.ZMin, k.ZMin);
                        c.ZMax = std::max(c.ZMax, k.ZMax);
                        if ((k.Flags & kCellPlanar) != 0U) {
                            const T xk = static_cast<T>(kx) * childSize + T(0.5) * childSize;
                            const T yk = static_cast<T>(ky) * childSize + T(0.5) * childSize;
                            sumW += k.W + k.Gx * (xc - xk) + k.Gy * (yc - yk);
                            sumGx += k.Gx;
                            sumGy += k.Gy;
                            sumNx += k.Nx, sumNy += k.Ny, sumNz += k.Nz;
                        }
                        const T weight = static_cast<T>(k.Count);
                        sumR += k.R * weight, sumG += k.G * weight, sumB += k.B * weight;
                        if (out != nullptr) {
                            const T* in = &sh[ki * 4U * shCount];
                            for (uint32_t e = 0; e < 4U * shCount; ++e) out[e] += in[e];
                        }
                    }
                if (allGeometry) c.Flags |= kCellGeometry;
                if (!allGeometry || !allPlanar || (twoSided != 0U && twoSided != children)) continue;
                const T inverse = T(1) / static_cast<T>(children);
                const T w = sumW * inverse, gx = sumGx * inverse, gy = sumGy * inverse;
                const T length = std::sqrt(sumNx * sumNx + sumNy * sumNy + sumNz * sumNz);
                if (!(length > T(0))) continue;
                const T nx = sumNx / length, ny = sumNy / length, nz = sumNz / length;
                bool planar = true;
                for (uint32_t k = 0; k < children && planar; ++k) {
                    const CellT<T>& kid = *kids[k];
                    if (!(kid.Nx * nx + kid.Ny * ny + kid.Nz * nz >= normalCos)) planar = false;
                    const T x0 = static_cast<T>(kidX[k]) * childSize, y0 = static_cast<T>(kidY[k]) * childSize;
                    const T xk = x0 + T(0.5) * childSize, yk = y0 + T(0.5) * childSize;
                    for (uint32_t corner = 0; corner < 4U && planar; ++corner) {
                        const T px = x0 + ((corner & 1U) != 0U ? childSize : T(0));
                        const T py = y0 + ((corner & 2U) != 0U ? childSize : T(0));
                        const T wc = w + gx * (px - xc) + gy * (py - yc);
                        const T wk = kid.W + kid.Gx * (px - xk) + kid.Gy * (py - yk);
                        if (!(wc > T(0)) || !(std::abs(wk - wc) <= tolerance * wc)) planar = false;
                    }
                }
                if (!planar) continue;
                c.Flags |= kCellPlanar;
                if (twoSided != 0U) c.Flags |= kCellTwoSided;
                c.W = w, c.Gx = gx, c.Gy = gy;
                c.Nx = nx, c.Ny = ny, c.Nz = nz;
                const T count = static_cast<T>(c.Count);
                c.R = sumR / count, c.G = sumG / count, c.B = sumB / count;
            }
    }
    h.Cells.reserve(cells.size());
    for (const auto& c : cells) h.Cells.push_back(Widen(c));
    // Non-planar cells keep only flags, count, bounds (mean and plane zero).
    for (size_t i = 0; i < h.Cells.size(); ++i)
        if ((h.Cells[i].Flags & kCellPlanar) == 0U) {
            h.Cells[i].Mean = {};
            h.Cells[i].W = h.Cells[i].Gx = h.Cells[i].Gy = 0.0;
            h.Cells[i].Normal = {};
        }
    h.Sh.assign(sh.begin(), sh.end());
    return h;
}

double Factorial(uint32_t n) {
    double value = 1.0;
    for (uint32_t k = 2; k <= n; ++k) value *= k;
    return value;
}

} // namespace

const char* ValidateHierarchySettings(const HierarchySettings& s) noexcept {
    if (s.Levels < 2U || s.Levels > kMaxHierarchyLevels) return "hierarchy levels outside [2, 8]";
    if (!(s.PlaneTolerance >= 0.0) || !std::isfinite(s.PlaneTolerance)) return "plane tolerance must be finite and >= 0";
    if (!(s.NormalCos >= -1.0 && s.NormalCos <= 1.0)) return "normal cosine outside [-1, 1]";
    if (s.ShOrder > kMaxShOrder) return "SH order above 2";
    return "";
}

DepthHierarchy BuildDepthHierarchy(const GatherInput& input, const PreparedGeometry& geometry,
                                   const HierarchySettings& settings, bool float32) {
    return float32 ? BuildT<float>(input, geometry, settings, true) : BuildT<double>(input, geometry, settings, false);
}

void CellTexels(const DepthHierarchy& h, uint32_t level, uint32_t x, uint32_t y, std::vector<size_t>& indices) {
    indices.clear();
    const uint32_t size = 1U << level;
    for (uint32_t j = y * size; j < std::min((y + 1U) * size, h.Height); ++j)
        for (uint32_t i = x * size; i < std::min((x + 1U) * size, h.Width); ++i)
            indices.push_back(static_cast<size_t>(j) * h.Width + i);
}

double ClampedCosineCoefficient(uint32_t l) noexcept {
    if (l == 0U) return kPi;
    if (l == 1U) return 2.0 * kPi / 3.0;
    if ((l & 1U) != 0U) return 0.0;
    const double sign = ((l / 2U) % 2U == 1U) ? 1.0 : -1.0; // (-1)^(l/2 - 1)
    const double half = Factorial(l / 2U);
    return 2.0 * kPi * sign / ((l + 2.0) * (l - 1.0)) * (Factorial(l) / (std::pow(2.0, l) * half * half));
}

void ShBasis(const Vec3& d, uint32_t order, double* out) {
    // Y_lm = sqrt(2) K_lm Pbar_l^m(z) Re((x + i y)^m) (m > 0), Im(...) (m < 0),
    // K_l0 P_l(z) (m = 0); K_lm = sqrt((2l + 1) / (4 pi) (l - m)! / (l + m)!);
    // Pbar_l^m = P_l^m / sin^m(theta) without the Condon-Shortley phase.
    const double z = d.Z;
    double re[17] = {1.0}, im[17] = {0.0};
    for (uint32_t m = 1; m <= order; ++m) {
        re[m] = re[m - 1] * d.X - im[m - 1] * d.Y;
        im[m] = re[m - 1] * d.Y + im[m - 1] * d.X;
    }
    for (uint32_t m = 0; m <= order; ++m) {
        double pmm = 1.0; // (2m - 1)!!
        for (uint32_t k = 1; k <= m; ++k) pmm *= 2.0 * k - 1.0;
        double previous = 0.0, current = pmm;
        for (uint32_t l = m; l <= order; ++l) {
            if (l == m + 1U) {
                previous = current;
                current = z * (2.0 * m + 1.0) * pmm;
            } else if (l > m + 1U) {
                const double next = ((2.0 * l - 1.0) * z * current - (l + m - 1.0) * previous) / (l - m);
                previous = current;
                current = next;
            }
            const double k = std::sqrt((2.0 * l + 1.0) / (4.0 * kPi) * Factorial(l - m) / Factorial(l + m));
            if (m == 0U) {
                out[l * l + l] = k * current;
            } else {
                out[l * l + l + m] = std::sqrt(2.0) * k * current * re[m];
                out[l * l + l - m] = std::sqrt(2.0) * k * current * im[m];
            }
        }
    }
}

void ProjectSourceTexel(const Rgb& radiance, const Vec3& normal, uint32_t order, double* coefficients) {
    std::vector<double> basis(ShCount(order));
    ShBasis(normal, order, basis.data());
    for (uint32_t l = 0; l <= order; ++l) {
        const double a = ClampedCosineCoefficient(l);
        for (uint32_t k = l * l; k < (l + 1U) * (l + 1U); ++k) {
            const double projected = a * basis[k];
            coefficients[4 * k] += radiance.R * projected;
            coefficients[4 * k + 1] += radiance.G * projected;
            coefficients[4 * k + 2] += radiance.B * projected;
            coefficients[4 * k + 3] += projected;
        }
    }
}

Rgb EvaluateShRadiance(const double* coefficients, uint32_t order, const Vec3& omega) {
    std::vector<double> basis(ShCount(order));
    ShBasis(omega, order, basis.data());
    double n0 = 0.0, n1 = 0.0, n2 = 0.0, d = 0.0;
    for (uint32_t k = 0; k < ShCount(order); ++k) {
        n0 += coefficients[4 * k] * basis[k];
        n1 += coefficients[4 * k + 1] * basis[k];
        n2 += coefficients[4 * k + 2] * basis[k];
        d += coefficients[4 * k + 3] * basis[k];
    }
    if (!(d > 0.0)) return {};
    return {n0 / d, n1 / d, n2 / d};
}

Rgb ExactFootprintRadiance(const std::vector<Rgb>& radiance, const std::vector<Vec3>& normals, const Vec3& omega) {
    double n0 = 0.0, n1 = 0.0, n2 = 0.0, d = 0.0;
    for (size_t i = 0; i < radiance.size(); ++i) {
        const double c = std::max(0.0, Dot(normals[i], omega));
        n0 += radiance[i].R * c;
        n1 += radiance[i].G * c;
        n2 += radiance[i].B * c;
        d += c;
    }
    if (!(d > 0.0)) return {};
    return {n0 / d, n1 / d, n2 / d};
}

} // namespace Fast::Renderer::DiffuseGi
