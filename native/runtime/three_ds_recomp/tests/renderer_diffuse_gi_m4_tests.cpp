// M4 tests of the diffuse screen-space GI specification (sections 5, 6, 10
// T10, 12): depth hierarchy, radiance cache and SH backend, metric radial
// distribution, guiding, blue-noise masks, and the T10 comparison of every
// optimization with the M1-M3 baseline. Lines starting with DGI_M4 are the
// reproducible numbers quoted in docs/TRIAEVUM_SCREEN_SPACE_GI.md ("M4").
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_guiding.h"
#include "fast/renderer/diffuse_gi_hierarchy.h"
#include "fast/renderer/diffuse_gi_noise.h"
#include "fast/renderer/diffuse_gi_scene.h"
#include "fast/renderer/diffuse_gi_temporal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kDegree = kPi / 180.0;

struct Prepared {
    Scene SceneValue;
    GatherInput Input;
    PreparedGeometry Geometry;
};

Prepared Make(const Scene& scene, bool faceNormals = false) {
    Prepared p{scene, Rasterize(scene), {}};
    if (faceNormals) p.Input.NormalSource = GeometricNormalSource::RendererFaceNormal;
    p.Geometry = PrepareGeometry(p.Input);
    return p;
}

DepthHierarchy Hierarchy(const Prepared& p, const HierarchySettings& settings = {}, bool float32 = false) {
    if (float32) return BuildDepthHierarchy(p.Input, PrepareGeometryF32(p.Input), settings, true);
    return BuildDepthHierarchy(p.Input, p.Geometry, settings, false);
}

GatherSettings RadialSettings(double radius, double thickness, uint32_t sections = 8, uint32_t k = 16,
                              double exponent = 2.0) {
    GatherSettings g;
    g.Radius = radius;
    g.Thickness = thickness;
    g.Sections = sections;
    g.Sectors = 64;
    g.Schedule = CandidateSchedule::Radial;
    g.CandidatesPerSide = k;
    g.RadialExponent = exponent;
    g.RadialJitter = 0.5;
    g.Threads = 8;
    return g;
}

GatherSettings EverySettings(double radius, double thickness, uint32_t sections = 8) {
    GatherSettings g;
    g.Radius = radius;
    g.Thickness = thickness;
    g.Sections = sections;
    g.Sectors = 64;
    g.Threads = 8;
    return g;
}

double Luma(const Rgb& c) { return 0.2126 * c.R + 0.7152 * c.G + 0.0722 * c.B; }

bool SameBits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

bool Identical(const PixelGather& a, const PixelGather& b) {
    return a.Valid == b.Valid && SameBits(a.J.R, b.J.R) && SameBits(a.J.G, b.J.G) && SameBits(a.J.B, b.J.B) &&
           SameBits(a.ARaw, b.ARaw) && SameBits(a.A, b.A) && SameBits(a.Confidence, b.Confidence) &&
           a.OccupiedSectors == b.OccupiedSectors && a.Counters.Scheduled == b.Counters.Scheduled &&
           a.Counters.Valid == b.Counters.Valid && a.Counters.Sky == b.Counters.Sky &&
           a.Counters.SelfRejected == b.Counters.SelfRejected && a.Counters.BeyondRadius == b.Counters.BeyondRadius &&
           a.Counters.Degenerate == b.Counters.Degenerate && a.Counters.BackFacing == b.Counters.BackFacing &&
           a.Counters.Duplicates == b.Counters.Duplicates && a.Counters.Occupying == b.Counters.Occupying &&
           a.Counters.Coarse == 0U && b.Counters.Coarse == 0U;
}

// Image statistics of J (luminance) and A between two gathers over the
// pixels valid in both.
struct ImageDiff {
    double MeanJa = 0.0, MeanJb = 0.0, MeanAa = 0.0, MeanAb = 0.0;
    double MeanAbsJ = 0.0, P95AbsJ = 0.0, MaxAbsJ = 0.0, MeanAbsA = 0.0, MaxAbsA = 0.0;
    size_t Pixels = 0;
    [[nodiscard]] double MeanBiasJ() const { return MeanJb > 0.0 ? MeanJa / MeanJb - 1.0 : 0.0; }
    [[nodiscard]] double RelMeanAbsJ() const { return MeanJb > 0.0 ? MeanAbsJ / MeanJb : 0.0; }
};

ImageDiff Compare(const std::vector<PixelGather>& a, const std::vector<PixelGather>& b) {
    ImageDiff d;
    std::vector<double> errors;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!a[i].Valid || !b[i].Valid) continue;
        const double ja = Luma(a[i].J), jb = Luma(b[i].J);
        d.MeanJa += ja, d.MeanJb += jb;
        d.MeanAa += a[i].ARaw, d.MeanAb += b[i].ARaw;
        const double e = std::abs(ja - jb);
        errors.push_back(e);
        d.MeanAbsJ += e;
        d.MaxAbsJ = std::max(d.MaxAbsJ, e);
        const double ea = std::abs(a[i].ARaw - b[i].ARaw);
        d.MeanAbsA += ea;
        d.MaxAbsA = std::max(d.MaxAbsA, ea);
        ++d.Pixels;
    }
    const double n = static_cast<double>(std::max<size_t>(d.Pixels, 1));
    d.MeanJa /= n, d.MeanJb /= n, d.MeanAa /= n, d.MeanAb /= n, d.MeanAbsJ /= n, d.MeanAbsA /= n;
    if (!errors.empty()) {
        std::sort(errors.begin(), errors.end());
        d.P95AbsJ = errors[std::min(errors.size() - 1, static_cast<size_t>(0.95 * errors.size()))];
    }
    return d;
}

uint64_t SumCoarse(const std::vector<PixelGather>& image) {
    uint64_t n = 0;
    for (const auto& p : image) n += p.Counters.Coarse;
    return n;
}
uint64_t SumScheduled(const std::vector<PixelGather>& image) {
    uint64_t n = 0;
    for (const auto& p : image) n += p.Counters.Scheduled;
    return n;
}

struct Rng {
    uint64_t State;
    double Uniform() {
        State = State * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(State >> 11U) * (1.0 / 9007199254740992.0);
    }
};

std::vector<Prepared> StudyScenes(uint32_t w, uint32_t h, bool faceNormals) {
    std::vector<Prepared> out;
    for (const Scene& s : {CornerRoomScene(w, h), PillarsScene(w, h), BlackOccluderScene(w, h, true),
                           ThinBarScene(w, h), ThickBlockScene(w, h), WordBoundaryScene(w, h), OverhangScene(w, h),
                           PanelScene(w, h, false), CheckerRoomScene(w, h), SlopedPlaneScene(w, h, 30.0 * kDegree),
                           SlopedPlaneScene(w, h, 60.0 * kDegree), SlopedPlaneScene(w, h, 75.0 * kDegree)})
        out.push_back(Make(s, faceNormals));
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// M4 off: the M1-M3 paths are unchanged bit for bit, even with every M4 input
// present (hierarchy, a non-uniform guide table, a noise mask, guide records).

TEST(DiffuseGiM4, SwitchesOffKeepM1M3BitForBit) {
    NoiseMaskSettings noiseSettings;
    noiseSettings.Width = noiseSettings.Height = 8, noiseSettings.Depth = 4;
    const NoiseMask noise = GenerateWhiteNoiseMask(noiseSettings);
    size_t images = 0, pixels = 0, mismatches = 0;
    for (const Scene& scene : {CornerRoomScene(96, 64), PillarsScene(80, 56), BlackOccluderScene(72, 48, true)}) {
        const Prepared p = Make(scene);
        const DepthHierarchy h64 = Hierarchy(p);
        const DepthHierarchy h32 = Hierarchy(p, {}, true);
        GuidingSettings gs;
        const GuideTable table = MakeGuideTable(p.Input.Viewport.Width, p.Input.Viewport.Height, gs,
                                                {0.1, 0.5, 2.0, 0.3, 0.05, 0.9, 1.5, 0.2});
        std::vector<GatherSettings> variants;
        variants.push_back(EverySettings(2.5, 0.5, 4));
        variants.push_back(RadialSettings(2.5, 0.5, 6, 16, 2.0));
        GatherSettings stratified = RadialSettings(3.0, 0.5, 1, 12, 1.5);
        stratified.Rotations.Mode = RotationMode::Stratified;
        stratified.Rotations.Frame = 5;
        stratified.SequenceRadialJitter = true;
        variants.push_back(stratified);
        for (const auto& settings : variants) {
            ASSERT_STREQ(ValidateGatherSettings(settings), "");
            std::vector<GuideSample> records;
            for (int precision = 0; precision < 2; ++precision) {
                GatherAcceleration acc;
                acc.Hierarchy = precision == 0 ? &h64 : &h32;
                acc.Guide = &table;
                acc.Noise = &noise;
                acc.Samples = &records;
                ASSERT_STREQ(ValidateGatherAcceleration(p.Input, settings, acc), "");
                const auto plain = precision == 0 ? GatherImage(p.Input, settings) : GatherImageF32(p.Input, settings);
                const auto with = precision == 0 ? GatherImage(p.Input, settings, acc)
                                                 : GatherImageF32(p.Input, settings, acc);
                ++images;
                for (size_t i = 0; i < plain.size(); ++i, ++pixels)
                    if (!Identical(plain[i], with[i])) ++mismatches;
            }
        }
    }
    std::printf("DGI_M4 off_bit_identity images=%zu pixels=%zu mismatches=%zu\n", images, pixels, mismatches);
    EXPECT_EQ(mismatches, 0U);
}

// ---------------------------------------------------------------------------
// Hierarchy invariants: counts, flags, bounds, planes, means and SH sums
// against a brute force over the footprint texels.

TEST(DiffuseGiM4, HierarchyBoundsFlagsPlanesAndCacheAgainstBruteForce) {
    size_t cells = 0, flagErrors = 0, boundErrors = 0, countErrors = 0, planarCells = 0;
    double planeDeviation = 0.0, normalAngle = 0.0, meanError = 0.0, shError = 0.0;
    std::vector<size_t> texels;
    for (bool faceNormals : {false, true})
        for (const Scene& scene : {CornerRoomScene(97, 61), PillarsScene(96, 64), CheckerRoomScene(96, 64),
                                   SlopedPlaneScene(96, 64, 60.0 * kDegree)}) {
            const Prepared p = Make(scene, faceNormals);
            const DepthHierarchy h = Hierarchy(p);
            const uint32_t w = p.Geometry.Width;
            for (uint32_t l = 0; l < h.Levels.size(); ++l)
                for (uint32_t y = 0; y < h.Levels[l].Height; ++y)
                    for (uint32_t x = 0; x < h.Levels[l].Width; ++x) {
                        const HierarchyCell& c = h.Cell(l, x, y);
                        CellTexels(h, l, x, y, texels);
                        ++cells;
                        if (c.Count != texels.size()) ++countErrors;
                        bool geometry = true, sky = false, unavailable = false;
                        double zmin = std::numeric_limits<double>::infinity(), zmax = -zmin;
                        Rgb sum;
                        std::vector<double> shSum(4U * h.ShCoefficients, 0.0);
                        for (const size_t i : texels) {
                            const uint8_t cls = p.Geometry.Class[i];
                            const bool g = cls == 1U || cls == 2U;
                            const Vec3& n = p.Geometry.Normal[i];
                            const bool normal = Dot(n, n) > 0.0;
                            if (g) zmin = std::min(zmin, p.Geometry.ViewDepth[i]), zmax = std::max(zmax, p.Geometry.ViewDepth[i]);
                            if (cls == 3U) sky = true;
                            if (!(g && normal) && cls != 3U) unavailable = true;
                            if (!(g && normal)) geometry = false;
                            if (g && normal) {
                                const size_t bi = (static_cast<size_t>(p.Input.Viewport.Y) + i / w) * p.Input.BufferWidth +
                                                  p.Input.Viewport.X + i % w;
                                const Rgb r{p.Input.Radiance[3 * bi], p.Input.Radiance[3 * bi + 1], p.Input.Radiance[3 * bi + 2]};
                                sum.R += r.R, sum.G += r.G, sum.B += r.B;
                                ProjectSourceTexel(r, n, h.Settings.ShOrder, shSum.data());
                            }
                        }
                        if (((c.Flags & kCellGeometry) != 0U) != geometry || ((c.Flags & kCellSky) != 0U) != sky ||
                            ((c.Flags & kCellUnavailable) != 0U) != unavailable)
                            ++flagErrors;
                        if (zmin <= zmax && (c.ZMin != zmin || c.ZMax != zmax)) ++boundErrors;
                        const double* sh = h.CellSh(l, x, y);
                        for (size_t k = 0; k < shSum.size(); ++k)
                            shError = std::max(shError, std::abs(sh[k] - shSum[k]) / std::max(1.0, std::abs(shSum[k])));
                        if ((c.Flags & kCellPlanar) == 0U) continue;
                        ++planarCells;
                        const double size = static_cast<double>(1U << l);
                        const double xc = (x + 0.5) * size, yc = (y + 0.5) * size;
                        for (const size_t i : texels) {
                            const double px = i % w + 0.5, py = static_cast<double>(i / w) + 0.5;
                            const double plane = c.W + c.Gx * (px - xc) + c.Gy * (py - yc);
                            const double truth = 1.0 / p.Geometry.ViewDepth[i];
                            planeDeviation = std::max(planeDeviation, std::abs(plane - truth) / truth);
                            normalAngle = std::max(normalAngle, std::acos(std::min(1.0, Dot(c.Normal, p.Geometry.Normal[i]))));
                        }
                        const double count = static_cast<double>(texels.size());
                        meanError = std::max({meanError, std::abs(c.Mean.R - sum.R / count), std::abs(c.Mean.G - sum.G / count),
                                              std::abs(c.Mean.B - sum.B / count)});
                    }
        }
    std::printf("DGI_M4 hierarchy_invariants cells=%zu planar=%zu count_errors=%zu flag_errors=%zu bound_errors=%zu "
                "planar_texel_plane_dev_max=%.2e planar_texel_normal_angle_max_deg=%.3f cache_mean_max_abs=%.2e "
                "sh_sum_max_rel=%.2e\n",
                cells, planarCells, countErrors, flagErrors, boundErrors, planeDeviation, normalAngle / kDegree, meanError,
                shError);
    EXPECT_EQ(countErrors, 0U);
    EXPECT_EQ(flagErrors, 0U);
    EXPECT_EQ(boundErrors, 0U);
    EXPECT_LT(meanError, 1.0e-12);
    EXPECT_LT(shError, 1.0e-12);
    EXPECT_LT(planeDeviation, 4.0 * HierarchySettings{}.PlaneTolerance);
}

// ---------------------------------------------------------------------------
// The discontinuity mask never bridges two surfaces: every planar cell above
// level 0 shows one face of one primitive (ground truth of the synthetic
// scenes), including the near dark box in front of the bright wall. The
// tolerance grid chooses PlaneTolerance and NormalCos (largest coverage with
// zero bridging, with a margin).

TEST(DiffuseGiM4, HierarchyNeverBridgesSilhouettesAndToleranceStudy) {
    struct Variant {
        const char* Name;
        uint32_t W, H;
        bool FaceNormals;
        DepthStorage Storage;
    };
    const Variant variants[] = {{"depth_normals_f32", 96, 64, false, DepthStorage::Float32},
                                {"face_normals_f32", 96, 64, true, DepthStorage::Float32},
                                {"depth_normals_unorm24", 96, 64, false, DepthStorage::Unorm24},
                                {"depth_normals_192x128", 192, 128, false, DepthStorage::Float32}};
    const double tolerances[] = {1.0e-5, 1.0e-4, 1.0e-3, 1.0e-2, 1.0e-1};
    const double cosines[] = {0.99, 0.999, 0.9995, 0.9999};
    std::vector<size_t> texels;
    size_t defaultBridging = 0;
    for (const Variant& v : variants) {
        std::vector<Prepared> scenes;
        std::vector<SurfaceIds> ids;
        for (const Prepared& base : StudyScenes(v.W, v.H, v.FaceNormals)) {
            Scene s = base.SceneValue;
            s.Camera.Storage = v.Storage;
            scenes.push_back(Make(s, v.FaceNormals));
            ids.push_back(RasterizeSurfaceIds(s));
        }
        for (const double tolerance : tolerances)
            for (const double cosine : cosines) {
                HierarchySettings settings;
                settings.PlaneTolerance = tolerance;
                settings.NormalCos = cosine;
                settings.ShOrder = 0;
                size_t bridging = 0, singleFace = 0, covered = 0, planar = 0, ties = 0;
                size_t perLevelSingle[kMaxHierarchyLevels] = {}, perLevelCovered[kMaxHierarchyLevels] = {};
                for (size_t k = 0; k < scenes.size(); ++k) {
                    const DepthHierarchy h = Hierarchy(scenes[k], settings);
                    const DepthCamera camera = MakeDepthCamera(scenes[k].SceneValue.Camera);
                    const uint32_t w = scenes[k].Geometry.Width;
                    for (uint32_t l = 1; l < h.Levels.size(); ++l)
                        for (uint32_t y = 0; y < h.Levels[l].Height; ++y)
                            for (uint32_t x = 0; x < h.Levels[l].Width; ++x) {
                                CellTexels(h, l, x, y, texels);
                                // Ground truth: the footprint is one surface when the
                                // exact surface points of all its texels lie on the
                                // plane of one of their faces (a texel whose centre is
                                // on a crease line belongs to both faces).
                                bool oneId = true, allHit = true;
                                for (const size_t i : texels) {
                                    allHit = allHit && ids[k].Primitive[i] >= 0;
                                    oneId = oneId && ids[k].Primitive[i] == ids[k].Primitive[texels[0]] &&
                                            ids[k].Face[i] == ids[k].Face[texels[0]];
                                }
                                bool oneSurface = false;
                                if (allHit)
                                    for (const size_t f : texels) {
                                        Vec3 n;
                                        double d;
                                        FacePlane(scenes[k].SceneValue.Primitives[static_cast<size_t>(ids[k].Primitive[f])],
                                                  ids[k].Face[f], n, d);
                                        bool on = true;
                                        for (const size_t i : texels) {
                                            const double u = (i % w + 0.5) / w, vv = (static_cast<double>(i / w) + 0.5) / scenes[k].Geometry.Height;
                                            const Vec3 point = Scale(Vec3{camera.ViewX[0] * u + camera.ViewX[1] * vv + camera.ViewX[2],
                                                                          camera.ViewY[0] * u + camera.ViewY[1] * vv + camera.ViewY[2], -1.0},
                                                                     ids[k].Distance[i]);
                                            on = on && std::abs(Dot(n, point) - d) <= 1.0e-9 * std::sqrt(Dot(point, point));
                                        }
                                        if (on) {
                                            oneSurface = true;
                                            break;
                                        }
                                    }
                                const bool isPlanar = (h.Cell(l, x, y).Flags & kCellPlanar) != 0U;
                                if (oneId && allHit) ++singleFace, ++perLevelSingle[l];
                                if (oneId && allHit && isPlanar) ++covered, ++perLevelCovered[l];
                                if (isPlanar) ++planar;
                                if (isPlanar && !oneSurface) ++bridging;
                                if (isPlanar && oneSurface && !oneId) ++ties;
                            }
                }
                const bool chosen = tolerance == HierarchySettings{}.PlaneTolerance && cosine == HierarchySettings{}.NormalCos;
                if (chosen) defaultBridging += bridging;
                std::printf("DGI_M4 hierarchy_tolerance variant=%s plane_tol=%.0e normal_cos=%.4f planar_cells=%zu "
                            "bridging=%zu crease_tie_cells=%zu single_face_cells=%zu coverage=%.4f per_level_coverage",
                            v.Name, tolerance, cosine, planar, bridging, ties, singleFace,
                            singleFace > 0 ? static_cast<double>(covered) / singleFace : 0.0);
                for (uint32_t l = 1; l < HierarchySettings{}.Levels; ++l)
                    std::printf(" L%u=%.3f", l, perLevelSingle[l] > 0 ? static_cast<double>(perLevelCovered[l]) / perLevelSingle[l] : 0.0);
                std::printf("%s\n", chosen ? " (default)" : "");
            }
    }
    EXPECT_EQ(defaultBridging, 0U);
}

// ---------------------------------------------------------------------------
// The footprint level follows the sample's footprint, the resolution and the
// projection: it comes from the spacing of consecutive samples along the line
// in pixels of the actual image. The receivers below are the same 3D points
// in three images: 96 x 64, the same view at 288 x 192 (x3) and the same
// resolution zoomed x3 (tangents / 3). Every metric sample lands 3 times
// farther in pixels, so its requested level rises by log2(3) (1 or 2 levels)
// and its metric footprint 2^l x pixel size changes by 2/3 or 4/3 at most.

TEST(DiffuseGiM4, FootprintLevelFollowsResolutionAndProjection) {
    const double tan30 = std::tan(30.0 * kDegree);
    struct Config {
        const char* Name;
        uint32_t W, H;
        double TanHalfY, Pixel;
    };
    const Config configs[3] = {{"96x64", 96, 64, tan30, 1.0},
                               {"288x192", 288, 192, tan30, 1.0 / 3.0},
                               {"96x64_zoom3", 96, 64, tan30 / 3.0, 1.0 / 3.0}};
    std::vector<Prepared> prepared;
    std::vector<DepthHierarchy> hierarchies;
    HierarchySettings hs;
    hs.Levels = 8;
    for (const Config& c : configs) {
        Scene scene = CornerRoomScene(c.W, c.H);
        scene.Camera.TanHalfY = c.TanHalfY;
        prepared.push_back(Make(scene, true));
        hierarchies.push_back(Hierarchy(prepared.back(), hs));
    }
    GatherSettings g = RadialSettings(2.5, 0.5, 4, 16, 1.0);
    g.Schedule = CandidateSchedule::MetricRadial;
    g.HierarchyFootprint = true;
    g.HierarchyMaxLevel = 7;
    size_t matched[3] = {}, steps[3][4] = {};
    double worst[3] = {}, spacingRatio[3] = {};
    for (uint32_t y = 21; y <= 42; y += 3)
        for (uint32_t x = 32; x <= 63; x += 3) {
            const uint32_t px[3] = {x, 3 * x + 1, 3 * x - 95}, py[3] = {y, 3 * y + 1, 3 * y - 63};
            bool geometry = true;
            for (int c = 0; c < 3; ++c)
                geometry = geometry && prepared[c].Geometry.ViewDepth[static_cast<size_t>(py[c]) * configs[c].W + px[c]] > 0.0;
            if (!geometry) continue;
            for (int s = 0; s < 4; ++s) {
                const double phi = kPi * (s + 0.5) / 4.0;
                std::vector<ScheduledPosition> schedule[3];
                for (int c = 0; c < 3; ++c) {
                    GatherAcceleration acc;
                    acc.Hierarchy = &hierarchies[c];
                    schedule[c] = ScheduleSection(prepared[c].Input, prepared[c].Geometry, px[c], py[c], phi, g, acc);
                }
                for (const auto& a : schedule[0]) {
                    if (a.Spacing < 1.0 || a.Requested >= 7U) continue;
                    for (int c = 1; c < 3; ++c)
                        for (const auto& b : schedule[c]) {
                            if (b.Side != a.Side || std::abs(b.Lambda - 3.0 * a.Lambda) > 1.0e-6 * b.Lambda || b.Requested >= 7U)
                                continue;
                            ++matched[c];
                            const int step = static_cast<int>(b.Requested) - static_cast<int>(a.Requested);
                            ++steps[c][std::clamp(step, 0, 3)];
                            const double ratio = std::log2(std::ldexp(1.0, static_cast<int>(b.Requested)) * configs[c].Pixel /
                                                           std::ldexp(1.0, static_cast<int>(a.Requested)));
                            worst[c] = std::max(worst[c], std::abs(ratio));
                            spacingRatio[c] = std::max(spacingRatio[c], std::abs(b.Spacing / a.Spacing - 3.0));
                        }
                }
            }
        }
    for (int c = 1; c < 3; ++c) {
        std::printf("DGI_M4 footprint config=%s matched_samples=%zu level_step 0:%zu 1:%zu 2:%zu >2:%zu "
                    "max_abs_log2_metric_footprint_ratio=%.3f max_abs(spacing_ratio-3)=%.1e\n",
                    configs[c].Name, matched[c], steps[c][0], steps[c][1], steps[c][2], steps[c][3], worst[c],
                    spacingRatio[c]);
        EXPECT_GT(matched[c], 100U);
        EXPECT_EQ(steps[c][0] + steps[c][3], 0U);
        EXPECT_LE(worst[c], std::log2(1.5) + 1.0e-9);
    }
}

// ---------------------------------------------------------------------------
// Radial distribution (specification section 6): scene units, contact region.

TEST(DiffuseGiM4, MetricRadialAndHierarchyScaleWithSceneUnits) {
    // With its symmetric camera the corner room has texels whose centre lies
    // exactly on the floor-wall crease: their normal (depth reconstruction:
    // the tie rule; face normal: the first primitive hit) and their sources
    // on the one-sided boundary n . (P - q) = 0 are ill-conditioned decisions
    // that last-bit changes flip (first lines). The scale test proper uses an
    // off-centre principal point, so no crease passes through a texel centre.
    const Scene symmetric = CornerRoomScene(96, 64);
    Scene base = symmetric;
    base.Camera.ShiftX = 0.0031, base.Camera.ShiftY = -0.0017;
    {
        const Scene& sym = symmetric;
        GatherSettings g = RadialSettings(2.5, 0.5, 8, 16, 2.0);
        g.Schedule = CandidateSchedule::MetricRadial;
        g.RadialJitter = 0.37;
        for (const bool faceNormals : {false, true}) {
            const auto reference = GatherImage(Make(sym, faceNormals).Input, g);
            for (const double factor : {0.01, 100.0}) {
                GatherSettings s = g;
                s.Radius *= factor, s.Thickness *= factor;
                const auto image = GatherImage(Make(Scaled(sym, factor), faceNormals).Input, s);
                size_t flips = 0;
                for (size_t i = 0; i < image.size(); ++i)
                    if (std::abs(Luma(image[i].J) - Luma(reference[i].J)) > 1.0e-9 * std::max(1.0, Luma(reference[i].J))) ++flips;
                std::printf("DGI_M4 scale_units config=symmetric_camera_%s factor=%g pixels_beyond_1e-9=%zu/%zu "
                            "(crease-centre ties)\n",
                            faceNormals ? "face_normals" : "depth_normals", factor, flips, image.size());
            }
        }
    }
    for (const bool hierarchy : {false, true})
        for (const bool faceNormals : {false, true}) {
            GatherSettings g = RadialSettings(2.5, 0.5, 8, 16, 2.0);
            g.Schedule = CandidateSchedule::MetricRadial;
            g.RadialJitter = 0.37;
            g.HierarchyFootprint = hierarchy;
            g.Cache = hierarchy ? CacheMode::Mean : CacheMode::Off;
            ASSERT_STREQ(ValidateGatherSettings(g), "");
            const Prepared unit = Make(base, faceNormals);
            const DepthHierarchy unitHierarchy = Hierarchy(unit);
            GatherAcceleration unitAcc;
            unitAcc.Hierarchy = &unitHierarchy;
            const auto reference = GatherImage(unit.Input, g, unitAcc);
            for (const double factor : {0.01, 100.0}) {
                GatherSettings scaled = g;
                scaled.Radius *= factor;
                scaled.Thickness *= factor;
                const Prepared p = Make(Scaled(base, factor), faceNormals);
                const DepthHierarchy h = Hierarchy(p);
                GatherAcceleration acc;
                acc.Hierarchy = &h;
                const auto image = GatherImage(p.Input, scaled, acc);
                size_t flagDiff = 0;
                for (size_t i = 0; i < h.Cells.size(); ++i) flagDiff += h.Cells[i].Flags != unitHierarchy.Cells[i].Flags ? 1U : 0U;
                double maxJ = 0.0, maxA = 0.0;
                size_t counterMismatch = 0, flipsJ = 0;
                for (size_t i = 0; i < image.size(); ++i) {
                    const double dj = std::abs(Luma(image[i].J) - Luma(reference[i].J)) / std::max(1.0, Luma(reference[i].J));
                    maxJ = std::max(maxJ, dj);
                    if (dj > 1.0e-9) ++flipsJ;
                    maxA = std::max(maxA, std::abs(image[i].ARaw - reference[i].ARaw));
                    if (image[i].Counters.Scheduled != reference[i].Counters.Scheduled ||
                        image[i].Counters.Occupying != reference[i].Counters.Occupying ||
                        image[i].Counters.Contact != reference[i].Counters.Contact ||
                        image[i].Counters.Coarse != reference[i].Counters.Coarse)
                        ++counterMismatch;
                }
                std::printf("DGI_M4 scale_units config=%s_%s factor=%g max_abs_J/max(1,J)=%.2e J_pixels_beyond_1e-9=%zu "
                            "max_abs_A=%.2e counter_mismatch=%zu/%zu hierarchy_flag_diffs=%zu\n",
                            hierarchy ? "metric_hierarchy_cache" : "metric", faceNormals ? "face_normals" : "depth_normals",
                            factor, maxJ, flipsJ, maxA, counterMismatch, image.size(), flagDiff);
                // Depth-reconstructed source normals of edge texels may take the
                // other plane within the M2 tie tolerance after rescaling (the
                // one-sided test then flips); face normals are exact.
                if (faceNormals) EXPECT_LT(maxJ, 1.0e-9);
                else EXPECT_LE(flipsJ, image.size() / 200);
                EXPECT_LT(maxA, 1.0e-9);
                EXPECT_EQ(counterMismatch, 0U);
                EXPECT_EQ(flagDiff, 0U);
            }
        }
}

TEST(DiffuseGiM4, RadialJitterNeverSkipsTheContactRegion) {
    const Prepared p = Make(CornerRoomScene(96, 64));
    const GatherAcceleration none;
    struct Schedule {
        const char* Name;
        CandidateSchedule Kind;
        uint32_t K;
        double Exponent;
    };
    const Schedule schedules[] = {{"metric_K16_e2", CandidateSchedule::MetricRadial, 16, 2.0},
                                  {"metric_K8_e1", CandidateSchedule::MetricRadial, 8, 1.0},
                                  {"screen_K16_e1", CandidateSchedule::Radial, 16, 1.0},
                                  {"screen_K16_e2", CandidateSchedule::Radial, 16, 2.0},
                                  {"screen_K8_e1", CandidateSchedule::Radial, 8, 1.0}};
    for (const Schedule& schedule : schedules) {
        GatherSettings g = RadialSettings(2.5, 0.5, 4, schedule.K, schedule.Exponent);
        g.Schedule = schedule.Kind;
        size_t sides = 0, never = 0, tiny = 0;
        double fraction = 0.0;
        for (uint32_t y = 1; y < 64; y += 3)
            for (uint32_t x = 1; x < 96; x += 3) {
                if (!(p.Geometry.ViewDepth[static_cast<size_t>(y) * 96 + x] > 0.0)) continue;
                for (int s = 0; s < 4; ++s) {
                    const double phi = kPi * (s + 0.5) / 4.0;
                    const SectionSetup setup = SetupSection(p.Input, p.Geometry, x, y, phi, g);
                    if (!setup.Valid) continue;
                    for (const int side : {1, -1}) {
                        const double dx = setup.DirX * side, dy = setup.DirY * side;
                        const double tx = std::abs(dx) > 0.0 ? 0.5 / std::abs(dx) : 1e300;
                        const double ty = std::abs(dy) > 0.0 ? 0.5 / std::abs(dy) : 1e300;
                        const int sx = dx > 0.0 ? 1 : (dx < 0.0 ? -1 : 0), sy = dy > 0.0 ? 1 : (dy < 0.0 ? -1 : 0);
                        const int cx = tx <= ty ? sx : 0, cy = ty <= tx ? sy : 0;
                        if (setup.Extent[side > 0 ? 0 : 1] <= std::min(2.0 * tx, 2.0 * ty)) {
                            ++tiny; // the R-ball projects inside the contact texel
                            continue;
                        }
                        int hits = 0;
                        for (int j = 0; j < 16; ++j) {
                            g.RadialJitter = (j + 0.5) / 16.0;
                            bool found = false;
                            for (const auto& position : ScheduleSection(p.Input, p.Geometry, x, y, phi, g, none))
                                found = found || (position.Side == side && position.CellX == cx && position.CellY == cy);
                            hits += found ? 1 : 0;
                        }
                        ++sides;
                        if (hits == 0) ++never;
                        fraction += hits / 16.0;
                    }
                }
            }
        std::printf("DGI_M4 contact schedule=%s sides=%zu never_sampled=%zu mean_fraction_of_jitters=%.3f "
                    "ball_inside_contact_texel=%zu\n",
                    schedule.Name, sides, never, fraction / std::max<size_t>(sides, 1), tiny);
        if (schedule.Kind == CandidateSchedule::MetricRadial) EXPECT_EQ(never, 0U);
    }
}

TEST(DiffuseGiM4, RadialDistributionStudy) {
    std::vector<Prepared> scenes;
    for (const Scene& s : {CornerRoomScene(96, 64), PillarsScene(96, 64), CheckerRoomScene(96, 64)}) scenes.push_back(Make(s));
    const double radii[] = {2.5, 3.0, 2.5};
    for (size_t k = 0; k < scenes.size(); ++k) {
        const auto reference = GatherImage(scenes[k].Input, EverySettings(radii[k], 0.5, 8));
        uint64_t referenceScheduled = SumScheduled(reference);
        for (const CandidateSchedule kind : {CandidateSchedule::Radial, CandidateSchedule::MetricRadial})
            for (const uint32_t count : {8U, 16U, 32U})
                for (const double exponent : {1.0, 2.0, 3.0}) {
                    GatherSettings g = RadialSettings(radii[k], 0.5, 8, count, exponent);
                    g.Schedule = kind;
                    std::vector<PixelGather> mean = reference;
                    std::vector<double> sum(reference.size(), 0.0), sum2(reference.size(), 0.0), sumA(reference.size(), 0.0);
                    uint64_t scheduled = 0;
                    constexpr int kJitters = 4;
                    for (int j = 0; j < kJitters; ++j) {
                        g.RadialJitter = (j + 0.5) / kJitters;
                        const auto image = GatherImage(scenes[k].Input, g);
                        scheduled += SumScheduled(image);
                        for (size_t i = 0; i < image.size(); ++i) {
                            const double l = Luma(image[i].J);
                            sum[i] += l, sum2[i] += l * l, sumA[i] += image[i].ARaw;
                        }
                    }
                    double noise = 0.0;
                    size_t lit = 0;
                    for (size_t i = 0; i < mean.size(); ++i) {
                        const double m = sum[i] / kJitters;
                        mean[i].J = {m / 0.2126 / 3.0, m / 0.7152 / 3.0, m / 0.0722 / 3.0}; // luminance m
                        mean[i].ARaw = sumA[i] / kJitters;
                        if (reference[i].Valid && Luma(reference[i].J) > 0.02) {
                            noise += std::sqrt(std::max(0.0, sum2[i] / kJitters - m * m)) / Luma(reference[i].J);
                            ++lit;
                        }
                    }
                    const ImageDiff d = Compare(mean, reference);
                    std::printf("DGI_M4 radial_study scene=%s schedule=%s K=%u e=%.0f bias_J=%+.4f mean_abs_rel_J=%.4f "
                                "p95_abs_J=%.4f bias_A=%+.5f mean_abs_A=%.5f jitter_noise_rel=%.4f scheduled_ratio=%.3f\n",
                                scenes[k].SceneValue.Name.c_str(), kind == CandidateSchedule::Radial ? "screen" : "metric",
                                count, exponent, d.MeanBiasJ(), d.RelMeanAbsJ(), d.P95AbsJ, d.MeanAa - d.MeanAb, d.MeanAbsA,
                                noise / std::max<size_t>(lit, 1),
                                static_cast<double>(scheduled) / kJitters / static_cast<double>(referenceScheduled));
                }
    }
}

// ---------------------------------------------------------------------------
// The hierarchy footprint: a sparse sample on a planar cell occupies the strip
// of its footprint along the line, closing the gaps sparse point samples leave
// on continuous surfaces. Study of the spacing scale and the deepest level
// against EveryTexel (S = 8, jitter mean) and, on receivers of two scenes,
// against the 3D ground truth of the finite-radius model.

TEST(DiffuseGiM4, HierarchyFootprintStudy) {
    struct Schedule {
        const char* Name;
        CandidateSchedule Kind;
        uint32_t K;
        double Exponent;
    };
    const Schedule schedules[] = {{"screen_K16_e2", CandidateSchedule::Radial, 16, 2.0},
                                  {"metric_K16_e1", CandidateSchedule::MetricRadial, 16, 1.0},
                                  {"metric_K16_e2", CandidateSchedule::MetricRadial, 16, 2.0},
                                  {"metric_K8_e1", CandidateSchedule::MetricRadial, 8, 1.0}};
    struct Variant {
        const char* Name;
        bool Footprint;
        double Scale;
        uint32_t MaxLevel;
    };
    const Variant variants[] = {{"off", false, 1.0, 4},   {"k1_L2", true, 1.0, 2}, {"k1_L4", true, 1.0, 4},
                                {"k1_L6", true, 1.0, 6}, {"k2_L2", true, 2.0, 2}, {"k2_L4", true, 2.0, 4},
                                {"k2_L6", true, 2.0, 6}};
    HierarchySettings hs;
    hs.Levels = 7;
    struct StudyScene {
        Scene Value;
        double Radius, Thickness;
    };
    const StudyScene scenes[] = {{CornerRoomScene(96, 64), 2.5, 0.5},
                                 {PillarsScene(96, 64), 3.0, 0.5},
                                 {CheckerRoomScene(96, 64), 2.5, 0.5},
                                 {BlackOccluderScene(96, 64, true), 5.0, 1.0}};
    for (const StudyScene& sc : scenes) {
        const Prepared p = Make(sc.Value);
        const DepthHierarchy h = Hierarchy(p, hs);
        GatherAcceleration acc;
        acc.Hierarchy = &h;
        const auto reference = GatherImage(p.Input, EverySettings(sc.Radius, sc.Thickness, 8));
        const uint64_t referenceScheduled = SumScheduled(reference);
        for (const Schedule& schedule : schedules)
            for (const Variant& v : variants) {
                GatherSettings g = RadialSettings(sc.Radius, sc.Thickness, 8, schedule.K, schedule.Exponent);
                g.Schedule = schedule.Kind;
                g.HierarchyFootprint = v.Footprint;
                g.HierarchySpacingScale = v.Scale;
                g.HierarchyMaxLevel = v.MaxLevel;
                constexpr int kJitters = 4;
                std::vector<std::vector<PixelGather>> frames;
                uint64_t coarse = 0, scheduled = 0;
                for (int j = 0; j < kJitters; ++j) {
                    g.RadialJitter = (j + 0.5) / kJitters;
                    frames.push_back(GatherImage(p.Input, g, acc));
                    coarse += SumCoarse(frames.back());
                    scheduled += SumScheduled(frames.back());
                }
                std::vector<PixelGather> mean = frames.front();
                for (size_t i = 0; i < mean.size(); ++i) {
                    Rgb j;
                    double a = 0.0;
                    for (const auto& f : frames) j.R += f[i].J.R, j.G += f[i].J.G, j.B += f[i].J.B, a += f[i].ARaw;
                    mean[i].J = {j.R / kJitters, j.G / kJitters, j.B / kJitters};
                    mean[i].ARaw = a / kJitters;
                }
                const ImageDiff d = Compare(mean, reference);
                std::printf("DGI_M4 footprint_study scene=%s schedule=%s variant=%s bias_J=%+.4f mean_abs_rel_J=%.4f "
                            "bias_A=%+.5f mean_abs_A=%.5f coarse_fraction=%.3f candidates_vs_every_texel=%.3f\n",
                            sc.Value.Name.c_str(), schedule.Name, v.Name, d.MeanBiasJ(), d.RelMeanAbsJ(),
                            d.MeanAa - d.MeanAb, d.MeanAbsA, static_cast<double>(coarse) / std::max<uint64_t>(scheduled, 1),
                            static_cast<double>(scheduled) / kJitters / static_cast<double>(referenceScheduled));
            }
    }
    // 3D ground truth (the screen reference with continuous intervals, S = 32).
    for (const StudyScene* sc : {&scenes[0], &scenes[3]}) {
        const Prepared p = Make(sc->Value);
        const DepthHierarchy h = Hierarchy(p, hs);
        GatherAcceleration acc;
        acc.Hierarchy = &h;
        struct Config {
            const char* Name;
            GatherSettings Settings;
        };
        std::vector<Config> configs;
        GatherSettings every = EverySettings(sc->Radius, sc->Thickness, 32);
        every.Mask = MaskMode::ContinuousIntervals;
        configs.push_back({"every_texel_continuous", every});
        configs.push_back({"screen_K16_e2", RadialSettings(sc->Radius, sc->Thickness, 32, 16, 2.0)});
        GatherSettings metric = RadialSettings(sc->Radius, sc->Thickness, 32, 16, 1.0);
        metric.Schedule = CandidateSchedule::MetricRadial;
        configs.push_back({"metric_K16_e1", metric});
        GatherSettings footprint = metric;
        footprint.HierarchyFootprint = true;
        footprint.HierarchySpacingScale = 2.0;
        configs.push_back({"metric_K16_e1_footprint_k2_L4", footprint});
        double errJ[4] = {}, errA[4] = {}, biasJ[4] = {};
        size_t receivers = 0;
        for (uint32_t y = 34; y < 64; y += 6)
            for (uint32_t x = 8; x < 96; x += 16) {
                if (!(p.Geometry.ViewDepth[static_cast<size_t>(y) * 96 + x] > 0.0)) continue;
                const GroundTruth truth = IntegrateGroundTruth(sc->Value, p.Input, p.Geometry, x, y, sc->Radius, 1.0e-5);
                if (!truth.Converged) continue;
                ++receivers;
                for (size_t c = 0; c < configs.size(); ++c) {
                    const PixelGather r = GatherPixel(p.Input, p.Geometry, x, y, configs[c].Settings, acc);
                    errJ[c] += std::abs(Luma(r.J) - Luma(truth.J));
                    biasJ[c] += Luma(r.J) - Luma(truth.J);
                    errA[c] += std::abs(r.ARaw - truth.A);
                }
            }
        for (size_t c = 0; c < configs.size(); ++c)
            std::printf("DGI_M4 footprint_truth scene=%s config=%s receivers=%zu mean_abs_J=%.4f bias_J=%+.4f mean_abs_A=%.4f\n",
                        sc->Value.Name.c_str(), configs[c].Name, receivers, errJ[c] / std::max<size_t>(receivers, 1),
                        biasJ[c] / std::max<size_t>(receivers, 1), errA[c] / std::max<size_t>(receivers, 1));
    }
}

// ---------------------------------------------------------------------------
// Radiance cache (specification section 5).

TEST(DiffuseGiM4, CacheNeverMergesANearDarkObjectWithAFarBrightBackground) {
    // T06: black box near the camera in front of the bright wall (4, 3, 2).
    const Prepared p = Make(BlackOccluderScene(96, 64, true));
    const SurfaceIds ids = RasterizeSurfaceIds(p.SceneValue);
    const DepthHierarchy h = Hierarchy(p);
    GatherAcceleration acc;
    acc.Hierarchy = &h;
    GatherSettings g = RadialSettings(5.0, 1.0, 8, 16, 2.0);
    g.HierarchyFootprint = true;
    g.Cache = CacheMode::Mean;
    ASSERT_STREQ(ValidateGatherAcceleration(p.Input, g, acc), "");
    std::vector<size_t> texels;
    size_t coarse = 0, onBox = 0, mixed = 0, radianceMismatch = 0;
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 96; ++x)
            for (int s = 0; s < 8; ++s) {
                const auto built = BuildSection(p.Input, p.Geometry, x, y, kPi * (s + 0.5) / 8.0, g, acc);
                for (const Candidate& c : built.Ordered) {
                    if (c.Level == 0U) continue;
                    ++coarse;
                    CellTexels(h, c.Level, static_cast<uint32_t>(c.HX), static_cast<uint32_t>(c.HY), texels);
                    std::set<int32_t> primitives;
                    for (const size_t i : texels) primitives.insert(ids.Primitive[i]);
                    if (primitives.size() != 1U) ++mixed;
                    const int32_t primitive = *primitives.begin();
                    if (primitive == 2) ++onBox;
                    // Uniform faces: the cache equals the face's radiance unless back-facing (zero).
                    const Rgb face = p.SceneValue.Primitives[static_cast<size_t>(primitive)].Radiance;
                    const bool zero = c.Radiance.R == 0.0 && c.Radiance.G == 0.0 && c.Radiance.B == 0.0;
                    if (!zero && (std::abs(c.Radiance.R - face.R) > 1e-12 || std::abs(c.Radiance.G - face.G) > 1e-12 ||
                                  std::abs(c.Radiance.B - face.B) > 1e-12))
                        ++radianceMismatch;
                }
            }
    GatherSettings plain = RadialSettings(5.0, 1.0, 8, 16, 2.0);
    const ImageDiff d = Compare(GatherImage(p.Input, g, acc), GatherImage(p.Input, plain));
    std::printf("DGI_M4 cache_silhouette coarse_candidates=%zu on_black_box=%zu cells_mixing_primitives=%zu "
                "radiance_not_of_the_face=%zu | image vs point reference: mean_J=%.5f/%.5f mean_abs_J=%.2e max_abs_J=%.2e "
                "mean_abs_A=%.2e\n",
                coarse, onBox, mixed, radianceMismatch, d.MeanJa, d.MeanJb, d.MeanAbsJ, d.MaxAbsJ, d.MeanAbsA);
    EXPECT_GT(coarse, 1000U);
    EXPECT_GT(onBox, 0U);
    EXPECT_EQ(mixed, 0U);
    EXPECT_EQ(radianceMismatch, 0U);
}

TEST(DiffuseGiM4, CacheReducesSparseSamplingAliasingOnTexturedSources) {
    const Prepared p = Make(CheckerRoomScene(96, 64), true);
    const DepthHierarchy h = Hierarchy(p);
    GatherAcceleration acc;
    acc.Hierarchy = &h;
    const auto reference = GatherImage(p.Input, EverySettings(2.5, 0.5, 8));
    struct Config {
        const char* Name;
        CandidateSchedule Kind;
        bool Depth;
        CacheMode Cache;
    };
    const Config configs[] = {{"screen_off", CandidateSchedule::Radial, false, CacheMode::Off},
                              {"screen_cache", CandidateSchedule::Radial, false, CacheMode::Mean},
                              {"screen_hierarchy", CandidateSchedule::Radial, true, CacheMode::Off},
                              {"screen_hierarchy_cache", CandidateSchedule::Radial, true, CacheMode::Mean},
                              {"metric_off", CandidateSchedule::MetricRadial, false, CacheMode::Off},
                              {"metric_cache", CandidateSchedule::MetricRadial, false, CacheMode::Mean},
                              {"metric_hierarchy_cache", CandidateSchedule::MetricRadial, true, CacheMode::Mean}};
    double offNoise = 0.0, cacheNoise = 0.0;
    for (const Config& c : configs) {
        GatherSettings g = RadialSettings(2.5, 0.5, 8, 16, 2.0);
        g.Schedule = c.Kind;
        g.HierarchyFootprint = c.Depth;
        g.Cache = c.Cache;
        constexpr int kJitters = 16;
        std::vector<double> sum(reference.size(), 0.0), sum2(reference.size(), 0.0), sumA(reference.size(), 0.0);
        uint64_t coarse = 0, scheduled = 0;
        for (int j = 0; j < kJitters; ++j) {
            g.RadialJitter = (j + 0.5) / kJitters;
            const auto image = GatherImage(p.Input, g, acc);
            coarse += SumCoarse(image);
            scheduled += SumScheduled(image);
            for (size_t i = 0; i < image.size(); ++i) {
                const double l = Luma(image[i].J);
                sum[i] += l, sum2[i] += l * l, sumA[i] += image[i].ARaw;
            }
        }
        std::vector<PixelGather> mean = reference;
        double noise = 0.0;
        size_t lit = 0;
        for (size_t i = 0; i < mean.size(); ++i) {
            const double m = sum[i] / kJitters;
            mean[i].J = {m / 0.2126 / 3.0, m / 0.7152 / 3.0, m / 0.0722 / 3.0};
            mean[i].ARaw = sumA[i] / kJitters;
            if (reference[i].Valid && Luma(reference[i].J) > 0.02) {
                noise += std::sqrt(std::max(0.0, sum2[i] / kJitters - m * m)) / Luma(reference[i].J);
                ++lit;
            }
        }
        noise /= std::max<size_t>(lit, 1);
        const ImageDiff d = Compare(mean, reference);
        std::printf("DGI_M4 cache_checker config=%s coarse_fraction=%.3f jitter_noise_rel=%.4f | jitter mean vs EveryTexel: "
                    "bias_J=%+.4f mean_abs_rel_J=%.4f bias_A=%+.5f\n",
                    c.Name, static_cast<double>(coarse) / std::max<uint64_t>(scheduled, 1), noise, d.MeanBiasJ(),
                    d.RelMeanAbsJ(), d.MeanAa - d.MeanAb);
        if (std::string(c.Name) == "screen_off") offNoise = noise;
        if (std::string(c.Name) == "screen_cache") cacheNoise = noise;
    }
    EXPECT_LT(cacheNoise, offNoise);
}

// ---------------------------------------------------------------------------
// SH backend (specification section 5; [B5] eqs. 3, 5, 6, 8, 9).

namespace {

std::vector<Vec3> SphereDirections(size_t n) {
    std::vector<Vec3> out;
    for (size_t i = 0; i < n; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / n;
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double a = 2.399963229728653 * static_cast<double>(i);
        out.push_back({r * std::cos(a), r * std::sin(a), z});
    }
    return out;
}

// Integral over the sphere of f(omega) with Gauss-Legendre in z = cos(theta)
// on [z0, z1] and the midpoint rule in phi (exact for polynomials of the
// matching degrees).
template <typename F>
double SphereIntegral(F&& f, uint32_t nz, uint32_t nphi, double z0 = -1.0, double z1 = 1.0) {
    const GaussLegendreRule& rule = GaussLegendre(nz);
    double sum = 0.0;
    for (uint32_t i = 0; i < nz; ++i) {
        const double z = 0.5 * (z1 - z0) * rule.Nodes[i] + 0.5 * (z1 + z0);
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        for (uint32_t k = 0; k < nphi; ++k) {
            const double phi = 2.0 * kPi * (k + 0.5) / nphi;
            sum += 0.5 * (z1 - z0) * rule.Weights[i] * (2.0 * kPi / nphi) * f(Vec3{r * std::cos(phi), r * std::sin(phi), z});
        }
    }
    return sum;
}

} // namespace

TEST(DiffuseGiM4, ShBasisAndClampedCosineOperatorFromB5) {
    // (a) Closed forms (production) against the general basis, and [B5] eq. 3.
    double closed = 0.0;
    for (const Vec3& d : SphereDirections(2000)) {
        double general[9], production[9];
        ShBasis(d, 2, general);
        ShBasisOrder2<double>(d.X, d.Y, d.Z, production);
        for (int k = 0; k < 9; ++k) closed = std::max(closed, std::abs(general[k] - production[k]));
    }
    const double printed[5] = {0.282095, 0.488603, 1.092548, 0.315392, 0.546274};
    const double exact[5] = {0.28209479177387814, 0.4886025119029199, 1.0925484305920792, 0.31539156525252005,
                             0.5462742152960396};
    double eq3 = 0.0;
    for (int k = 0; k < 5; ++k) eq3 = std::max(eq3, std::abs(printed[k] - exact[k]));
    // (b) Orthonormality up to order 8 (exact quadrature for degree <= 16).
    constexpr uint32_t kOrder = 8;
    double gram = 0.0;
    for (uint32_t a = 0; a < ShCount(kOrder); ++a)
        for (uint32_t b = a; b < ShCount(kOrder); ++b) {
            const double value = SphereIntegral(
                [&](const Vec3& w) {
                    double y[81];
                    ShBasis(w, kOrder, y);
                    return y[a] * y[b];
                },
                16, 40);
            gram = std::max(gram, std::abs(value - (a == b ? 1.0 : 0.0)));
        }
    // (c) A_hat_l = sqrt(4 pi / (2l + 1)) integral max(0, z) Y_l0 ([B5] eqs. 6, 8, 9).
    double coefficient = 0.0, eq9 = 0.0;
    const double printed9[7] = {3.141593, 2.094395, 0.785398, 0.0, -0.130900, 0.0, 0.049087};
    for (uint32_t l = 0; l <= kOrder; ++l) {
        const double value = std::sqrt(4.0 * kPi / (2.0 * l + 1.0)) *
                             SphereIntegral(
                                 [&](const Vec3& w) {
                                     double y[81];
                                     ShBasis(w, kOrder, y);
                                     return w.Z * y[l * l + l];
                                 },
                                 16, 8, 0.0, 1.0);
        coefficient = std::max(coefficient, std::abs(value - ClampedCosineCoefficient(l)));
        if (l < 7U) eq9 = std::max(eq9, std::abs(ClampedCosineCoefficient(l) - printed9[l]));
    }
    // (d) Projection operator (Funk-Hecke): integral max(0, n . w) Y_lm(w) = A_hat_l Y_lm(n),
    // integrated over the front hemisphere of n in a frame aligned with n.
    double operatorError = 0.0;
    Rng rng{7};
    for (int trial = 0; trial < 12; ++trial) {
        const double z = 2.0 * rng.Uniform() - 1.0, a = 2.0 * kPi * rng.Uniform();
        const Vec3 n{std::sqrt(1.0 - z * z) * std::cos(a), std::sqrt(1.0 - z * z) * std::sin(a), z};
        const SectionFrame frame = SectionFrame::FromView(n);
        std::vector<double> projected(ShCount(4), 0.0);
        const GaussLegendreRule& rule = GaussLegendre(16);
        for (uint32_t i = 0; i < 16; ++i) {
            const double c = 0.5 * rule.Nodes[i] + 0.5; // cos(alpha) in [0, 1]
            const double s = std::sqrt(1.0 - c * c);
            for (uint32_t k = 0; k < 32; ++k) {
                const double b = 2.0 * kPi * (k + 0.5) / 32.0;
                const Vec3 w = Add(Scale(n, c), Add(Scale(frame.E1, s * std::cos(b)), Scale(frame.E2, s * std::sin(b))));
                double y[25];
                ShBasis(w, 4, y);
                for (uint32_t e = 0; e < ShCount(4); ++e) projected[e] += 0.5 * rule.Weights[i] * (2.0 * kPi / 32.0) * c * y[e];
            }
        }
        double yn[25];
        ShBasis(n, 4, yn);
        for (uint32_t l = 0; l <= 4; ++l)
            for (uint32_t e = l * l; e < (l + 1U) * (l + 1U); ++e)
                operatorError = std::max(operatorError, std::abs(projected[e] - ClampedCosineCoefficient(l) * yn[e]));
    }
    std::printf("DGI_M4 sh_basis closed_forms_vs_general_max_abs=%.2e B5_eq3_printed_vs_exact_max_abs=%.2e "
                "orthonormality_order8_max_abs=%.2e | A_hat_l_vs_quadrature_l<=8_max_abs=%.2e B5_eq9_printed_max_abs=%.2e "
                "| funk_hecke_operator_l<=4_max_abs=%.2e\n",
                closed, eq3, gram, coefficient, eq9, operatorError);
    EXPECT_LT(closed, 1.0e-14);
    EXPECT_LT(eq3, 5.0e-7);
    EXPECT_LT(gram, 1.0e-12);
    EXPECT_LT(coefficient, 1.0e-12);
    EXPECT_LT(eq9, 5.0e-7);
    EXPECT_LT(operatorError, 1.0e-12);
}

TEST(DiffuseGiM4, ShCacheEqualsThePointReferenceOnPlanarCells) {
    // worst[normal source][uniform radiance or textured][precision]
    double worst[2][2][2] = {}, imageMax64 = 0.0, imageMax32 = 0.0;
    size_t cells = 0, evaluations = 0;
    const auto directions = SphereDirections(48);
    std::vector<size_t> texels;
    for (bool faceNormals : {false, true})
        for (const Scene& scene : {CornerRoomScene(96, 64), CheckerRoomScene(96, 64), PillarsScene(96, 64)}) {
            const Prepared p = Make(scene, faceNormals);
            for (const uint32_t order : {1U, 2U}) {
                HierarchySettings hs;
                hs.ShOrder = order;
                const DepthHierarchy h64 = Hierarchy(p, hs), h32 = Hierarchy(p, hs, true);
                for (const DepthHierarchy* h : {&h64, &h32})
                    for (uint32_t l = 0; l < h->Levels.size(); ++l)
                        for (uint32_t y = 0; y < h->Levels[l].Height; ++y)
                            for (uint32_t x = 0; x < h->Levels[l].Width; ++x) {
                                const HierarchyCell& c = h->Cell(l, x, y);
                                if ((c.Flags & kCellPlanar) == 0U || (l == 0U && (x + y) % 7U != 0U)) continue;
                                ++cells;
                                CellTexels(*h, l, x, y, texels);
                                bool uniform = true;
                                for (const size_t i : texels) {
                                    const size_t a = 3 * ((static_cast<size_t>(p.Input.Viewport.Y) + i / 96) * p.Input.BufferWidth +
                                                          p.Input.Viewport.X + i % 96);
                                    const size_t b = 3 * ((static_cast<size_t>(p.Input.Viewport.Y) + texels[0] / 96) *
                                                              p.Input.BufferWidth + p.Input.Viewport.X + texels[0] % 96);
                                    uniform = uniform && p.Input.Radiance[a] == p.Input.Radiance[b] &&
                                              p.Input.Radiance[a + 1] == p.Input.Radiance[b + 1] &&
                                              p.Input.Radiance[a + 2] == p.Input.Radiance[b + 2];
                                }
                                const double scale = std::max({c.Mean.R, c.Mean.G, c.Mean.B, 1e-6});
                                for (const Vec3& w : directions) {
                                    if (!(Dot(w, c.Normal) > 0.01)) continue; // the side test is separate
                                    ++evaluations;
                                    const Rgb v = h->Float32 ? EvaluateShRadianceOrder2<float>(h->CellSh(l, x, y), order,
                                                                                                 static_cast<float>(w.X),
                                                                                                 static_cast<float>(w.Y),
                                                                                                 static_cast<float>(w.Z))
                                                             : EvaluateShRadiance(h->CellSh(l, x, y), order, w);
                                    const double e = std::max({std::abs(v.R - c.Mean.R), std::abs(v.G - c.Mean.G),
                                                               std::abs(v.B - c.Mean.B)}) / scale;
                                    double& slot = worst[faceNormals ? 1 : 0][uniform ? 0 : 1][h->Float32 ? 1 : 0];
                                    slot = std::max(slot, e);
                                }
                            }
                // The gather with the SH cache equals the gather with the mean cache.
                GatherSettings g = RadialSettings(2.5, 0.5, 8, 16, 2.0);
                g.HierarchyFootprint = true;
                for (int precision = 0; precision < 2; ++precision) {
                    GatherAcceleration acc;
                    acc.Hierarchy = precision == 0 ? &h64 : &h32;
                    g.Cache = CacheMode::Mean;
                    const auto mean = precision == 0 ? GatherImage(p.Input, g, acc) : GatherImageF32(p.Input, g, acc);
                    g.Cache = CacheMode::SphericalHarmonics;
                    const auto sh = precision == 0 ? GatherImage(p.Input, g, acc) : GatherImageF32(p.Input, g, acc);
                    const ImageDiff d = Compare(sh, mean);
                    (precision == 0 ? imageMax64 : imageMax32) =
                        std::max(precision == 0 ? imageMax64 : imageMax32, d.MaxAbsJ / std::max(d.MeanJb, 1e-6));
                }
            }
        }
    std::printf("DGI_M4 sh_planar cells=%zu evaluations=%zu max_rel_vs_mean (f64/f32): depth_normals uniform=%.1e/%.1e "
                "textured=%.1e/%.1e | face_normals uniform=%.1e/%.1e textured=%.1e/%.1e | gather SH vs mean cache "
                "max_abs_J/mean_J f64=%.2e f32=%.2e\n",
                cells, evaluations, worst[0][0][0], worst[0][0][1], worst[0][1][0], worst[0][1][1], worst[1][0][0],
                worst[1][0][1], worst[1][1][0], worst[1][1][1], imageMax64, imageMax32);
    // Declared tolerances: a uniform planar cell is exact up to rounding; a
    // textured one deviates by (radiance contrast) x (spread of the clamped
    // cosines of its texels' normals), which the planarity bound keeps small.
    for (int n = 0; n < 2; ++n) {
        EXPECT_LT(worst[n][0][0], 1.0e-12);
        EXPECT_LT(worst[n][0][1], 1.0e-5);
        EXPECT_LT(worst[n][1][0], 1.0e-4);
        EXPECT_LT(worst[n][1][1], 1.0e-3);
    }
    EXPECT_LT(imageMax64, 1.0e-5);
    EXPECT_LT(imageMax32, 1.0e-4);
}

TEST(DiffuseGiM4, ShCreaseStudyRingingDirectionalityAndChroma) {
    const Rgb red{1.0, 0.1, 0.05}, green{0.05, 0.8, 0.1};
    const auto directions = SphereDirections(8192);
    struct Metrics {
        double MaxError = 0.0, MeanError = 0.0, Overshoot = 0.0, Contamination = 0.0, MeanContamination = 0.0;
        double AllFacingMax = 0.0;  // directions every texel faces (n . w > 0.05): the smooth part
        double WeightedMean = 0.0;  // mean error weighted by the exact projected area D(w)
        size_t LostSide = 0, Count = 0, OneSided = 0;
    };
    const auto measure = [&](const std::vector<Rgb>& radiance, const std::vector<Vec3>& normals, uint32_t order,
                             const std::vector<double>& coefficients, bool twoFaces, const Vec3& nA, const Vec3& nB) {
        Metrics m;
        double hi[3] = {0.0, 0.0, 0.0}, lo[3] = {1e300, 1e300, 1e300};
        for (const Rgb& r : radiance) {
            hi[0] = std::max(hi[0], r.R), hi[1] = std::max(hi[1], r.G), hi[2] = std::max(hi[2], r.B);
            lo[0] = std::min(lo[0], r.R), lo[1] = std::min(lo[1], r.G), lo[2] = std::min(lo[2], r.B);
        }
        const double scale = std::max({hi[0], hi[1], hi[2]});
        double weights = 0.0;
        for (const Vec3& w : directions) {
            double d = 0.0, facing = 1.0;
            for (const Vec3& n : normals) d += std::max(0.0, Dot(n, w)), facing = std::min(facing, Dot(n, w));
            if (!(d > 1e-9)) continue;
            const Rgb exact = ExactFootprintRadiance(radiance, normals, w);
            const Rgb sh = EvaluateShRadiance(coefficients.data(), order, w);
            ++m.Count;
            if (sh.R == 0.0 && sh.G == 0.0 && sh.B == 0.0) {
                ++m.LostSide; // D_SH <= 0 where the exact D > 0
                continue;
            }
            const double values[3] = {sh.R, sh.G, sh.B}, truth[3] = {exact.R, exact.G, exact.B};
            double e = 0.0;
            for (int c = 0; c < 3; ++c) {
                e = std::max(e, std::abs(values[c] - truth[c]) / scale);
                m.Overshoot = std::max({m.Overshoot, (values[c] - hi[c]) / scale, (lo[c] - values[c]) / scale});
            }
            m.MaxError = std::max(m.MaxError, e);
            m.MeanError += e;
            m.WeightedMean += e * d;
            weights += d;
            if (facing > 0.05) m.AllFacingMax = std::max(m.AllFacingMax, e);
            if (twoFaces) {
                const bool onlyA = Dot(nA, w) > 0.0 && Dot(nB, w) <= 0.0;
                const bool onlyB = Dot(nB, w) > 0.0 && Dot(nA, w) <= 0.0;
                if (onlyA || onlyB) {
                    // Fraction of the other face's colour in a direction seen by one face only.
                    const double c = onlyA ? (sh.G - red.G) / (green.G - red.G) : (sh.R - green.R) / (red.R - green.R);
                    m.Contamination = std::max(m.Contamination, std::abs(c));
                    m.MeanContamination += std::abs(c);
                    ++m.OneSided;
                }
            }
        }
        m.MeanError /= std::max<size_t>(m.Count - m.LostSide, 1);
        m.MeanContamination /= std::max<size_t>(m.OneSided, 1);
        m.WeightedMean /= std::max(weights, 1e-300);
        return m;
    };
    double order2At5 = 0.0, order8At90 = 1.0, order2At90 = 0.0;
    for (const double delta : {0.0, 2.0, 5.0, 10.0, 20.0, 45.0, 90.0, 135.0}) {
        const double h = 0.5 * delta * kDegree;
        const Vec3 nA{std::sin(h), 0.0, std::cos(h)}, nB{-std::sin(h), 0.0, std::cos(h)};
        std::vector<Rgb> radiance;
        std::vector<Vec3> normals;
        for (int k = 0; k < 8; ++k) radiance.push_back(red), normals.push_back(nA);
        for (int k = 0; k < 8; ++k) radiance.push_back(green), normals.push_back(nB);
        for (const uint32_t order : {1U, 2U, 4U, 8U}) {
            std::vector<double> coefficients(4U * ShCount(order), 0.0);
            for (size_t i = 0; i < radiance.size(); ++i) ProjectSourceTexel(radiance[i], normals[i], order, coefficients.data());
            const Metrics m = measure(radiance, normals, order, coefficients, delta > 0.0, nA, nB);
            std::printf("DGI_M4 sh_crease delta_deg=%.0f order=%u directions=%zu max_rel_error=%.4f mean=%.4f "
                        "area_weighted_mean=%.4f all_facing_max=%.4f overshoot=%.4f lost_side=%zu one_face_directions=%zu "
                        "contamination_max=%.4f mean=%.4f\n",
                        delta, order, m.Count, m.MaxError, m.MeanError, m.WeightedMean, m.AllFacingMax, m.Overshoot,
                        m.LostSide, m.OneSided, m.Contamination, m.MeanContamination);
            if (delta == 0.0) {
                EXPECT_LT(m.MaxError, 1.0e-12); // one plane: N / D is the mean in every direction
            }
            if (delta == 5.0 && order == 2U) order2At5 = m.MaxError;
            if (delta == 90.0 && order == 2U) order2At90 = m.MaxError;
            if (delta == 90.0 && order == 8U) order8At90 = m.MaxError;
        }
    }
    // Real non-planar cells (creases and silhouettes) of the corner room.
    const Prepared p = Make(CornerRoomScene(96, 64), true);
    const DepthHierarchy h = Hierarchy(p);
    std::vector<size_t> texels;
    double cellMax = 0.0, cellMean = 0.0, clippedMax = 0.0, cellWeighted = 0.0, clippedWeighted = 0.0;
    size_t cells = 0, negative = 0, coefficientsTotal = 0;
    for (uint32_t l = 1; l <= 3U; ++l)
        for (uint32_t y = 0; y < h.Levels[l].Height; ++y)
            for (uint32_t x = 0; x < h.Levels[l].Width; ++x) {
                const HierarchyCell& c = h.Cell(l, x, y);
                if ((c.Flags & kCellGeometry) == 0U || (c.Flags & kCellPlanar) != 0U) continue;
                CellTexels(h, l, x, y, texels);
                std::vector<Rgb> radiance;
                std::vector<Vec3> normals;
                for (const size_t i : texels) {
                    const size_t bi = (static_cast<size_t>(p.Input.Viewport.Y) + i / 96) * p.Input.BufferWidth +
                                      p.Input.Viewport.X + i % 96;
                    radiance.push_back({p.Input.Radiance[3 * bi], p.Input.Radiance[3 * bi + 1], p.Input.Radiance[3 * bi + 2]});
                    normals.push_back(p.Geometry.Normal[i]);
                }
                std::vector<double> coefficients(h.CellSh(l, x, y), h.CellSh(l, x, y) + 4U * h.ShCoefficients);
                const Metrics m = measure(radiance, normals, 2, coefficients, false, {}, {});
                std::vector<double> clipped = coefficients;
                for (double& value : clipped) {
                    ++coefficientsTotal;
                    if (value < 0.0) ++negative, value = 0.0; // what a colour clip would do (rejected)
                }
                const Metrics mc = measure(radiance, normals, 2, clipped, false, {}, {});
                cellMax = std::max(cellMax, m.MaxError);
                cellMean += m.MeanError;
                cellWeighted += m.WeightedMean;
                clippedMax = std::max(clippedMax, mc.MaxError);
                clippedWeighted += mc.WeightedMean;
                ++cells;
            }
    std::printf("DGI_M4 sh_scene_cells corner_room non_planar_geometry_cells=%zu order2 max_rel_error=%.4f mean=%.4f "
                "area_weighted_mean=%.4f | negative_coefficients=%zu/%zu clipping_them max_rel_error=%.4f area_weighted_mean=%.4f\n",
                cells, cellMax, cellMean / std::max<size_t>(cells, 1), cellWeighted / std::max<size_t>(cells, 1), negative,
                coefficientsTotal, clippedMax, clippedWeighted / std::max<size_t>(cells, 1));
    EXPECT_LT(order8At90, order2At90); // the richer reference converges
    EXPECT_GT(cells, 0U);
    EXPECT_GT(negative, 0U);
    (void)order2At5;
}

// ---------------------------------------------------------------------------
// Guiding (specification section 6, p = (1 - eta) / pi + eta g).

namespace {

struct Moments {
    double Sum = 0.0, Sum2 = 0.0;
    size_t N = 0;
    void Add(double v) { Sum += v, Sum2 += v * v, ++N; }
    [[nodiscard]] double Mean() const { return Sum / std::max<size_t>(N, 1); }
    [[nodiscard]] double StandardError() const {
        const double m = Mean();
        return std::sqrt(std::max(0.0, Sum2 / std::max<size_t>(N, 1) - m * m) / std::max<size_t>(N, 1));
    }
};

double ZScore(const Moments& m, double reference) {
    const double se = m.StandardError();
    return se > 0.0 ? (m.Mean() - reference) / se : (m.Mean() == reference ? 0.0 : 1e300);
}

} // namespace

TEST(DiffuseGiM4, GuidedEstimatorIsUnbiased) {
    // (a) Angular field (no depth buffer): a bright cap off the normal and a
    // wide dim cap on the other side; open elsewhere (A between 0 and 1).
    const SectionFrame frame = SectionFrame::FromView(Normalize({0.2, 0.1, 0.97}));
    const Vec3 n = Normalize(Add(Scale(frame.V, std::cos(40.0 * kDegree)), Scale(frame.Tangent(0.6), std::sin(40.0 * kDegree))));
    const CapField field(FieldSample{},
                         {{Normalize(Add(n, Scale(frame.Tangent(2.1), 0.8))), 25.0 * kDegree,
                           {DirectionState::Hit, {6.0, 4.0, 1.0}, {}}},
                          {Normalize(Add(n, Scale(frame.Tangent(0.3), -1.2))), 55.0 * kDegree,
                           {DirectionState::Hit, {0.3, 0.3, 0.3}, {}}}});
    const auto sectionF = [&](double phi) {
        const SectionContribution c = EvaluateSection(field, frame, n, phi, 64, {});
        return std::pair<double, double>{Luma(c.F), c.FA};
    };
    double referenceJ = 0.0, referenceA = 0.0;
    constexpr int kReference = 20000;
    for (int s = 0; s < kReference; ++s) {
        const auto [j, a] = sectionF(kPi * (s + 0.5) / kReference);
        referenceJ += j * kPi / kReference, referenceA += a * kPi / kReference;
    }
    // Learned-like table (g from a coarse histogram of F) and an adversarial one.
    GuidingSettings gs;
    std::vector<double> histogram(gs.Bins, 0.0), adversarial(gs.Bins, 0.0);
    for (uint32_t b = 0; b < gs.Bins; ++b) {
        for (int k = 0; k < 16; ++k) histogram[b] += sectionF(kPi * (b + (k + 0.5) / 16.0) / gs.Bins).first;
        adversarial[b] = 1.0 / (histogram[b] + 1e-3);
    }
    double worstZ = 0.0, controlZ = 1e300;
    Rng rng{12345};
    for (const double eta : {0.25, 0.5, 0.9})
        for (const auto* density : {&histogram, &adversarial}) {
            gs.Mix = eta;
            const GuideTable table = MakeGuideTable(1, 1, gs, *density);
            Moments guidedJ, guidedA, wrongJ, uniformJ;
            constexpr uint32_t kSections = 2;
            for (int trial = 0; trial < 20000; ++trial) {
                const double rotation = rng.Uniform();
                double j = 0.0, a = 0.0, wrong = 0.0, uniform = 0.0;
                for (uint32_t s = 0; s < kSections; ++s) {
                    const double u = (s + rotation) / kSections;
                    double pdf;
                    uint32_t bin;
                    const double phi = GuideDraw<double>(table, 0, u, pdf, bin);
                    const auto [fj, fa] = sectionF(phi);
                    j += fj / pdf / kSections, a += fa / pdf / kSections;
                    wrong += fj * kPi / kSections; // negative control: the draw's density ignored
                    uniform += sectionF(kPi * u).first * kPi / kSections;
                }
                guidedJ.Add(j), guidedA.Add(a), wrongJ.Add(wrong), uniformJ.Add(uniform);
            }
            const double zJ = ZScore(guidedJ, referenceJ), zA = ZScore(guidedA, referenceA);
            const double zWrong = ZScore(wrongJ, referenceJ), zUniform = ZScore(uniformJ, referenceJ);
            worstZ = std::max({worstZ, std::abs(zJ), std::abs(zA)});
            controlZ = std::min(controlZ, std::abs(zWrong));
            std::printf("DGI_M4 guiding_unbiased_field eta=%.2f table=%s J_ref=%.6f guided_mean=%.6f z=%+.2f "
                        "A_ref=%.6f guided_A=%.6f z=%+.2f | std guided=%.4f uniform=%.4f (z=%+.2f) | wrong_pdf z=%+.1f\n",
                        eta, density == &histogram ? "histogram" : "adversarial", referenceJ, guidedJ.Mean(), zJ,
                        referenceA, guidedA.Mean(), zA, guidedJ.StandardError() * std::sqrt(20000.0),
                        uniformJ.StandardError() * std::sqrt(20000.0), zUniform, zWrong);
        }
    // (b) Screen space: receivers of two scenes, a table learned from one
    // uniform frame (then frozen), S = 2, random rotations.
    double worstScreenZ = 0.0, worstScreenControl = 1e300;
    size_t receivers = 0;
    for (const Scene& scene : {CornerRoomScene(96, 64), PillarsScene(96, 64)}) {
        const Prepared p = Make(scene);
        GatherSettings g = RadialSettings(scene.Name == "pillars" ? 3.0 : 2.5, 0.5, 2, 16, 2.0);
        GuidingSettings guiding;
        GuideField learned(96, 64, guiding);
        {
            std::vector<GuideSample> records;
            GatherAcceleration acc;
            acc.Guide = &learned.Frozen();
            acc.Samples = &records;
            GatherSettings first = g;
            first.Sections = 8;
            (void)GatherImage(p.Input, first, acc);
            learned.Update(records, first.Sections);
        }
        const GuideTable table = learned.Frozen();
        const uint32_t pixels[6][2] = {{20, 40}, {48, 50}, {70, 44}, {30, 30}, {60, 20}, {85, 55}};
        for (const auto& xy : pixels) {
            if (!(p.Geometry.ViewDepth[static_cast<size_t>(xy[1]) * 96 + xy[0]] > 0.0)) continue;
            GatherSettings reference = g;
            reference.Sections = 2048;
            const PixelGather ref = GatherPixel(p.Input, p.Geometry, xy[0], xy[1], reference);
            if (!ref.Valid || Luma(ref.J) < 1e-3) continue;
            ++receivers;
            Moments guidedJ, guidedA, wrongJ;
            std::vector<GuideSample> records;
            GatherAcceleration acc;
            acc.Guide = &table;
            acc.Samples = &records;
            GatherSettings guided = g;
            guided.Guiding = true;
            guided.GuidingAccessibility = GuidedAccessibility::SamePdf; // the A that depends on the guide
            for (int trial = 0; trial < 1500; ++trial) {
                guided.Rotation = rng.Uniform();
                const PixelGather r = GatherPixel(p.Input, p.Geometry, xy[0], xy[1], guided, acc);
                guidedJ.Add(Luma(r.J)), guidedA.Add(r.ARaw);
                double wrong = 0.0;
                for (const auto& record : records) wrong += record.Contribution * kPi / guided.Sections;
                wrongJ.Add(wrong);
            }
            const double zJ = ZScore(guidedJ, Luma(ref.J)), zA = ZScore(guidedA, ref.ARaw), zW = ZScore(wrongJ, Luma(ref.J));
            worstScreenZ = std::max({worstScreenZ, std::abs(zJ), std::abs(zA)});
            worstScreenControl = std::min(worstScreenControl, std::abs(zW));
            std::printf("DGI_M4 guiding_unbiased_screen scene=%s pixel=(%u,%u) J_ref=%.5f guided=%.5f z=%+.2f A_ref=%.5f "
                        "guided=%.5f z=%+.2f wrong_pdf z=%+.1f\n",
                        scene.Name.c_str(), xy[0], xy[1], Luma(ref.J), guidedJ.Mean(), zJ, ref.ARaw, guidedA.Mean(), zA, zW);
        }
    }
    std::printf("DGI_M4 guiding_unbiased max_abs_z field=%.2f screen=%.2f (receivers=%zu) | negative control min_abs_z "
                "field=%.1f screen=%.1f\n",
                worstZ, worstScreenZ, receivers, controlZ, worstScreenControl);
    EXPECT_LT(worstZ, 4.5);
    EXPECT_LT(worstScreenZ, 4.5);
    EXPECT_GT(controlZ, 10.0);
}

TEST(DiffuseGiM4, GuidingIsFrozenUpdatedSeparatelyAndResetsToUniform) {
    const Prepared p = Make(CornerRoomScene(64, 48));
    GuidingSettings gs;
    GuideField field(64, 48, gs);
    const auto uniformDeviation = [](const GuideTable& t) {
        double worst = 0.0;
        for (const double v : t.Pdf) worst = std::max(worst, std::abs(v * kPi - 1.0));
        return worst;
    };
    const double initial = uniformDeviation(field.Frozen());
    GatherSettings g = RadialSettings(2.5, 0.5, 4, 16, 2.0);
    g.Guiding = true;
    std::vector<GuideSample> records;
    GatherAcceleration acc;
    acc.Guide = &field.Frozen();
    acc.Samples = &records;
    const GuideTable before = field.Frozen();
    const auto frame0 = GatherImage(p.Input, g, acc);
    const bool frozen = before.Cdf == field.Frozen().Cdf && before.Pdf == field.Frozen().Pdf;
    const GuideUpdateReport learned = field.Update(records, g.Sections);
    const double afterLearning = uniformDeviation(field.Frozen());
    const bool beforeUnchanged = before.Pdf != field.Frozen().Pdf; // the old table was not edited in place
    // A uniform table: the guided estimator equals the uniform estimator.
    const GuideTable uniform = UniformGuideTable(64, 48, gs);
    GatherAcceleration accUniform;
    accUniform.Guide = &uniform;
    GatherSettings fixed = g;
    fixed.Rotation = 0.5;
    const auto guidedUniform = GatherImage(p.Input, fixed, accUniform);
    fixed.Guiding = false;
    const auto unguided = GatherImage(p.Input, fixed);
    double uniformEquality = 0.0;
    for (size_t i = 0; i < unguided.size(); ++i)
        uniformEquality = std::max({uniformEquality, std::abs(Luma(guidedUniform[i].J) - Luma(unguided[i].J)) / std::max(1e-9, Luma(unguided[i].J)),
                                    std::abs(guidedUniform[i].ARaw - unguided[i].ARaw)});
    // The independent uniform A of a guided frame is the unguided A, bit for bit.
    GatherAcceleration accLearned;
    accLearned.Guide = &field.Frozen();
    GatherSettings guidedIndependent = fixed;
    guidedIndependent.Guiding = true;
    guidedIndependent.GuidingAccessibility = GuidedAccessibility::IndependentUniform;
    const auto independent = GatherImage(p.Input, guidedIndependent, accLearned);
    size_t independentAMismatch = 0;
    for (size_t i = 0; i < unguided.size(); ++i)
        if (!SameBits(independent[i].ARaw, unguided[i].ARaw)) ++independentAMismatch;
    // No receiver: the memory is kept.
    std::vector<GuideSample> empty(records.size());
    const GuideTable kept = field.Frozen();
    const GuideUpdateReport none = field.Update(empty, g.Sections);
    const bool keptEqual = kept.Pdf == field.Frozen().Pdf;
    // Zero contributions everywhere: every updated region returns to uniform after its memory decays to zero.
    std::vector<GuideSample> zeros = records;
    for (auto& r : zeros) r.Contribution = 0.0;
    GuidingSettings once = gs;
    once.Rate = 1.0;
    GuideField zeroField(64, 48, once);
    zeroField.Update(records, g.Sections);
    const GuideUpdateReport zeroReport = zeroField.Update(zeros, g.Sections);
    const double afterZero = uniformDeviation(zeroField.Frozen());
    // A non-finite and a negative record reset their regions only.
    std::vector<GuideSample> broken = records;
    size_t first = 0;
    while (first < broken.size() && broken[first].Bin == kNoGuideBin) ++first;
    broken[first].Contribution = kNaN;
    size_t last = broken.size() - 1;
    while (last > 0 && broken[last].Bin == kNoGuideBin) --last;
    broken[last].Contribution = -1.0;
    const GuideUpdateReport brokenReport = field.Update(broken, g.Sections);
    const uint32_t firstRegion = field.Frozen().Region(static_cast<uint32_t>((first / g.Sections) % 64), static_cast<uint32_t>((first / g.Sections) / 64));
    double firstRegionDeviation = 0.0;
    for (uint32_t b = 0; b < gs.Bins; ++b)
        firstRegionDeviation = std::max(firstRegionDeviation, std::abs(field.Frozen().Pdf[firstRegion * gs.Bins + b] * kPi - 1.0));
    const double brokenOthers = uniformDeviation(field.Frozen());
    // Scene change.
    field.Invalidate();
    const double afterInvalidate = uniformDeviation(field.Frozen());
    std::printf("DGI_M4 guiding_state independent_A_vs_unguided_A_bit_mismatches=%zu\n", independentAMismatch);
    EXPECT_EQ(independentAMismatch, 0U);
    std::printf("DGI_M4 guiding_state initial_uniform_dev=%.1e frozen_during_frame=%d separate_update=%d learned_regions=%u "
                "learned_max_dev_from_uniform=%.3f | uniform_table guided_vs_unguided_max=%.1e | no_receiver_update kept=%d "
                "updated=%u | zero_sums reset=%u dev=%.1e | nonfinite_and_negative reset=%u region_dev=%.1e others_max_dev=%.3f "
                "| invalidate dev=%.1e\n",
                initial, frozen, beforeUnchanged, learned.Updated, afterLearning, uniformEquality, keptEqual, none.Updated,
                zeroReport.ResetZero, afterZero, brokenReport.ResetNonFinite, firstRegionDeviation, brokenOthers,
                afterInvalidate);
    (void)frame0;
    EXPECT_LT(initial, 1e-12);
    EXPECT_TRUE(frozen);
    EXPECT_TRUE(beforeUnchanged);
    EXPECT_GT(afterLearning, 0.1);
    EXPECT_LT(uniformEquality, 1e-12);
    EXPECT_TRUE(keptEqual);
    EXPECT_EQ(none.Updated, 0U);
    EXPECT_GT(zeroReport.ResetZero, 0U);
    EXPECT_LT(afterZero, 1e-12);
    EXPECT_GE(brokenReport.ResetNonFinite, 1U);
    EXPECT_LT(firstRegionDeviation, 1e-12);
    EXPECT_GT(brokenOthers, 0.1);
    EXPECT_LT(afterInvalidate, 1e-12);
}

TEST(DiffuseGiM4, GuidingParameterStudy) {
    // Per-frame estimates (S = 2, M3 stratified rotations, no temporal
    // accumulation) against the unguided S = 256 reference of the same
    // schedule, after 8 learning frames. "uniform_S4" spends the section work
    // of a guided frame with the independent uniform A (2 S sections).
    struct Config {
        const char* Name;
        bool Guided;
        GuidingSettings Settings;
        uint32_t Sections = 2;
        GuidedAccessibility Accessibility = GuidedAccessibility::SamePdf;
    };
    // One parameter at a time around the starting point (8 x 8 regions, 8
    // bins, eta 1/2, beta 1/4); then the best values together ("combined",
    // the defaults of GuidingSettings) with both A estimators.
    GuidingSettings start;
    start.RegionLog2 = 3, start.Bins = 8, start.Mix = 0.5, start.Rate = 0.25;
    std::vector<Config> configs;
    configs.push_back({"uniform", false, start});
    configs.push_back({"uniform_S4", false, start, 4});
    configs.push_back({"start", true, start});
    for (const uint32_t r : {2U, 4U, 5U}) {
        Config c{"region", true, start};
        c.Settings.RegionLog2 = r;
        configs.push_back(c);
    }
    for (const uint32_t b : {4U, 16U}) {
        Config c{"bins", true, start};
        c.Settings.Bins = b;
        configs.push_back(c);
    }
    for (const double eta : {0.25, 0.75}) {
        Config c{"eta", true, start};
        c.Settings.Mix = eta;
        configs.push_back(c);
    }
    for (const double beta : {0.1, 0.5, 1.0}) {
        Config c{"beta", true, start};
        c.Settings.Rate = beta;
        configs.push_back(c);
    }
    {
        Config c{"combined_same_pdf_A", true, GuidingSettings{}};
        configs.push_back(c);
        c.Name = "combined_independent_A";
        c.Accessibility = GuidedAccessibility::IndependentUniform;
        configs.push_back(c);
        Config d{"start_independent_A", true, start};
        d.Accessibility = GuidedAccessibility::IndependentUniform;
        configs.push_back(d);
    }
    for (const Scene& scene : {CornerRoomScene(96, 64), PillarsScene(96, 64)}) {
        const Prepared p = Make(scene);
        const double radius = scene.Name == "pillars" ? 3.0 : 2.5;
        GatherSettings base = RadialSettings(radius, 0.5, 2, 16, 2.0);
        base.Rotations.Mode = RotationMode::Stratified;
        base.SequenceRadialJitter = true;
        GatherSettings referenceSettings = RadialSettings(radius, 0.5, 256, 16, 2.0);
        const auto reference = GatherImage(p.Input, referenceSettings);
        double uniformJ = 0.0, uniformA = 0.0;
        for (const Config& c : configs) {
            GuideField field(96, 64, c.Settings);
            std::vector<GuideSample> records;
            double errorJ = 0.0, errorA = 0.0, energy = 0.0, biasJ = 0.0, meanRef = 0.0;
            constexpr int kFrames = 40, kWarmup = 8;
            for (int frame = 0; frame < kFrames; ++frame) {
                GatherSettings g = base;
                g.Rotations.Frame = static_cast<uint64_t>(frame);
                g.Guiding = c.Guided;
                g.Sections = c.Sections;
                g.GuidingAccessibility = c.Accessibility;
                GatherAcceleration acc;
                acc.Guide = &field.Frozen();
                acc.Samples = &records;
                const auto image = GatherImage(p.Input, g, acc);
                if (frame >= kWarmup)
                    for (size_t i = 0; i < image.size(); ++i) {
                        if (!reference[i].Valid) continue;
                        const double dj = Luma(image[i].J) - Luma(reference[i].J);
                        errorJ += dj * dj, energy += Luma(reference[i].J) * Luma(reference[i].J);
                        errorA += (image[i].ARaw - reference[i].ARaw) * (image[i].ARaw - reference[i].ARaw);
                        biasJ += dj, meanRef += Luma(reference[i].J);
                    }
                field.Update(records, g.Sections);
            }
            if (!c.Guided && c.Sections == 2U) uniformJ = errorJ, uniformA = errorA;
            const uint32_t work = c.Sections * (c.Guided && c.Accessibility == GuidedAccessibility::IndependentUniform ? 2U : 1U);
            std::printf("DGI_M4 guiding_study scene=%s config=%s region=%u bins=%u eta=%.2f beta=%.2f A=%s sections_of_work=%u "
                        "rel_mse_J=%.4f variance_reduction_J=%.3f variance_reduction_A=%.3f frame_mean_bias_J=%+.4f\n",
                        scene.Name.c_str(), c.Name, 1U << c.Settings.RegionLog2, c.Settings.Bins, c.Settings.Mix,
                        c.Settings.Rate,
                        c.Guided ? (c.Accessibility == GuidedAccessibility::SamePdf ? "same_pdf" : "independent") : "uniform",
                        work, errorJ / energy, uniformJ / errorJ, uniformA / errorA, biasJ / meanRef);
        }
    }
}

// ---------------------------------------------------------------------------
// Blue noise (specification sections 6 and 12; [B4] section 3.1, eq. 4).

TEST(DiffuseGiM4, BlueNoiseMaskGeneratorSpectraAndAssetRoundTrip) {
    NoiseMaskSettings settings;
    const auto start = std::chrono::steady_clock::now();
    const NoiseMask mask = GenerateBlueNoiseMask(settings);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const NoiseMask again = GenerateBlueNoiseMask(settings);
    std::vector<uint8_t> seen(mask.Count(), 0U);
    bool permutation = true;
    for (const uint32_t r : mask.Rank) {
        permutation = permutation && r < mask.Count() && seen[r] == 0U;
        if (r < mask.Count()) seen[r] = 1U;
    }
    NoiseMaskSettings flat = settings;
    flat.Spatiotemporal = false;
    const NoiseMask slices = GenerateBlueNoiseMask(flat);
    const NoiseMask white = GenerateWhiteNoiseMask(settings);
    const auto print = [](const char* name, const NoiseSpectrum& s) {
        std::printf("DGI_M4 noise_spectrum mask=%s spatial_low=%.3f spatial_high=%.3f temporal_low=%.3f temporal_high=%.3f "
                    "slice_ks=%.4f column_max_gap=%.3f lag1_spatial=%+.3f lag1_temporal=%+.3f\n",
                    name, s.SpatialLow, s.SpatialHigh, s.TemporalLow, s.TemporalHigh, s.SliceUniformity, s.ColumnCoverage,
                    s.NeighbourCorrelation, s.TemporalCorrelation);
    };
    const NoiseSpectrum stbn = AnalyzeNoiseMask(mask), twoD = AnalyzeNoiseMask(slices), whiteSpectrum = AnalyzeNoiseMask(white);
    print("stbn_32x32x32_sigma1.9", stbn);
    print("2d_per_slice_32x32x32", twoD);
    print("white_32x32x32", whiteSpectrum);
    for (const double sigma : {1.5, 2.5}) {
        NoiseMaskSettings s = settings;
        s.Sigma = sigma;
        char name[64];
        std::snprintf(name, sizeof(name), "stbn_32x32x32_sigma%.1f", sigma);
        print(name, AnalyzeNoiseMask(GenerateBlueNoiseMask(s)));
    }
    // Asset: serialize, write, read back, parse; corrupted copies are rejected.
    const std::vector<uint8_t> bytes = SerializeNoiseMask(mask);
    const char* directory = std::getenv("DGI_M4_ASSET_DIR");
    const std::string path = std::string(directory != nullptr ? directory : ".") + "/dgi_stbn_32x32x32.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::vector<uint8_t> read;
    {
        std::ifstream in(path, std::ios::binary);
        read.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    NoiseMask parsed;
    const std::string parseError = ParseNoiseMask(read, parsed);
    const bool roundTrip = parseError.empty() && parsed.Rank == mask.Rank && parsed.Width == mask.Width &&
                           parsed.Settings.Sigma == mask.Settings.Sigma && parsed.Settings.Seed == mask.Settings.Seed;
    const auto withChecksum = [](std::vector<uint8_t> b) {
        const uint64_t h = Fnv1a64(b.data(), b.size() - 8U);
        for (int k = 0; k < 8; ++k) b[b.size() - 8U + k] = static_cast<uint8_t>(h >> (8 * k));
        return b;
    };
    std::vector<std::pair<std::string, std::vector<uint8_t>>> corrupt;
    corrupt.push_back({"magic", read});
    corrupt.back().second[0] = 'X';
    corrupt.push_back({"payload_bit", read});
    corrupt.back().second[100] ^= 1U;
    corrupt.push_back({"truncated", std::vector<uint8_t>(read.begin(), read.end() - 9)});
    {
        std::vector<uint8_t> dup = read;
        std::memcpy(&dup[48 + 4], &dup[48], 4); // two voxels with the same rank
        corrupt.push_back({"duplicate_rank", withChecksum(dup)});
        std::vector<uint8_t> zero = read;
        zero[8] = zero[9] = zero[10] = zero[11] = 0U;
        corrupt.push_back({"zero_width", withChecksum(zero)});
    }
    size_t rejected = 0;
    for (const auto& [name, data] : corrupt) {
        NoiseMask m;
        const std::string error = ParseNoiseMask(data, m);
        std::printf("DGI_M4 noise_asset corruption=%s rejected=%d reason=\"%s\"\n", name.c_str(), !error.empty(), error.c_str());
        rejected += error.empty() ? 0U : 1U;
    }
    std::printf("DGI_M4 noise_asset generation_seconds=%.2f deterministic=%d permutation=%d bytes=%zu fnv1a64=%016llx "
                "round_trip=%d rejected=%zu/%zu file=%s\n",
                seconds, mask.Rank == again.Rank, permutation, bytes.size(),
                static_cast<unsigned long long>(Fnv1a64(bytes.data(), bytes.size())), roundTrip, rejected, corrupt.size(),
                path.c_str());
    EXPECT_TRUE(mask.Rank == again.Rank);
    EXPECT_TRUE(permutation);
    EXPECT_TRUE(roundTrip);
    EXPECT_EQ(rejected, corrupt.size());
    EXPECT_LT(stbn.SpatialLow, 0.5 * whiteSpectrum.SpatialLow);
    EXPECT_LT(stbn.TemporalLow, 0.5 * whiteSpectrum.TemporalLow);
    EXPECT_LT(twoD.SpatialLow, 0.5 * whiteSpectrum.SpatialLow);
}

TEST(DiffuseGiM4, BlueNoiseRotationConvergence) {
    const Prepared p = Make(CornerRoomScene(96, 64));
    NoiseMaskSettings ns;
    const NoiseMask stbn = GenerateBlueNoiseMask(ns);
    NoiseMaskSettings flat = ns;
    flat.Spatiotemporal = false;
    const NoiseMask slices = GenerateBlueNoiseMask(flat);
    const NoiseMask white = GenerateWhiteNoiseMask(ns);
    GatherSettings base = RadialSettings(2.5, 0.5, 1, 16, 2.0);
    GatherSettings referenceSettings = RadialSettings(2.5, 0.5, 64, 16, 2.0);
    referenceSettings.RadialJitter = 0.5;
    // Reference: the average over 16 radial jitters of S = 64 (both dimensions resolved).
    std::vector<double> reference(96 * 64, 0.0);
    for (int j = 0; j < 16; ++j) {
        referenceSettings.RadialJitter = (j + 0.5) / 16.0;
        const auto image = GatherImage(p.Input, referenceSettings);
        for (size_t i = 0; i < image.size(); ++i) reference[i] += Luma(image[i].J) / 16.0;
    }
    struct Source {
        const char* Name;
        RotationMode Mode;
        const NoiseMask* Mask;
    };
    const Source sources[] = {{"white", RotationMode::BlueNoise, &white},
                              {"m3_stratified", RotationMode::Stratified, nullptr},
                              {"2d_blue_per_slice", RotationMode::BlueNoise, &slices},
                              {"spatiotemporal_blue", RotationMode::BlueNoise, &stbn}};
    for (const Source& source : sources) {
        GatherSettings g = base;
        g.Rotations.Mode = source.Mode;
        g.SequenceRadialJitter = source.Mode == RotationMode::Stratified;
        GatherAcceleration acc;
        acc.Noise = source.Mask;
        std::vector<double> sum(reference.size(), 0.0);
        std::printf("DGI_M4 noise_convergence source=%s rmse_rel_by_frames:", source.Name);
        double lowFraction = 0.0;
        for (int frame = 0; frame < 32; ++frame) {
            g.Rotations.Frame = static_cast<uint64_t>(frame);
            const auto image = GatherImage(p.Input, g, acc);
            for (size_t i = 0; i < image.size(); ++i) sum[i] += Luma(image[i].J);
            const int frames = frame + 1;
            if ((frames & (frames - 1)) == 0) {
                double error = 0.0, energy = 0.0;
                std::vector<double> residual(reference.size(), 0.0);
                for (size_t i = 0; i < reference.size(); ++i) {
                    residual[i] = sum[i] / frames - reference[i];
                    error += residual[i] * residual[i];
                    energy += reference[i] * reference[i];
                }
                std::printf(" F%d=%.4f", frames, std::sqrt(error / energy));
                if (frames == 4) {
                    // Low-frequency share of the residual (|f| <= 1/8 cycles per pixel),
                    // 2D DFT by rows and columns, relative to its white-noise expectation.
                    double low = 0.0, all = 0.0;
                    size_t lowCount = 0, allCount = 0;
                    for (uint32_t ky = 0; ky < 64; ++ky)
                        for (uint32_t kx = 0; kx < 96; ++kx) {
                            if (kx == 0U && ky == 0U) continue;
                            const double fx = std::min(kx, 96U - kx) / 96.0, fy = std::min(ky, 64U - ky) / 64.0;
                            double re = 0.0, im = 0.0;
                            for (uint32_t y = 0; y < 64; ++y)
                                for (uint32_t x = 0; x < 96; ++x) {
                                    const double a = -2.0 * kPi * (static_cast<double>(kx) * x / 96.0 + static_cast<double>(ky) * y / 64.0);
                                    re += residual[static_cast<size_t>(y) * 96 + x] * std::cos(a);
                                    im += residual[static_cast<size_t>(y) * 96 + x] * std::sin(a);
                                }
                            const double power = re * re + im * im;
                            all += power, ++allCount;
                            if (std::sqrt(fx * fx + fy * fy) <= 0.125) low += power, ++lowCount;
                        }
                    lowFraction = (low / lowCount) / (all / allCount);
                }
            }
        }
        std::printf(" | residual_low_frequency_ratio_F4=%.3f\n", lowFraction);
    }
}

// ---------------------------------------------------------------------------
// T10: every optimization alone and together against the M1-M3 baseline.

namespace {

struct T10Scene {
    Scene Value;
    double Radius, Thickness;
};

std::vector<T10Scene> T10Scenes() {
    return {{CornerRoomScene(96, 64), 2.5, 0.5},
            {PillarsScene(96, 64), 3.0, 0.5},
            {CheckerRoomScene(96, 64), 2.5, 0.5},
            {BlackOccluderScene(96, 64, true), 5.0, 1.0}};
}

void PrintT10(const char* part, const std::string& scene, const char* config, const ImageDiff& vsBaseline,
              const ImageDiff& vsReference, double coarse) {
    std::printf("DGI_M4 T10 part=%s scene=%s config=%s mean_J=%.5f mean_A=%.5f | vs_baseline bias_J=%+.4f mean_abs_rel_J=%.4f "
                "p95_abs_J=%.4f max_abs_J=%.4f bias_A=%+.5f max_abs_A=%.4f | vs_reference bias_J=%+.4f mean_abs_rel_J=%.4f "
                "bias_A=%+.5f | coarse_fraction=%.3f\n",
                part, scene.c_str(), config, vsBaseline.MeanJa, vsBaseline.MeanAa, vsBaseline.MeanBiasJ(),
                vsBaseline.RelMeanAbsJ(), vsBaseline.P95AbsJ, vsBaseline.MaxAbsJ, vsBaseline.MeanAa - vsBaseline.MeanAb,
                vsBaseline.MaxAbsA, vsReference.MeanBiasJ(), vsReference.RelMeanAbsJ(), vsReference.MeanAa - vsReference.MeanAb,
                coarse);
}

std::vector<PixelGather> Average(const std::vector<std::vector<PixelGather>>& frames) {
    std::vector<PixelGather> out = frames.front();
    for (size_t i = 0; i < out.size(); ++i) {
        Rgb j;
        double a = 0.0;
        for (const auto& f : frames) j.R += f[i].J.R, j.G += f[i].J.G, j.B += f[i].J.B, a += f[i].ARaw;
        const double n = static_cast<double>(frames.size());
        out[i].J = {j.R / n, j.G / n, j.B / n};
        out[i].ARaw = a / n;
    }
    return out;
}

} // namespace

TEST(DiffuseGiM4, T10OptimizationsAgainstTheBaseline) {
    NoiseMaskSettings ns;
    const NoiseMask stbn = GenerateBlueNoiseMask(ns);
    double worstDeterministicBias = 0.0, worstStochasticBias = 0.0;
    for (const T10Scene& t : T10Scenes()) {
        const Prepared p = Make(t.Value);
        const DepthHierarchy h = Hierarchy(p);
        GatherAcceleration acc;
        acc.Hierarchy = &h;
        // Deterministic, noise-free mode (S = 8, rotation and jitter 1/2).
        const auto reference = GatherImage(p.Input, EverySettings(t.Radius, t.Thickness, 8));
        const GatherSettings baselineSettings = RadialSettings(t.Radius, t.Thickness, 8, 16, 2.0);
        const auto baseline = GatherImage(p.Input, baselineSettings);
        const ImageDiff baselineVsReference = Compare(baseline, reference);
        PrintT10("deterministic", t.Value.Name, "baseline_radial", Compare(baseline, baseline), baselineVsReference, 0.0);
        struct Config {
            const char* Name;
            CandidateSchedule Kind;
            bool Depth;
            CacheMode Cache;
        };
        const Config configs[] = {{"hierarchy", CandidateSchedule::Radial, true, CacheMode::Off},
                                  {"cache_mean", CandidateSchedule::Radial, false, CacheMode::Mean},
                                  {"cache_sh", CandidateSchedule::Radial, false, CacheMode::SphericalHarmonics},
                                  {"hierarchy_cache", CandidateSchedule::Radial, true, CacheMode::Mean},
                                  {"metric_radial", CandidateSchedule::MetricRadial, false, CacheMode::Off},
                                  {"metric_hierarchy_cache", CandidateSchedule::MetricRadial, true, CacheMode::Mean}};
        for (const Config& c : configs) {
            GatherSettings g = baselineSettings;
            g.Schedule = c.Kind;
            g.HierarchyFootprint = c.Depth;
            g.Cache = c.Cache;
            ASSERT_STREQ(ValidateGatherAcceleration(p.Input, g, acc), "");
            const auto image = GatherImage(p.Input, g, acc);
            const ImageDiff vsBaseline = Compare(image, baseline);
            const ImageDiff vsReference = Compare(image, reference);
            PrintT10("deterministic", t.Value.Name, c.Name, vsBaseline, vsReference,
                     static_cast<double>(SumCoarse(image)) / std::max<uint64_t>(SumScheduled(image), 1));
            if (c.Kind == CandidateSchedule::Radial && !c.Depth)
                worstDeterministicBias = std::max(worstDeterministicBias, std::abs(vsBaseline.MeanBiasJ()));
            // Declared T10 tolerances (document): the cache alone changes only the
            // radiance of planar footprints (A identical, mean J within 0.5%); the
            // footprint strips move J and A toward the EveryTexel reference.
            if (!c.Depth && c.Cache != CacheMode::Off) {
                EXPECT_LT(std::abs(vsBaseline.MeanBiasJ()), 0.005) << t.Value.Name << " " << c.Name;
                EXPECT_EQ(vsBaseline.MaxAbsA, 0.0) << t.Value.Name << " " << c.Name;
            }
            if (c.Depth) {
                EXPECT_LT(std::abs(vsReference.MeanBiasJ()), std::abs(baselineVsReference.MeanBiasJ())) << t.Value.Name;
                EXPECT_LT(std::abs(vsReference.MeanAa - vsReference.MeanAb),
                          std::abs(baselineVsReference.MeanAa - baselineVsReference.MeanAb))
                    << t.Value.Name;
            }
        }
        // Frames: S = 2 per frame, 32 frames, per-pixel rotations; the image is the
        // per-pixel mean over the frames (guiding learns online, frozen per frame).
        GatherSettings frameBase = RadialSettings(t.Radius, t.Thickness, 2, 16, 2.0);
        frameBase.Rotations.Mode = RotationMode::Stratified;
        frameBase.SequenceRadialJitter = true;
        const auto frameReference = GatherImage(p.Input, EverySettings(t.Radius, t.Thickness, 64));
        struct FrameConfig {
            const char* Name;
            bool Guiding, Noise, All;
        };
        const FrameConfig frameConfigs[] = {{"baseline_stratified", false, false, false},
                                            {"guiding", true, false, false},
                                            {"blue_noise", false, true, false},
                                            {"all_together", true, true, true}};
        std::vector<PixelGather> frameBaseline;
        ImageDiff frameBaselineVsReference;
        for (const FrameConfig& c : frameConfigs) {
            GuideField field(96, 64, GuidingSettings{});
            std::vector<GuideSample> records;
            std::vector<std::vector<PixelGather>> frames;
            double frameNoise = 0.0;
            uint64_t coarse = 0, scheduled = 0;
            for (int frame = 0; frame < 32; ++frame) {
                GatherSettings g = frameBase;
                g.Rotations.Frame = static_cast<uint64_t>(frame);
                g.Guiding = c.Guiding;
                if (c.Noise) g.Rotations.Mode = RotationMode::BlueNoise, g.SequenceRadialJitter = false;
                if (c.All) g.Schedule = CandidateSchedule::MetricRadial, g.HierarchyFootprint = true, g.Cache = CacheMode::Mean;
                GatherAcceleration a;
                a.Hierarchy = &h;
                a.Guide = &field.Frozen();
                a.Samples = &records;
                a.Noise = &stbn;
                frames.push_back(GatherImage(p.Input, g, a));
                coarse += SumCoarse(frames.back());
                scheduled += SumScheduled(frames.back());
                field.Update(records, g.Sections);
                const ImageDiff single = Compare(frames.back(), frameReference);
                frameNoise += single.RelMeanAbsJ() / 32.0;
            }
            const auto mean = Average(frames);
            if (frameBaseline.empty()) frameBaseline = mean;
            const ImageDiff vsBaseline = Compare(mean, frameBaseline);
            const ImageDiff vsFrameReference = Compare(mean, frameReference);
            if (frameBaselineVsReference.Pixels == 0U) frameBaselineVsReference = vsFrameReference;
            PrintT10("frames", t.Value.Name, c.Name, vsBaseline, vsFrameReference,
                     static_cast<double>(coarse) / std::max<uint64_t>(scheduled, 1));
            // Guiding and blue noise change only the sampling (unbiased): the
            // 32-frame means agree with the baseline within 1%; everything
            // together moves toward the reference (footprint strips).
            if (!c.All) EXPECT_LT(std::abs(vsBaseline.MeanBiasJ()), 0.01) << t.Value.Name << " " << c.Name;
            else EXPECT_LT(std::abs(vsFrameReference.MeanBiasJ()), std::abs(frameBaselineVsReference.MeanBiasJ()));
            std::printf("DGI_M4 T10 part=frames scene=%s config=%s single_frame_mean_abs_rel_J_vs_reference=%.4f\n",
                        t.Value.Name.c_str(), c.Name, frameNoise);
            if (!c.All) worstStochasticBias = std::max(worstStochasticBias, std::abs(vsBaseline.MeanBiasJ()));
        }
    }
    std::printf("DGI_M4 T10 worst_mean_bias_vs_baseline deterministic_radial_configs=%.4f stochastic_configs=%.4f\n",
                worstDeterministicBias, worstStochasticBias);
}

// ---------------------------------------------------------------------------
// FP32 twins of the M4 paths (the CPU side of the GPU comparison).

TEST(DiffuseGiM4, Fp32TwinsOfTheM4Paths) {
    NoiseMaskSettings ns;
    ns.Width = ns.Height = 16, ns.Depth = 8;
    const NoiseMask noise = GenerateBlueNoiseMask(ns);
    for (const Scene& scene : {CornerRoomScene(97, 61), CheckerRoomScene(97, 61), BlackOccluderScene(97, 61, true)}) {
        const Prepared p = Make(scene);
        const DepthHierarchy h64 = Hierarchy(p), h32 = Hierarchy(p, {}, true);
        size_t flagFlips = 0, planarBoth = 0;
        double planeDiff = 0.0, meanDiff = 0.0;
        for (size_t i = 0; i < h64.Cells.size(); ++i) {
            const auto& a = h64.Cells[i];
            const auto& b = h32.Cells[i];
            if (a.Flags != b.Flags) ++flagFlips;
            if ((a.Flags & b.Flags & kCellPlanar) == 0U) continue;
            ++planarBoth;
            planeDiff = std::max(planeDiff, std::abs(a.W - b.W) / a.W);
            meanDiff = std::max({meanDiff, std::abs(a.Mean.R - b.Mean.R), std::abs(a.Mean.G - b.Mean.G),
                                 std::abs(a.Mean.B - b.Mean.B)});
        }
        std::printf("DGI_M4 fp32_hierarchy scene=%s cells=%zu flag_flips=%zu planar_both=%zu plane_rel_max=%.2e "
                    "cache_mean_max_abs=%.2e\n",
                    scene.Name.c_str(), h64.Cells.size(), flagFlips, planarBoth, planeDiff, meanDiff);
        EXPECT_LE(flagFlips, h64.Cells.size() / 1000);
        // A guide table learned in double from one frame, frozen and shared.
        GuidingSettings gs;
        GuideField learner(97, 61, gs);
        std::vector<GuideSample> records;
        const double radius = scene.Name == "t06_black_occluder" ? 5.0 : 2.5;
        {
            GatherAcceleration acc;
            acc.Guide = &learner.Frozen();
            acc.Samples = &records;
            (void)GatherImage(p.Input, RadialSettings(radius, 0.5, 8, 16, 2.0), acc);
            learner.Update(records, 8);
        }
        const GuideTable table = learner.Frozen();
        // The guide update itself: the same records through the double and the float update.
        GuideField u64(97, 61, gs), u32(97, 61, gs, true);
        u64.Update(records, 8);
        u32.Update(records, 8);
        double updateDiff = 0.0;
        for (size_t i = 0; i < u64.Frozen().Pdf.size(); ++i)
            updateDiff = std::max(updateDiff, std::abs(u64.Frozen().Pdf[i] - u32.Frozen().Pdf[i]) * kPi);
        std::printf("DGI_M4 fp32_guide_update scene=%s pdf_times_pi_max_abs=%.2e\n", scene.Name.c_str(), updateDiff);
        EXPECT_LT(updateDiff, 1.0e-5);
        struct Feature {
            const char* Name;
            CandidateSchedule Kind;
            bool Footprint, Guiding, Noise;
            CacheMode Cache;
        };
        const Feature features[] = {{"metric_radial", CandidateSchedule::MetricRadial, false, false, false, CacheMode::Off},
                                    {"hierarchy", CandidateSchedule::Radial, true, false, false, CacheMode::Off},
                                    {"cache_mean", CandidateSchedule::Radial, false, false, false, CacheMode::Mean},
                                    {"cache_sh", CandidateSchedule::Radial, true, false, false, CacheMode::SphericalHarmonics},
                                    {"guiding", CandidateSchedule::Radial, false, true, false, CacheMode::Off},
                                    {"guiding_same_pdf_A", CandidateSchedule::Radial, false, true, false, CacheMode::Off},
                                    {"blue_noise", CandidateSchedule::Radial, false, false, true, CacheMode::Off},
                                    {"all", CandidateSchedule::MetricRadial, true, true, true, CacheMode::Mean}};
        for (const Feature& f : features) {
            GatherSettings g = RadialSettings(radius, 0.5, 4, 16, 2.0);
            g.Schedule = f.Kind;
            g.HierarchyFootprint = f.Footprint;
            g.Cache = f.Cache;
            g.Guiding = f.Guiding;
            if (std::string(f.Name) == "guiding_same_pdf_A") g.GuidingAccessibility = GuidedAccessibility::SamePdf;
            if (f.Noise) g.Rotations.Mode = RotationMode::BlueNoise, g.Rotations.Frame = 3;
            GatherAcceleration a64, a32;
            a64.Hierarchy = &h64, a64.Guide = &table, a64.Noise = &noise;
            a32.Hierarchy = &h32, a32.Guide = &table, a32.Noise = &noise;
            ASSERT_STREQ(ValidateGatherAcceleration(p.Input, g, a64), "");
            const auto i64 = GatherImage(p.Input, g, a64);
            const auto i32 = GatherImageF32(p.Input, g, a32);
            double meanJ = 0.0, meanA = 0.0, maxJ = 0.0, maxA = 0.0;
            size_t over = 0, valid = 0, validMismatch = 0;
            float maxRadiance = 1.0F;
            for (const float r : p.Input.Radiance) maxRadiance = std::max(maxRadiance, r);
            for (size_t i = 0; i < i64.size(); ++i) {
                if (i64[i].Valid != i32[i].Valid) ++validMismatch;
                if (!i64[i].Valid) continue;
                ++valid;
                const double scale = std::max({1.0, std::abs(i64[i].J.R), std::abs(i64[i].J.G), std::abs(i64[i].J.B)});
                const double dj = std::max({std::abs(i32[i].J.R - i64[i].J.R), std::abs(i32[i].J.G - i64[i].J.G),
                                            std::abs(i32[i].J.B - i64[i].J.B)}) / scale;
                const double da = std::abs(i32[i].ARaw - i64[i].ARaw);
                meanJ += dj, meanA += da, maxJ = std::max(maxJ, dj), maxA = std::max(maxA, da);
                if (dj > 1.0e-4 || da > 1.0e-4) ++over;
            }
            meanJ /= std::max<size_t>(valid, 1), meanA /= std::max<size_t>(valid, 1);
            // M2 tolerance model: rounding on average; isolated decision flips,
            // each at most one sector of one section (2 pi / (N S), times the
            // radiance for J); a footprint decision moves at most one strip.
            std::printf("DGI_M4 fp32_gather scene=%s feature=%s mean_abs_J=%.2e mean_abs_A=%.2e max_abs_J=%.2e max_abs_A=%.2e "
                        "pixels_over_1e-4=%zu/%zu valid_mismatch=%zu\n",
                        scene.Name.c_str(), f.Name, meanJ, meanA, maxJ, maxA, over, valid, validMismatch);
            if (std::getenv("DGI_M4_DEBUG") != nullptr && f.Guiding && !f.Noise) {
                size_t worst = 0;
                double worstValue = -1.0;
                for (size_t i = 0; i < i64.size(); ++i)
                    if (std::abs(i32[i].ARaw - i64[i].ARaw) > worstValue) worstValue = std::abs(i32[i].ARaw - i64[i].ARaw), worst = i;
                std::vector<GuideSample> r64, r32;
                a64.Samples = &r64, a32.Samples = &r32;
                (void)GatherImage(p.Input, g, a64);
                (void)GatherImageF32(p.Input, g, a32);
                std::printf("DEBUG guided pixel=(%zu,%zu) region=%u A=%.6f/%.6f J=%.6f/%.6f\n", worst % 97, worst / 97,
                            table.Region(static_cast<uint32_t>(worst % 97), static_cast<uint32_t>(worst / 97)), i64[worst].ARaw,
                            i32[worst].ARaw, Luma(i64[worst].J), Luma(i32[worst].J));
                for (uint32_t s = 0; s < g.Sections; ++s) {
                    const auto& x64 = r64[worst * g.Sections + s];
                    const auto& x32 = r32[worst * g.Sections + s];
                    std::printf("DEBUG   s=%u bin=%u/%u pdf=%.9f/%.9f phi=%.9f/%.9f contribution=%.6f/%.6f\n", s, x64.Bin, x32.Bin,
                                x64.Pdf, x32.Pdf, x64.Phi, x32.Phi, x64.Contribution, x32.Contribution);
                }
                const uint32_t region = table.Region(static_cast<uint32_t>(worst % 97), static_cast<uint32_t>(worst / 97));
                std::printf("DEBUG   cdf:");
                for (uint32_t b = 0; b <= table.Bins; ++b) std::printf(" %.9f", table.Cdf[region * (table.Bins + 1) + b]);
                std::printf("\n");
            }
            // Declared M4 FP32 model: the means are dominated by isolated
            // decisions (a sector centre, a level or a strip end) that flip in
            // at most 2% of the pixels, each moving at most one strip or one
            // candidate of one of S = 4 sections.
            EXPECT_EQ(validMismatch, 0U);
            EXPECT_LT(meanJ, 5.0e-4);
            EXPECT_LT(meanA, 5.0e-4);
            EXPECT_LE(over, valid / 50);
            (void)maxRadiance;
        }
    }
}

// ---------------------------------------------------------------------------
// Robustness (specification section 9): non-finite source radiance never
// enters a cell, so no coarse candidate carries it; extreme settings stay finite.

TEST(DiffuseGiM4, NonFiniteSourcesNeverEnterTheCache) {
    Prepared p = Make(CheckerRoomScene(96, 64));
    const float poison[3] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()};
    const uint32_t texels[3][2] = {{40, 50}, {60, 45}, {20, 30}};
    for (int k = 0; k < 3; ++k) {
        const size_t bi = static_cast<size_t>(texels[k][1]) * p.Input.BufferWidth + texels[k][0];
        p.Input.Radiance[3 * bi] = poison[k];
    }
    const DepthHierarchy h = Hierarchy(p);
    size_t poisonedPlanar = 0;
    for (uint32_t l = 0; l < h.Levels.size(); ++l)
        for (int k = 0; k < 3; ++k)
            if ((h.Cell(l, texels[k][0] >> l, texels[k][1] >> l).Flags & kCellPlanar) != 0U) ++poisonedPlanar;
    GatherAcceleration acc;
    acc.Hierarchy = &h;
    GatherSettings g = RadialSettings(2.5, 0.5, 8, 16, 2.0);
    const auto plain = GatherImage(p.Input, g);
    g.Schedule = CandidateSchedule::MetricRadial, g.HierarchyFootprint = true, g.Cache = CacheMode::Mean;
    size_t nonFiniteCoarse = 0;
    for (uint32_t y = 0; y < 64; y += 2)
        for (uint32_t x = 0; x < 96; x += 2)
            for (const Candidate& c : BuildSection(p.Input, p.Geometry, x, y, 0.7, g, acc).Ordered)
                if (c.Level > 0U && !(std::isfinite(c.Radiance.R) && std::isfinite(c.Radiance.G) && std::isfinite(c.Radiance.B)))
                    ++nonFiniteCoarse;
    size_t plainNonFinite = 0, cachedNonFinite = 0;
    const auto cached = GatherImage(p.Input, g, acc);
    for (size_t i = 0; i < cached.size(); ++i) {
        plainNonFinite += std::isfinite(Luma(plain[i].J)) ? 0U : 1U;
        cachedNonFinite += std::isfinite(Luma(cached[i].J)) ? 0U : 1U;
    }
    // Extreme settings: one candidate, one sector, one section, R and tau far beyond the scene.
    GatherSettings extreme = g;
    extreme.CandidatesPerSide = 1, extreme.Sectors = 1, extreme.Sections = 1, extreme.HierarchyMaxLevel = 4;
    extreme.Radius = 1.0e6, extreme.Thickness = 1.0e6;
    const Prepared clean = Make(CheckerRoomScene(37, 23));
    const DepthHierarchy hc = Hierarchy(clean);
    GatherAcceleration accClean;
    accClean.Hierarchy = &hc;
    size_t extremeNonFinite = 0;
    for (const auto& r : GatherImage(clean.Input, extreme, accClean))
        extremeNonFinite += std::isfinite(Luma(r.J)) && std::isfinite(r.ARaw) ? 0U : 1U;
    std::printf("DGI_M4 robustness poisoned_texels=3 planar_cells_holding_them=%zu nonfinite_coarse_candidates=%zu "
                "nonfinite_pixels point_reference=%zu with_footprint_cache=%zu | extreme_settings nonfinite=%zu\n",
                poisonedPlanar, nonFiniteCoarse, plainNonFinite, cachedNonFinite, extremeNonFinite);
    EXPECT_EQ(poisonedPlanar, 0U);
    EXPECT_EQ(nonFiniteCoarse, 0U);
    EXPECT_EQ(extremeNonFinite, 0U);
}
