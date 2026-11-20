#pragma once

// Diffuse screen-space GI, milestone M4: geometric hierarchy and radiance
// cache (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M4"; specification section 5).
//
// Title-neutral and Vulkan-free; own construction from the specification
// (no hierarchical depth, mip, SPD or SH code of the repository or of any
// dependency was used). One structure per frame and view, built from the
// adapter buffers after the geometric normals (PrepareGeometry):
//
// - Level l cell (I, J) covers the aligned footprint of level-0 texels
//   [I 2^l, (I + 1) 2^l) x [J 2^l, (J + 1) 2^l) clipped to the viewport.
// - ZMin / ZMax bound the view depth of its geometry texels: they are limits,
//   never a surface to light.
// - Validity: kCellGeometry when every texel of the footprint is geometry with
//   a geometric normal; kCellSky / kCellUnavailable record what else it holds.
// - Discontinuity mask: kCellPlanar is set only when the footprint is ONE
//   surface: every texel geometry of one class, the planes of the four children
//   agree with the cell's plane within PlaneTolerance (relative inverse depth,
//   at the corners of each child's footprint, where the deviation of two affine
//   functions is largest) and their normals within NormalCos. A footprint that
//   mixes distinct planes, a silhouette, sky or unavailable texels is not
//   planar: the gather descends a level there and reaches the point reference
//   at level 0. Depth is never averaged across a silhouette.
// - Plane: on a plane the inverse view depth w = 1/z is affine in screen
//   coordinates. Level 0 takes it from the texel's depth and geometric normal
//   (w = (n . ray(x, y)) / (n . P)); a planar cell stores the mean of its
//   children's planes at its centre (same basis: level-0 pixel coordinates).
// - Radiance cache (Mean): the mean source radiance of a planar footprint,
//   every texel weighted 1 (the screen-uniform density of the reference
//   schedule). It reconstructs the current frame's source signal (initialized
//   from the source buffer), it is not a new bounce, and depth and radiance of
//   a cell represent the same surface.
// - SH backend (optional): real orthonormal spherical harmonics in view space
//   (zenith +Z, [B5] eq. 3 convention: Y_1,1 ~ x, Y_1,0 ~ z, Y_1,-1 ~ y, no
//   Condon-Shortley phase; index l^2 + l + m), order ShOrder <= 2, separate RGB
//   coefficients, no chroma compression. Represented function, for directions
//   omega from the source toward the receiver:
//     N(omega) = sum_i L_i max(0, n_i . omega),  D(omega) = sum_i max(0, n_i . omega),
//   the outgoing radiance toward omega is N / D (the mean radiance of the
//   footprint's texels weighted by their projected area toward omega).
//   Operator: projection of the clamped cosine by its zonal coefficients,
//   N_lm += L_i A_l Y_lm(n_i), D_lm += A_l Y_lm(n_i), A_l of [B5] eqs. 6, 8
//   (A_0 = pi, A_1 = 2 pi / 3, A_2 = pi / 4). Coefficients are sums over texels
//   in one basis (view space), never clipped; N / D is radiance, not the
//   cosine-convolved irradiance. The side test of the source stays separate
//   (the gather's one-sided test), and so do geometric discontinuities (the SH
//   is read only on planar cells, where N / D equals the mean exactly).

#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_reference.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kMaxHierarchyLevels = 8;
inline constexpr uint32_t kMaxShOrder = 2; // production backend; the reference basis goes further

inline constexpr uint32_t kCellGeometry = 1U;
inline constexpr uint32_t kCellPlanar = 2U;
inline constexpr uint32_t kCellTwoSided = 4U;
inline constexpr uint32_t kCellSky = 8U;
inline constexpr uint32_t kCellUnavailable = 16U;

struct HierarchySettings {
    uint32_t Levels = 5;              // levels 0 .. Levels - 1 (footprints up to 2^(Levels - 1) texels)
    double PlaneTolerance = 1.0e-3;   // chosen by HierarchyToleranceStudy (document)
    double NormalCos = 0.9995;        // chosen by HierarchyToleranceStudy (document)
    uint32_t ShOrder = 2;             // 0: no SH data; 1 or 2
};

[[nodiscard]] const char* ValidateHierarchySettings(const HierarchySettings& settings) noexcept;

struct HierarchyLevelInfo {
    uint32_t Width = 0, Height = 0;
    size_t Offset = 0; // first cell of the level in DepthHierarchy::Cells
};

struct HierarchyCell {
    uint32_t Flags = 0;
    uint32_t Count = 0; // level-0 texels of the footprint inside the viewport
    double ZMin = 0.0, ZMax = 0.0; // bounds of the geometry texels (ZMin > ZMax: none)
    // Planar cells: w(x, y) = W + Gx (x - xc) + Gy (y - yc), level-0 pixel
    // coordinates, (xc, yc) the aligned footprint centre ((I + 1/2) 2^l, ...).
    double W = 0.0, Gx = 0.0, Gy = 0.0;
    Vec3 Normal; // planar cells: unit, oriented toward the camera
    Rgb Mean;    // planar cells: mean source radiance (radiance cache)
};

struct DepthHierarchy {
    uint32_t Width = 0, Height = 0; // viewport of the gather input
    bool Float32 = false;           // built with float arithmetic (twin of the GPU kernels)
    HierarchySettings Settings;
    std::vector<HierarchyLevelInfo> Levels;
    std::vector<HierarchyCell> Cells;
    uint32_t ShCoefficients = 0; // (ShOrder + 1)^2
    // Per cell and coefficient: N.r, N.g, N.b, D (sums over the geometry
    // texels of the footprint; also on non-planar cells, for the studies).
    std::vector<double> Sh;

    [[nodiscard]] size_t Index(uint32_t level, uint32_t x, uint32_t y) const noexcept {
        return Levels[level].Offset + static_cast<size_t>(y) * Levels[level].Width + x;
    }
    [[nodiscard]] const HierarchyCell& Cell(uint32_t level, uint32_t x, uint32_t y) const noexcept {
        return Cells[Index(level, x, y)];
    }
    [[nodiscard]] const double* CellSh(uint32_t level, uint32_t x, uint32_t y) const noexcept {
        return Sh.empty() ? nullptr : &Sh[Index(level, x, y) * 4U * ShCoefficients];
    }
};

// Double reference (geometry from PrepareGeometry) or the FP32 twin of the GPU
// kernels (geometry from PrepareGeometryF32, float arithmetic, values stored
// widened).
[[nodiscard]] DepthHierarchy BuildDepthHierarchy(const GatherInput& input, const PreparedGeometry& geometry,
                                                 const HierarchySettings& settings, bool float32 = false);

// Viewport texel indices (row-major) of the footprint of a cell.
void CellTexels(const DepthHierarchy& hierarchy, uint32_t level, uint32_t x, uint32_t y,
                std::vector<size_t>& indices);

// Largest level l <= maxLevel with 2^l <= spacing (pixels along the line).
template <typename T>
[[nodiscard]] inline uint32_t FootprintLevel(T spacing, uint32_t maxLevel) noexcept {
    uint32_t level = 0;
    while (level < maxLevel && static_cast<T>(2U << level) <= spacing) ++level;
    return level;
}

// ---------------------------------------------------------------------------
// Spherical harmonics (SH backend). Real orthonormal basis, view space.

[[nodiscard]] inline constexpr uint32_t ShCount(uint32_t order) noexcept { return (order + 1U) * (order + 1U); }

// Order <= 2 closed forms with the constants of [B5] eq. 3 written exactly
// (1 / (2 sqrt(pi)), sqrt(3 / (4 pi)), sqrt(15 / (4 pi)), sqrt(5 / (16 pi)),
// sqrt(15 / (16 pi))). Index l^2 + l + m: 0 Y00; 1 Y1-1 (y); 2 Y10 (z);
// 3 Y11 (x); 4 Y2-2 (xy); 5 Y2-1 (yz); 6 Y20 (3z^2 - 1); 7 Y21 (xz); 8 Y22 (x^2 - y^2).
template <typename T>
inline void ShBasisOrder2(T x, T y, T z, T* out) noexcept {
    out[0] = T(0.28209479177387814);
    out[1] = T(0.4886025119029199) * y;
    out[2] = T(0.4886025119029199) * z;
    out[3] = T(0.4886025119029199) * x;
    out[4] = T(1.0925484305920792) * (x * y);
    out[5] = T(1.0925484305920792) * (y * z);
    out[6] = T(0.31539156525252005) * (T(3) * z * z - T(1));
    out[7] = T(1.0925484305920792) * (x * z);
    out[8] = T(0.5462742152960396) * (x * x - y * y);
}

// A_hat_l of the clamped cosine max(0, cos), [B5] eq. 8: pi, 2 pi / 3, then 0
// for odd l > 1 and 2 pi (-1)^(l/2 - 1) / ((l + 2)(l - 1)) l! / (2^l ((l/2)!)^2) for even l.
[[nodiscard]] double ClampedCosineCoefficient(uint32_t l) noexcept;
template <typename T>
[[nodiscard]] inline T ClampedCosineCoefficientOrder2(uint32_t l) noexcept {
    return l == 0U ? T(3.14159265358979323846) : (l == 1U ? T(2.0943951023931957) : T(0.78539816339744831));
}
[[nodiscard]] inline constexpr uint32_t ShBand(uint32_t index) noexcept {
    return index == 0U ? 0U : (index < 4U ? 1U : (index < 9U ? 2U : 3U));
}

// Any order (reference, double): out has ShCount(order) values.
void ShBasis(const Vec3& unitDirection, uint32_t order, double* out);
// Adds one source texel (radiance, unit normal) to the coefficients
// (ShCount(order) x {N.r, N.g, N.b, D}).
void ProjectSourceTexel(const Rgb& radiance, const Vec3& normal, uint32_t order, double* coefficients);
// N(omega) / D(omega) toward unit omega (source -> receiver); zero when D <= 0.
[[nodiscard]] Rgb EvaluateShRadiance(const double* coefficients, uint32_t order, const Vec3& omega);
// Exact represented function (richer reference): sum_i L_i max(0, n_i . omega) / sum_i max(0, n_i . omega).
[[nodiscard]] Rgb ExactFootprintRadiance(const std::vector<Rgb>& radiance, const std::vector<Vec3>& normals,
                                         const Vec3& omega);

// FP32 production evaluation (order <= 2), the operation order of the GLSL.
template <typename T>
[[nodiscard]] inline Rgb EvaluateShRadianceOrder2(const double* coefficients, uint32_t order, T ox, T oy, T oz) noexcept {
    T basis[9];
    ShBasisOrder2<T>(ox, oy, oz, basis);
    const uint32_t count = ShCount(order);
    T n0 = T(0), n1 = T(0), n2 = T(0), d = T(0);
    for (uint32_t k = 0; k < count; ++k) {
        n0 += static_cast<T>(coefficients[4 * k]) * basis[k];
        n1 += static_cast<T>(coefficients[4 * k + 1]) * basis[k];
        n2 += static_cast<T>(coefficients[4 * k + 2]) * basis[k];
        d += static_cast<T>(coefficients[4 * k + 3]) * basis[k];
    }
    if (!(d > T(0))) return {};
    const T inverse = T(1) / d;
    return {static_cast<double>(n0 * inverse), static_cast<double>(n1 * inverse), static_cast<double>(n2 * inverse)};
}

} // namespace Fast::Renderer::DiffuseGi
