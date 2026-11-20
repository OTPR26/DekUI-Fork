// M8 acceptance tests of the diffuse screen-space GI: smooth-shaded receivers,
// docs/TRIAEVUM_SCREEN_SPACE_GI.md "M8".
//
// Low-poly meshes with interpolated vertex normals (the renderer's normal
// guide) and one known source. The facet index FI of the M8 brief (mean log
// luminance step across smooth facet edges over the mean step between interior
// neighbours) is measured on the GI image and on the analytic smooth surface:
// the source polygon's form factor from each receiver with its shading normal,
// clipped to the shading hemisphere (Lambert's contour formula). Flat surfaces
// (face = shading normal) must equal M7 bit for bit; contact between distinct
// surfaces and at hard edges must stay.

#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_scene.h"
#include "fast/renderer/diffuse_gi_temporal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

constexpr uint32_t kWidth = 160, kHeight = 120;

Vec3 Sub3(const Vec3& a, const Vec3& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }

GatherSettings M8Settings(bool shading) {
    GatherSettings s;
    s.Radius = 4.5;
    s.Thickness = 0.5;
    s.Sections = 16;
    s.Sectors = 64;
    s.Schedule = CandidateSchedule::Radial;
    s.CandidatesPerSide = 32;
    s.RadialJitter = 0.0; // deterministic and spatially coherent: no noise
    s.Threads = 8;
    s.ShadingNormals = shading;
    return s;
}

// The game's mode: renderer face normals, shading normals from the guide.
GatherInput RasterizeM8(const Scene& scene) {
    GatherInput in = Rasterize(scene);
    in.NormalSource = GeometricNormalSource::RendererFaceNormal;
    return in;
}

double LumaY(const Rgb& c) { return 0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B; }

std::vector<double> LuminanceOf(const std::vector<PixelGather>& image) {
    std::vector<double> y(image.size(), 0.0);
    for (size_t i = 0; i < image.size(); ++i) y[i] = image[i].Valid ? LumaY(image[i].J) : 0.0;
    return y;
}

// Texels whose first hit is one of the primitives [first, last).
std::vector<char> SurfaceMask(const Scene& scene, size_t first, size_t last) {
    const SurfaceIds ids = RasterizeSurfaceIds(scene);
    std::vector<char> mask(ids.Primitive.size(), 0);
    for (size_t i = 0; i < mask.size(); ++i)
        mask[i] = ids.Primitive[i] >= static_cast<int32_t>(first) && ids.Primitive[i] < static_cast<int32_t>(last);
    return mask;
}

struct FacetIndexResult {
    double Index = 0.0;     // smooth facet edges over interior pairs
    double HardIndex = 0.0; // hard edges over interior pairs
    size_t Interior = 0, Smooth = 0, Hard = 0;
};

// The facet index of the M8 brief: receivers are OneSided texels with a face
// and a shading normal; right and down neighbours with |zp - zq| <= 0.03
// min(z); tf, ts the angles between their face and shading normals (degrees);
// interior pair tf < 0.3; smooth facet edge 3 <= tf <= 50 and ts <= max(2,
// tf / 3); hard edge tf >= 3 and ts > tf / 2; eps = 0.05 median Y over
// receivers with Y > 0; g = |ln(Yp + eps) - ln(Yq + eps)|.
FacetIndexResult FacetIndex(const PreparedGeometry& g, const std::vector<double>& y, const std::vector<char>& use) {
    const auto receiver = [&](size_t i) {
        return use[i] != 0 && g.Class[i] == static_cast<uint8_t>(TexelClass::OneSided) && g.ViewDepth[i] > 0.0 &&
               Dot(g.Normal[i], g.Normal[i]) > 0.0 && Dot(g.Shading[i], g.Shading[i]) > 0.0;
    };
    std::vector<double> positive;
    for (size_t i = 0; i < y.size(); ++i)
        if (receiver(i) && y[i] > 0.0) positive.push_back(y[i]);
    FacetIndexResult r;
    if (positive.empty()) return r;
    std::nth_element(positive.begin(), positive.begin() + static_cast<std::ptrdiff_t>(positive.size() / 2),
                     positive.end());
    const double eps = 0.05 * positive[positive.size() / 2];
    const auto angle = [](const Vec3& a, const Vec3& b) {
        return std::acos(std::clamp(Dot(Normalize(a), Normalize(b)), -1.0, 1.0)) * 180.0 / kPi;
    };
    double interior = 0.0, smooth = 0.0, hard = 0.0;
    for (uint32_t yy = 0; yy < g.Height; ++yy)
        for (uint32_t x = 0; x < g.Width; ++x) {
            const size_t p = static_cast<size_t>(yy) * g.Width + x;
            if (!receiver(p)) continue;
            for (int down = 0; down < 2; ++down) {
                const uint32_t qx = x + (down == 0 ? 1U : 0U), qy = yy + (down == 1 ? 1U : 0U);
                if (qx >= g.Width || qy >= g.Height) continue;
                const size_t q = static_cast<size_t>(qy) * g.Width + qx;
                if (!receiver(q)) continue;
                const double zp = g.ViewDepth[p], zq = g.ViewDepth[q];
                if (std::abs(zp - zq) > 0.03 * std::min(zp, zq)) continue;
                const double tf = angle(g.Normal[p], g.Normal[q]), ts = angle(g.Shading[p], g.Shading[q]);
                const double step = std::abs(std::log(y[p] + eps) - std::log(y[q] + eps));
                if (tf < 0.3) {
                    interior += step;
                    ++r.Interior;
                } else if (tf >= 3.0 && tf <= 50.0 && ts <= std::max(2.0, tf / 3.0)) {
                    smooth += step;
                    ++r.Smooth;
                }
                if (tf >= 3.0 && ts > tf / 2.0) {
                    hard += step;
                    ++r.Hard;
                }
            }
        }
    const double base = r.Interior > 0 ? interior / static_cast<double>(r.Interior) : 0.0;
    if (base > 0.0) {
        r.Index = r.Smooth > 0 ? smooth / static_cast<double>(r.Smooth) / base : 0.0;
        r.HardIndex = r.Hard > 0 ? hard / static_cast<double>(r.Hard) / base : 0.0;
    }
    return r;
}

// Form factor of a convex planar polygon from a point with unit normal n,
// the polygon clipped to the half space above the point's plane (Lambert's
// contour integral: F = |sum angle(v_i, v_i+1) (unit(v_i x v_i+1) . n)| / 2 pi).
double PolygonFormFactor(const std::vector<Vec3>& polygon, const Vec3& point, const Vec3& n) {
    std::vector<Vec3> clipped;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Vec3 a = Sub3(polygon[i], point), b = Sub3(polygon[(i + 1) % polygon.size()], point);
        const double da = Dot(a, n), db = Dot(b, n);
        if (da >= 0.0) clipped.push_back(a);
        if ((da >= 0.0) != (db >= 0.0)) clipped.push_back(Add(a, Scale(Sub3(b, a), da / (da - db))));
    }
    if (clipped.size() < 3) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < clipped.size(); ++i) {
        const Vec3 a = Normalize(clipped[i]), b = Normalize(clipped[(i + 1) % clipped.size()]);
        const Vec3 c = Cross(a, b);
        const double length = std::sqrt(Dot(c, c));
        if (!(length > 0.0)) continue;
        sum += std::atan2(length, Dot(a, b)) * Dot(c, n) / length;
    }
    return std::abs(sum) / (2.0 * kPi);
}

// The analytic smooth surface: J = L F of the source (Primitives[0]) seen with
// the receiver's shading normal (the W3 policy: only the shading hemisphere
// bounds the lobe; a convex receiver sees nothing of itself above it).
std::vector<double> AnalyticLuminance(const Scene& scene, const GatherInput& input, const PreparedGeometry& g,
                                      bool shading = true) {
    const Primitive& source = scene.Primitives[0];
    const Vec3 u = Scale(source.Axes[0], source.Half.X), v = Scale(source.Axes[1], source.Half.Y);
    const std::vector<Vec3> polygon{Sub3(Sub3(source.Centre, u), v), Sub3(Add(source.Centre, u), v),
                                    Add(Add(source.Centre, u), v), Add(Sub3(source.Centre, u), v)};
    const double radiance = LumaY(source.Radiance);
    std::vector<double> y(static_cast<size_t>(g.Width) * g.Height, 0.0);
    for (uint32_t yy = 0; yy < g.Height; ++yy)
        for (uint32_t x = 0; x < g.Width; ++x) {
            const size_t i = static_cast<size_t>(yy) * g.Width + x;
            if (!(g.ViewDepth[i] > 0.0) || !(Dot(g.Shading[i], g.Shading[i]) > 0.0)) continue;
            const Vec3& n = shading ? g.Shading[i] : g.Normal[i];
            y[i] = radiance * PolygonFormFactor(polygon, g.Position(input.Camera, x, yy), Normalize(n));
        }
    return y;
}

// Mean |a - b| over mean b on the masked receivers.
double RelativeError(const std::vector<double>& a, const std::vector<double>& b, const std::vector<char>& use,
                     const PreparedGeometry& g) {
    double sum = 0.0, reference = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (use[i] == 0 || !(g.ViewDepth[i] > 0.0) || !(Dot(g.Shading[i], g.Shading[i]) > 0.0)) continue;
        sum += std::abs(a[i] - b[i]);
        reference += std::abs(b[i]);
    }
    return reference > 0.0 ? sum / reference : 0.0;
}

bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

bool SamePixel(const PixelGather& a, const PixelGather& b) {
    return a.Valid == b.Valid && SameBits(a.J.R, b.J.R) && SameBits(a.J.G, b.J.G) && SameBits(a.J.B, b.J.B) &&
           SameBits(a.ARaw, b.ARaw) && SameBits(a.A, b.A) && SameBits(a.Confidence, b.Confidence) &&
           a.OccupiedSectors == b.OccupiedSectors;
}

FilterInput FilterInputOf(const GatherInput& input, const PreparedGeometry& g, const std::vector<PixelGather>& image) {
    FilterInput f;
    f.Width = g.Width;
    f.Height = g.Height;
    f.ViewX = input.Camera.ViewX;
    f.ViewY = input.Camera.ViewY;
    const size_t n = image.size();
    f.Depth.assign(n, 0.0);
    f.Normal.assign(3 * n, 0.0);
    f.ShadingNormal.assign(3 * n, 0.0);
    f.J.assign(3 * n, 0.0);
    f.A.assign(n, 1.0);
    f.VarianceJ.assign(n, 0.0);
    f.VarianceA.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        if (!image[i].Valid) continue;
        f.Depth[i] = g.ViewDepth[i];
        f.Normal[3 * i] = g.Normal[i].X, f.Normal[3 * i + 1] = g.Normal[i].Y, f.Normal[3 * i + 2] = g.Normal[i].Z;
        f.ShadingNormal[3 * i] = g.Shading[i].X, f.ShadingNormal[3 * i + 1] = g.Shading[i].Y,
                            f.ShadingNormal[3 * i + 2] = g.Shading[i].Z;
        f.J[3 * i] = image[i].J.R, f.J[3 * i + 1] = image[i].J.G, f.J[3 * i + 2] = image[i].J.B;
        f.A[i] = image[i].ARaw;
        // A noise level of about 10 % of J: the luminance test lets smooth
        // gradients through and stops at real edges.
        const double y = LumaY(image[i].J);
        f.VarianceJ[i] = 0.01 * y * y + 1.0e-6;
        f.VarianceA[i] = 1.0e-4;
    }
    return f;
}

struct ContinuityCase {
    double FiReference = 0.0, FiM7 = 0.0, FiM8 = 0.0, ErrorM7 = 0.0, ErrorM8 = 0.0;
    double ErrorM7Faceted = 0.0; // M7 against the analytic faceted surface (face-normal lobe)
    size_t Interior = 0, Smooth = 0;
};

ContinuityCase MeasureContinuity(const Scene& scene) {
    GatherInput input = RasterizeM8(scene);
    const PreparedGeometry g = PrepareGeometry(input);
    const std::vector<char> mesh = SurfaceMask(scene, 1, scene.Primitives.size());
    const std::vector<double> reference = AnalyticLuminance(scene, input, g);
    const std::vector<double> m7 = LuminanceOf(GatherImage(input, M8Settings(false)));
    const std::vector<double> m8 = LuminanceOf(GatherImage(input, M8Settings(true)));
    ContinuityCase c;
    const FacetIndexResult ref = FacetIndex(g, reference, mesh), r7 = FacetIndex(g, m7, mesh),
                           r8 = FacetIndex(g, m8, mesh);
    c.FiReference = ref.Index, c.FiM7 = r7.Index, c.FiM8 = r8.Index;
    c.ErrorM7 = RelativeError(m7, reference, mesh, g), c.ErrorM8 = RelativeError(m8, reference, mesh, g);
    c.ErrorM7Faceted = RelativeError(m7, AnalyticLuminance(scene, input, g, false), mesh, g);
    c.Interior = r8.Interior, c.Smooth = r8.Smooth;
    std::printf("DGI_REPORT M8_continuity scene=%s pairs interior=%zu smooth=%zu | FI analytic=%.3f M7=%.3f M8=%.3f | "
                "error vs analytic M7=%.4f M8=%.4f (M7 vs faceted analytic %.4f)\n",
                scene.Name.c_str(), c.Interior, c.Smooth, c.FiReference, c.FiM7, c.FiM8, c.ErrorM7, c.ErrorM8,
                c.ErrorM7Faceted);
    return c;
}

// Declared bounds of the continuity tests (document "M8").
constexpr double kFiAboveAnalytic = 0.6; // FI(M8) <= FI(analytic) + this
constexpr double kM7FiFactor = 2.0;      // the facets show in M7: FI(M7) >= this x FI(M8)
// Mean |J - J_analytic| / mean J_analytic of M8 against the smooth surface:
// the screen-space approximation (sparse radial candidates, sectors), the
// same as M7's against the faceted surface within kErrorMargin.
constexpr double kErrorBound = 0.2;
constexpr double kErrorMargin = 0.03;

void ExpectContinuous(const ContinuityCase& c) {
    EXPECT_GT(c.Interior, 500U);
    EXPECT_GT(c.Smooth, 40U);
    EXPECT_LE(c.FiM8, c.FiReference + kFiAboveAnalytic);
    EXPECT_GE(c.FiM7, kM7FiFactor * c.FiM8);
    EXPECT_LE(c.ErrorM8, kErrorBound);
    EXPECT_LT(c.ErrorM8, c.ErrorM7);
    EXPECT_LE(c.ErrorM8, c.ErrorM7Faceted + kErrorMargin);
}

} // namespace

// R1 + R3: a low-poly cylinder of 8, 12 and 24 sides with radial vertex
// normals is lit continuously across its facets.
TEST(DiffuseGiM8, LowPolyCylinderIsContinuousAcrossFacets) {
    for (const uint32_t sides : {8U, 12U, 24U}) ExpectContinuous(MeasureContinuity(LowPolyCylinderScene(kWidth, kHeight, sides, true)));
}

// The same on icospheres of one and two subdivision levels (80 and 320
// triangles, barycentric vertex normals).
TEST(DiffuseGiM8, LowPolySphereIsContinuousAcrossFacets) {
    for (const uint32_t levels : {1U, 2U})
        ExpectContinuous(MeasureContinuity(LowPolySphereScene(kWidth, kHeight, levels, true, false)));
}

// R5 and flat surfaces: with the shading normal equal to the face normal the
// M8 gather equals M7 bit for bit (catalogue scenes of T05-T10 and the M4
// footprint path), and so does the M8 filter.
TEST(DiffuseGiM8, FlatSurfacesAreBitIdenticalToM7) {
    std::vector<Scene> scenes{CornerRoomScene(64, 48),      PillarsScene(64, 48),        BlackOccluderScene(64, 48, true),
                              ThinBarScene(64, 48),         ThickBlockScene(64, 48),     OverhangScene(64, 48),
                              PanelScene(64, 48, true),     PanelScene(64, 48, false),   CheckerRoomScene(64, 48),
                              SlopedPlaneScene(64, 48, 0.4), WordBoundaryScene(64, 48),  NearPlaneScene(64, 48),
                              LowPolyCylinderScene(64, 48, 12, false), LowPolySphereScene(64, 48, 1, false, true)};
    size_t pixels = 0, different = 0;
    for (Scene& scene : scenes) {
        scene.ShadingGuide = true;
        const GatherInput input = RasterizeM8(scene);
        for (const bool footprint : {false, true}) {
            GatherSettings m7;
            m7.Radius = 2.5;
            m7.Threads = 8;
            if (footprint) {
                m7.Schedule = CandidateSchedule::Radial;
                m7.HierarchyFootprint = true;
            }
            GatherSettings m8 = m7;
            m8.ShadingNormals = true;
            const auto a = GatherImage(input, m7), b = GatherImage(input, m8);
            ASSERT_EQ(a.size(), b.size());
            for (size_t i = 0; i < a.size(); ++i) {
                ++pixels;
                if (!SamePixel(a[i], b[i])) ++different;
            }
            if (footprint) continue;
            const PreparedGeometry g = PrepareGeometry(input);
            const FilterInput f = FilterInputOf(input, g, a);
            FilterSettings f7, f8;
            f8.ShadingNormals = true;
            FilterInput f7Input = f;
            f7Input.ShadingNormal.clear();
            const auto fa = FilterImage(f7Input, f7), fb = FilterImage(f, f8);
            for (size_t i = 0; i < fa.size(); ++i) {
                ++pixels;
                if (!(SameBits(fa[i].J.R, fb[i].J.R) && SameBits(fa[i].J.G, fb[i].J.G) &&
                      SameBits(fa[i].J.B, fb[i].J.B) && SameBits(fa[i].ARaw, fb[i].ARaw)))
                    ++different;
            }
        }
    }
    std::printf("DGI_REPORT M8_flat_bit_identity scenes=%zu pixels=%zu different=%zu\n", scenes.size(), pixels,
                different);
    EXPECT_EQ(different, 0U);
}

// Flat surfaces with the renderer's 8-bit guide (shading normal = face normal
// quantized): within declared bounds of M7 on the same catalogue scenes.
TEST(DiffuseGiM8, FlatSurfacesWithAnEightBitGuideStayWithinBounds) {
    std::vector<Scene> scenes{CornerRoomScene(64, 48), PillarsScene(64, 48), BlackOccluderScene(64, 48, true),
                              ThickBlockScene(64, 48), CheckerRoomScene(64, 48), SlopedPlaneScene(64, 48, 0.4)};
    double sumJ = 0.0, sumRef = 0.0, maxA = 0.0;
    for (Scene& scene : scenes) {
        scene.ShadingGuide = true;
        GatherInput input = RasterizeM8(scene);
        GatherSettings m7;
        m7.Radius = 2.5;
        m7.Threads = 8;
        const auto a = GatherImage(input, m7);
        QuantizeShadingNormals8(input);
        GatherSettings m8 = m7;
        m8.ShadingNormals = true;
        const auto b = GatherImage(input, m8);
        for (size_t i = 0; i < a.size(); ++i) {
            if (!a[i].Valid) continue;
            sumJ += std::abs(LumaY(a[i].J) - LumaY(b[i].J));
            sumRef += LumaY(a[i].J);
            maxA = std::max(maxA, std::abs(a[i].A - b[i].A));
        }
    }
    const double relative = sumRef > 0.0 ? sumJ / sumRef : 0.0;
    std::printf("DGI_REPORT M8_flat_8bit_guide mean_rel_J=%.2e max_abs_A=%.2e\n", relative, maxA);
    EXPECT_LE(relative, 0.01);
    EXPECT_LE(maxA, 0.05);
}

// R3: the creases of a concave smooth surface (inside of a low-poly half
// prism) are not darkened by the adjacent facets; M7 marks them.
TEST(DiffuseGiM8, ConcaveSmoothSurfaceCreasesStayOpen) {
    for (const uint32_t sides : {8U, 12U}) {
        const Scene scene = TroughScene(kWidth, kHeight, sides, true);
        const GatherInput input = RasterizeM8(scene);
        const PreparedGeometry g = PrepareGeometry(input);
        const std::vector<char> mesh = SurfaceMask(scene, 1, scene.Primitives.size());
        const auto m7 = GatherImage(input, M8Settings(false)), m8 = GatherImage(input, M8Settings(true));
        std::vector<double> a7(m7.size()), a8(m8.size());
        for (size_t i = 0; i < m7.size(); ++i) a7[i] = m7[i].A, a8[i] = m8[i].A;
        const FacetIndexResult j7 = FacetIndex(g, LuminanceOf(m7), mesh), j8 = FacetIndex(g, LuminanceOf(m8), mesh);
        const FacetIndexResult o7 = FacetIndex(g, a7, mesh), o8 = FacetIndex(g, a8, mesh);
        std::printf("DGI_REPORT M8_trough sides=%u pairs interior=%zu smooth=%zu | FI J M7=%.3f M8=%.3f | FI A M7=%.3f "
                    "M8=%.3f\n",
                    sides, j8.Interior, j8.Smooth, j7.Index, j8.Index, o7.Index, o8.Index);
        // The creases show in the occlusion: the adjacent facets darken them
        // in M7 (R3). The light from the floor is continuous in both.
        EXPECT_GT(j8.Smooth, 40U);
        EXPECT_LE(j8.Index, 1.0 + kFiAboveAnalytic);
        EXPECT_LE(j8.Index, j7.Index);
        EXPECT_LE(o8.Index, 1.0 + kFiAboveAnalytic);
        EXPECT_GE(o7.Index, kM7FiFactor * o8.Index);
    }
}

namespace {

// Mean occlusion 1 - A of the masked receivers within `band` (view depth
// metres) of a point set predicate.
template <typename Near>
double MeanOcclusion(const std::vector<PixelGather>& image, const std::vector<char>& use, const PreparedGeometry& g,
                     const GatherInput& input, Near near, size_t& count) {
    double sum = 0.0;
    count = 0;
    for (uint32_t y = 0; y < g.Height; ++y)
        for (uint32_t x = 0; x < g.Width; ++x) {
            const size_t i = static_cast<size_t>(y) * g.Width + x;
            if (use[i] == 0 || !image[i].Valid || !near(g.Position(input.Camera, x, y))) continue;
            sum += 1.0 - image[i].A;
            ++count;
        }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

} // namespace

// Hard edge: a wall meeting a floor at 90 degrees keeps its contact occlusion
// when each face carries smoothly bent shading normals (the shading jumps with
// the face at the crease), and equals M7 bit for bit without the bend.
TEST(DiffuseGiM8, HardWallFloorEdgeKeepsItsContact) {
    for (const double bend : {0.0, 20.0}) {
        const Scene scene = WallFloorScene(kWidth, kHeight, bend);
        const GatherInput input = RasterizeM8(scene);
        const PreparedGeometry g = PrepareGeometry(input);
        const std::vector<char> all = SurfaceMask(scene, 0, scene.Primitives.size());
        const auto m7 = GatherImage(input, M8Settings(false)), m8 = GatherImage(input, M8Settings(true));
        const auto crease = [](const Vec3& p) { return std::abs(p.Z + 7.0) < 0.5 && std::abs(p.Y + 1.0) < 0.5; };
        size_t n7 = 0, n8 = 0;
        const double o7 = MeanOcclusion(m7, all, g, input, crease, n7), o8 = MeanOcclusion(m8, all, g, input, crease, n8);
        std::printf("DGI_REPORT M8_hard_edge bend=%.0f crease_texels=%zu occlusion M7=%.4f M8=%.4f\n", bend, n8, o7, o8);
        EXPECT_GT(n8, 100U);
        EXPECT_GT(o7, 0.05);
        if (bend == 0.0) {
            for (size_t i = 0; i < m7.size(); ++i) ASSERT_TRUE(SamePixel(m7[i], m8[i]));
        } else {
            EXPECT_GE(o8, 0.85 * o7);
        }
    }
}

// Distinct surfaces: a smooth sphere resting on a floor keeps the contact
// occlusion on the floor and on its own lower half.
TEST(DiffuseGiM8, ObjectRestingOnAPlaneKeepsItsContact) {
    const Scene scene = LowPolySphereScene(kWidth, kHeight, 2, true, true);
    const GatherInput input = RasterizeM8(scene);
    const PreparedGeometry g = PrepareGeometry(input);
    const std::vector<char> floor = SurfaceMask(scene, 1, 2), sphere = SurfaceMask(scene, 2, scene.Primitives.size());
    const auto m7 = GatherImage(input, M8Settings(false)), m8 = GatherImage(input, M8Settings(true));
    const Vec3 contact{0.3, -0.8, -3.2};
    const auto near = [&](const Vec3& p) {
        const double dx = p.X - contact.X, dy = p.Y - contact.Y, dz = p.Z - contact.Z;
        return std::sqrt(dx * dx + dy * dy + dz * dz) < 0.6;
    };
    size_t nf = 0, ns = 0;
    const double f7 = MeanOcclusion(m7, floor, g, input, near, nf), f8 = MeanOcclusion(m8, floor, g, input, near, nf);
    const double s7 = MeanOcclusion(m7, sphere, g, input, near, ns), s8 = MeanOcclusion(m8, sphere, g, input, near, ns);
    std::printf("DGI_REPORT M8_contact floor_texels=%zu occlusion M7=%.4f M8=%.4f | sphere_texels=%zu occlusion "
                "M7=%.4f M8=%.4f\n",
                nf, f7, f8, ns, s7, s8);
    EXPECT_GT(nf, 50U);
    EXPECT_GT(ns, 20U);
    EXPECT_GT(f7, 0.1);
    EXPECT_NEAR(f8, f7, 0.02 * f7);
    EXPECT_GE(s8, 0.85 * s7);
}

// R2 on the CPU: the a-trous filter crosses the smooth creases of one surface
// (M8) instead of filtering each facet apart, which would step a continuous
// gradient at every facet edge.
TEST(DiffuseGiM8, FilterKeepsSmoothSurfacesContinuous) {
    const Scene scene = LowPolyCylinderScene(kWidth, kHeight, 12, true);
    const GatherInput input = RasterizeM8(scene);
    const PreparedGeometry g = PrepareGeometry(input);
    const std::vector<char> mesh = SurfaceMask(scene, 1, scene.Primitives.size());
    const auto gathered = GatherImage(input, M8Settings(true));
    const FilterInput f = FilterInputOf(input, g, gathered);
    FilterSettings faceEdges, shadingEdges;
    shadingEdges.ShadingNormals = true;
    FilterInput faceInput = f;
    faceInput.ShadingNormal.clear();
    const auto lum = [](const std::vector<FilteredPixel>& image) {
        std::vector<double> y(image.size(), 0.0);
        for (size_t i = 0; i < image.size(); ++i) y[i] = image[i].Receiver ? LumaY(image[i].J) : 0.0;
        return y;
    };
    const double raw = FacetIndex(g, LuminanceOf(gathered), mesh).Index;
    const double face = FacetIndex(g, lum(FilterImage(faceInput, faceEdges)), mesh).Index;
    const double shading = FacetIndex(g, lum(FilterImage(f, shadingEdges)), mesh).Index;
    std::printf("DGI_REPORT M8_filter FI gather=%.3f filtered face_edges=%.3f shading_edges=%.3f\n", raw, face, shading);
    EXPECT_LE(shading, raw + 0.25);
    EXPECT_LT(shading, face);
}
