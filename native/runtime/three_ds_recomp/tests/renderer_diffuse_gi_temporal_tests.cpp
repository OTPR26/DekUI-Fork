// M3 acceptance tests of the diffuse screen-space GI specification: rotation
// sequence, stratum reconstruction, temporal accumulation and a-trous filter
// (T11-T16, the four [B2]-derived requirements, and the parameter studies
// that chose the defaults). Lines starting with DGI_M3 are the reproducible
// numbers quoted in docs/TRIAEVUM_SCREEN_SPACE_GI.md ("M3").
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_scene.h"
#include "fast/renderer/diffuse_gi_sequence.h"
#include "fast/renderer/diffuse_gi_temporal.h"
#include "fast/renderer3ds/pica_geometry_guide.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// Fixtures.

constexpr uint32_t kCycle = 7; // chosen by B2InterleavedRotationsReconstructS96 (F = 5, 6, 7)

GatherSettings StratifiedGather(double radius, double thickness, uint32_t sections = 1, uint32_t cycle = kCycle) {
    GatherSettings g;
    g.Radius = radius;
    g.Thickness = thickness;
    g.Sections = sections;
    g.Sectors = 64;
    g.Threads = 4;
    g.Rotations.Mode = RotationMode::Stratified;
    g.Rotations.TileLog2 = 2;
    g.Rotations.Cycle = cycle;
    return g;
}

GatherSettings DeterministicGather(double radius, double thickness, uint32_t sections) {
    GatherSettings g;
    g.Radius = radius;
    g.Thickness = thickness;
    g.Sections = sections;
    g.Sectors = 64;
    g.Threads = 4;
    g.Rotation = 0.5;
    return g;
}

TemporalSettings DefaultTemporal() {
    TemporalSettings t;
    t.Threads = 4;
    return t;
}

FilterSettings DefaultFilter() {
    FilterSettings f;
    f.Threads = 4;
    return f;
}

struct StaticScene {
    Scene SceneValue;
    GatherInput Input;
    PreparedGeometry Geometry;
};

StaticScene MakeStatic(const Scene& scene) {
    StaticScene s{scene, Rasterize(scene), {}};
    s.Geometry = PrepareGeometry(s.Input);
    return s;
}

std::vector<PixelGather> GatherFrame(const GatherInput& input, GatherSettings g, uint64_t frame) {
    g.Rotations.Frame = frame;
    return GatherImage(input, g);
}

// Static camera and scene: zero motion, exact previous positions (= current).
TemporalFrame StaticFrame(const StaticScene& s, const std::vector<PixelGather>& gather, uint64_t index) {
    TemporalFrame f = MakeTemporalFrame(s.Input, s.Geometry, gather);
    f.FrameIndex = index;
    f.Validation = HistoryValidation::PreviousPosition;
    const size_t count = static_cast<size_t>(f.Width) * f.Height;
    f.PreviousPosition.assign(3 * count, kNaN);
    for (uint32_t y = 0; y < f.Height; ++y)
        for (uint32_t x = 0; x < f.Width; ++x) {
            const size_t i = static_cast<size_t>(y) * f.Width + x;
            if (!(f.Depth[i] > 0.0)) continue;
            const Vec3 p = s.Geometry.Position(s.Input.Camera, x, y);
            f.PreviousPosition[3 * i] = p.X, f.PreviousPosition[3 * i + 1] = p.Y, f.PreviousPosition[3 * i + 2] = p.Z;
        }
    return f;
}

// Linear combination of two gathers (J only; occupancy and A do not depend on
// radiance): J(s) = J_off + s (J_on - J_off).
std::vector<PixelGather> Blend(const std::vector<PixelGather>& off, const std::vector<PixelGather>& on, double s) {
    std::vector<PixelGather> out = off;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].J.R = off[i].J.R + s * (on[i].J.R - off[i].J.R);
        out[i].J.G = off[i].J.G + s * (on[i].J.G - off[i].J.G);
        out[i].J.B = off[i].J.B + s * (on[i].J.B - off[i].J.B);
    }
    return out;
}

void ScaleFrame(TemporalFrame& f, double factor) {
    for (size_t i = 0; i < f.Raw.size(); i += 4) {
        f.Raw[i] *= factor;
        f.Raw[i + 1] *= factor;
        f.Raw[i + 2] *= factor;
    }
}

struct Output {
    TemporalResult Temporal;
    std::vector<FilteredPixel> Filtered;
};

Output Step(TemporalAccumulator& acc, const TemporalFrame& f, const TemporalSettings& t, const FilterSettings& fs,
            Precision precision = Precision::Double) {
    Output o;
    o.Temporal = acc.Process(f, t);
    o.Filtered = FilterImage(MakeFilterInput(f, o.Temporal), fs, precision);
    return o;
}

// Compatible receivers in the whole reconstruction window (2 px): "interior".
std::vector<bool> InteriorMask(const TemporalFrame& f, int radius = 2, double sine = 0.05, double cosn = 0.9) {
    const size_t count = static_cast<size_t>(f.Width) * f.Height;
    std::vector<bool> mask(count, false);
    const auto position = [&](int x, int y) {
        const size_t i = static_cast<size_t>(y) * f.Width + x;
        const double u = (x + 0.5) / f.Width, v = (y + 0.5) / f.Height;
        const Vec3 ray{f.ViewX[0] * u + f.ViewX[1] * v + f.ViewX[2], f.ViewY[0] * u + f.ViewY[1] * v + f.ViewY[2], -1.0};
        return Scale(ray, f.Depth[i]);
    };
    for (int y = 0; y < static_cast<int>(f.Height); ++y)
        for (int x = 0; x < static_cast<int>(f.Width); ++x) {
            const size_t i = static_cast<size_t>(y) * f.Width + x;
            if (!(f.Depth[i] > 0.0)) continue;
            const Vec3 p = position(x, y);
            const Vec3 n{f.Normal[3 * i], f.Normal[3 * i + 1], f.Normal[3 * i + 2]};
            bool ok = true;
            for (int dy = -radius; dy <= radius && ok; ++dy)
                for (int dx = -radius; dx <= radius && ok; ++dx) {
                    const int qx = x + dx, qy = y + dy;
                    if (qx < 0 || qy < 0 || qx >= static_cast<int>(f.Width) || qy >= static_cast<int>(f.Height)) {
                        ok = false;
                        break;
                    }
                    const size_t q = static_cast<size_t>(qy) * f.Width + qx;
                    if (!(f.Depth[q] > 0.0)) {
                        ok = false;
                        break;
                    }
                    const Vec3 d = Add(position(qx, qy), Scale(p, -1.0));
                    const Vec3 nq{f.Normal[3 * q], f.Normal[3 * q + 1], f.Normal[3 * q + 2]};
                    ok = std::abs(Dot(n, d)) <= sine * std::sqrt(Dot(d, d)) + 1.0e-3 * f.Depth[i] && Dot(n, nq) >= cosn;
                }
            mask[i] = ok;
        }
    return mask;
}

double Lum(const Rgb& c) { return Luminance(c); }

struct Stats {
    double Mean = 0.0, Max = 0.0, P95 = 0.0;
    size_t Count = 0;
};
Stats Summarize(std::vector<double> values) {
    Stats s;
    s.Count = values.size();
    if (values.empty()) return s;
    std::sort(values.begin(), values.end());
    s.Mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    s.Max = values.back();
    s.P95 = values[std::min(values.size() - 1, static_cast<size_t>(0.95 * values.size()))];
    return s;
}

// Tile-periodic content of a residual: power at the 15 non-DC frequencies of
// a 4 x 4 tile relative to the mean power of all other non-DC frequencies
// (1 = no tile pattern), and the RMS amplitude of that tile component over the
// `support` pixels (Parseval).
struct TileSpectrum {
    double PeakFactor = 0.0, Rms = 0.0;
};
TileSpectrum TileContent(const std::vector<double>& residual, uint32_t w, uint32_t h, size_t support) {
    using C = std::complex<double>;
    std::vector<C> rows(static_cast<size_t>(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t k = 0; k < w; ++k) {
            C sum = 0.0;
            for (uint32_t x = 0; x < w; ++x)
                sum += residual[static_cast<size_t>(y) * w + x] * std::polar(1.0, -2.0 * kPi * k * x / w);
            rows[static_cast<size_t>(y) * w + k] = sum;
        }
    double tile = 0.0, other = 0.0;
    size_t tileCount = 0, otherCount = 0;
    for (uint32_t l = 0; l < h; ++l)
        for (uint32_t k = 0; k < w; ++k) {
            C sum = 0.0;
            for (uint32_t y = 0; y < h; ++y)
                sum += rows[static_cast<size_t>(y) * w + k] * std::polar(1.0, -2.0 * kPi * l * y / h);
            if (k == 0 && l == 0) continue;
            const double power = std::norm(sum);
            if ((k % (w / 4)) == 0 && (l % (h / 4)) == 0) {
                tile += power;
                ++tileCount;
            } else {
                other += power;
                ++otherCount;
            }
        }
    TileSpectrum t;
    t.PeakFactor = (tile / std::max<size_t>(tileCount, 1)) / std::max(other / std::max<size_t>(otherCount, 1), 1.0e-300);
    t.Rms = std::sqrt(tile / (static_cast<double>(w) * h * std::max<size_t>(support, 1)));
    return t;
}

// Residual image (interior pixels, mean removed, zero elsewhere).
std::vector<double> Residual(const std::vector<double>& a, const std::vector<double>& b, const std::vector<bool>& mask) {
    std::vector<double> r(a.size(), 0.0);
    double mean = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (mask[i]) {
            mean += a[i] - b[i];
            ++n;
        }
    mean /= std::max<size_t>(n, 1);
    for (size_t i = 0; i < a.size(); ++i)
        if (mask[i]) r[i] = a[i] - b[i] - mean;
    return r;
}

bool AllFinite(const Output& o) {
    for (const auto& p : o.Temporal.Pixels)
        if (!std::isfinite(p.J.R) || !std::isfinite(p.J.G) || !std::isfinite(p.J.B) || !std::isfinite(p.A) ||
            !std::isfinite(p.VarianceJ) || !std::isfinite(p.VarianceA))
            return false;
    for (const auto& p : o.Filtered)
        if (!std::isfinite(p.J.R) || !std::isfinite(p.J.G) || !std::isfinite(p.J.B) || !std::isfinite(p.ARaw) ||
            !std::isfinite(p.VarianceJ) || !std::isfinite(p.VarianceA))
            return false;
    return true;
}

// Static corner room at 96 x 64 with per-phase gathers (cycle of 6).
struct Room {
    StaticScene Scene;
    GatherSettings Gather;
    std::vector<std::vector<PixelGather>> Phases; // full period lcm(16, F)
    std::vector<PixelGather> S96;                 // deterministic S = 16 F
    [[nodiscard]] const std::vector<PixelGather>& At(uint64_t frame) const { return Phases[frame % Phases.size()]; }
};

Room MakeRoom(const Scene& scene, double radius, double thickness, uint32_t cycle = kCycle) {
    Room r{MakeStatic(scene), StratifiedGather(radius, thickness, 1, cycle), {}, {}};
    const uint32_t period = std::lcm(16U, cycle);
    for (uint64_t t = 0; t < period; ++t) r.Phases.push_back(GatherFrame(r.Scene.Input, r.Gather, t));
    r.S96 = GatherImage(r.Scene.Input, DeterministicGather(radius, thickness, 16 * cycle));
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// Rotation sequence ([B2]-derived requirement 1; section 12 permutation rules).

TEST(DiffuseGiTemporal, RotationSequenceIsAStratifiedBijection) {
    size_t configurations = 0;
    for (uint32_t log2 = 0; log2 <= kMaxRotationTileLog2; ++log2)
        for (uint32_t cycle = 1; cycle <= kMaxRotationCycle; cycle += (cycle < 8 ? 1 : 7)) {
            RotationSequence s{RotationMode::Stratified, log2, cycle, 0};
            const uint32_t tile = 1U << log2, tiles = tile * tile, strata = StratumCount(s);
            ASSERT_EQ(strata, tiles * cycle);
            std::vector<int> seen(strata, 0);
            std::vector<std::pair<uint32_t, uint32_t>> inverse(strata);
            for (uint32_t t = 0; t < cycle; ++t) {
                s.Frame = t + 7ULL * cycle; // only the phase matters
                std::set<uint32_t> coarse;
                for (uint32_t y = 0; y < tile; ++y)
                    for (uint32_t x = 0; x < tile; ++x) {
                        const uint32_t k = StratumIndex(s, x, y);
                        ASSERT_LT(k, strata);
                        ++seen[k];
                        inverse[k] = {y * tile + x, t};
                        coarse.insert(k / cycle);
                        ASSERT_EQ(k % cycle, t);
                        // Tiles repeat over the viewport.
                        ASSERT_EQ(StratumIndex(s, x + 3 * tile, y + 5 * tile), k);
                        ASSERT_LT(TileOrder(x, y, log2), tiles);
                    }
                ASSERT_EQ(coarse.size(), tiles) << "a frame must cover every coarse stratum";
            }
            for (uint32_t k = 0; k < strata; ++k) {
                ASSERT_EQ(seen[k], 1) << "tile x cycle must be a bijection onto the strata";
                // Inverse: (position, phase) -> k -> (position, phase).
                const auto [position, t] = inverse[k];
                s.Frame = t + 7ULL * cycle;
                ASSERT_EQ(StratumIndex(s, position % tile, position / tile), k);
            }
            const uint32_t p = LatticeMultiplier(strata);
            ASSERT_EQ(std::gcd(p, strata), 1U);
            std::vector<int> jitterSeen(strata, 0);
            for (uint32_t k = 0; k < strata; ++k) ++jitterSeen[(static_cast<uint64_t>(k) * p) % strata];
            for (uint32_t k = 0; k < strata; ++k) ASSERT_EQ(jitterSeen[k], 1);
            ++configurations;
        }
    // Standard 4 x 4 dispersed order and the [B2] configuration (4 x 4 x 6).
    const uint32_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) EXPECT_EQ(TileOrder(x, y, 2), bayer[y][x]);
    RotationSequence b2{RotationMode::Stratified, 2, 6, 0};
    std::vector<uint32_t> shifts;
    for (uint64_t t = 0; t < 6; ++t) {
        b2.Frame = t;
        shifts.push_back(FrameShift(b2));
    }
    EXPECT_EQ(shifts, (std::vector<uint32_t>{0, 0, 8, 8, 4, 4}));
    // Over T^2 = 16 frames every coarse stratum visits every tile position.
    for (uint32_t c = 0; c < 16; ++c) {
        std::set<uint32_t> positions;
        for (uint64_t t = 0; t < 16; ++t) {
            b2.Frame = t;
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x)
                    if (StratumIndex(b2, x, y) / 6 == c) positions.insert(y * 4 + x);
        }
        EXPECT_EQ(positions.size(), 16U);
    }
    // Spread of one pixel's cycle and correlation statistics of u.
    double minGap = 1.0, neighbour = 0.0, temporal = 0.0;
    size_t neighbourCount = 0, temporalCount = 0;
    GatherSettings g;
    g.Rotations = b2;
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            std::vector<double> us;
            for (uint64_t t = 0; t < 6; ++t) {
                g.Rotations.Frame = t;
                us.push_back(PixelRotation(g, x, y));
                if (t > 0) {
                    temporal += std::abs(us[t] - us[t - 1]);
                    ++temporalCount;
                }
            }
            std::sort(us.begin(), us.end());
            for (size_t i = 0; i < us.size(); ++i)
                minGap = std::min(minGap, i + 1 < us.size() ? us[i + 1] - us[i] : us[0] + 1.0 - us[i]);
        }
    for (uint64_t t = 0; t < 6; ++t) {
        g.Rotations.Frame = t;
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x + 1 < 16; ++x) {
                const double a = PixelRotation(g, x, y), b = PixelRotation(g, x + 1, y), c = PixelRotation(g, y, x),
                             d = PixelRotation(g, y, x + 1);
                // Circular distance on [0, 1).
                const auto dist = [](double u, double v) { return std::min(std::abs(u - v), 1.0 - std::abs(u - v)); };
                neighbour += dist(a, b) + dist(c, d);
                neighbourCount += 2;
            }
    }
    // FP32 twin of u is the float rounding of the double value.
    double f32 = 0.0;
    for (uint64_t t = 0; t < 6; ++t) {
        g.Rotations.Frame = t;
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                f32 = std::max(f32, std::abs(static_cast<double>(PixelRotationF32(g, x, y)) - PixelRotation(g, x, y)));
    }
    std::printf("DGI_M3 sequence configurations=%zu (tile 1-8, cycle 1-64) bijective inverse_ok lattice_ok | B2 4x4x6: "
                "shifts 0,0,8,8,4,4 (+XOR 1 on odd frames) every stratum at every position in 16 frames min_gap_in_one_pixel_cycle=%.4f (uniform 1/6=0.1667) mean_circular_neighbour_du=%.3f "
                "(independent 0.25) mean_frame_to_frame_du=%.3f f32_vs_f64=%.1e\n",
                configurations, minGap, neighbour / neighbourCount, temporal / temporalCount, f32);
    EXPECT_GT(minGap, 0.04);
    EXPECT_GT(neighbour / neighbourCount, 0.25);
    EXPECT_LT(f32, 1.0e-7);
}

TEST(DiffuseGiTemporal, StratifiedGatherEqualsDeterministicGatherPerPixel) {
    // Fixed mode is the M1/M2 path; a stratified pixel equals the deterministic
    // gather with Rotation = u(x, y) (same estimator, per-pixel rotation), and
    // a 1 x 1 tile with a cycle of 1 gives u = 1/2 everywhere.
    const StaticScene s = MakeStatic(CornerRoomScene(48, 32));
    GatherSettings fixed = DeterministicGather(2.5, 0.5, 2);
    const auto reference = GatherImage(s.Input, fixed);
    GatherSettings trivial = fixed;
    trivial.Rotations = {RotationMode::Stratified, 0, 1, 17};
    const auto same = GatherImage(s.Input, trivial);
    size_t mismatches = 0;
    for (size_t i = 0; i < reference.size(); ++i)
        mismatches += !(reference[i].J == same[i].J && reference[i].ARaw == same[i].ARaw);
    GatherSettings stratified = StratifiedGather(2.5, 0.5, 2);
    stratified.Rotations.Frame = 4;
    const auto image = GatherImage(s.Input, stratified);
    const auto imageF32 = GatherImageF32(s.Input, stratified);
    size_t checked = 0, perPixelMismatch = 0;
    double f32Mean = 0.0;
    for (uint32_t y = 0; y < 32; y += 3)
        for (uint32_t x = 0; x < 48; x += 5) {
            GatherSettings one = fixed;
            one.Rotation = PixelRotation(stratified, x, y);
            const PixelGather p = GatherPixel(s.Input, s.Geometry, x, y, one);
            const PixelGather& q = image[static_cast<size_t>(y) * 48 + x];
            perPixelMismatch += !(p.J == q.J && p.ARaw == q.ARaw);
            ++checked;
        }
    for (size_t i = 0; i < image.size(); ++i)
        f32Mean += std::abs(image[i].ARaw - imageF32[i].ARaw) / image.size();
    std::printf("DGI_M3 stratified_gather fixed_vs_tile1_cycle1_mismatches=%zu per_pixel_checked=%zu mismatches=%zu "
                "f32_mean_dA=%.1e\n",
                mismatches, checked, perPixelMismatch, f32Mean);
    EXPECT_EQ(mismatches, 0U);
    EXPECT_EQ(perPixelMismatch, 0U);
    EXPECT_LT(f32Mean, 2.0e-5);
    // Radial schedule with the lattice jitter: deterministic and finite.
    GatherSettings radial = stratified;
    radial.Schedule = CandidateSchedule::Radial;
    radial.CandidatesPerSide = 8;
    radial.SequenceRadialJitter = true;
    ASSERT_STREQ(ValidateGatherSettings(radial), "");
    const auto a = GatherImage(s.Input, radial), b = GatherImage(s.Input, radial);
    for (size_t i = 0; i < a.size(); ++i) {
        ASSERT_TRUE(a[i].J == b[i].J && std::isfinite(a[i].ARaw));
    }
    const uint32_t strata = StratumCount(radial.Rotations);
    EXPECT_NEAR(PixelRadialJitter(radial, 0, 0), (((StratumIndex(radial.Rotations, 0, 0) * uint64_t{LatticeMultiplier(strata)}) % strata) + 0.5) / strata, 1e-15);
}

// ---------------------------------------------------------------------------
// [B2] requirement 1: S = 1 per pixel, the 4 x 4 stratum reconstruction and 6
// frames of accumulation reproduce the deterministic S = 96 estimator, with
// no 4 x 4 pattern in the residual.

TEST(DiffuseGiTemporal, B2InterleavedRotationsReconstructS96) {
    for (const uint32_t cycle : {6U, 5U, 7U})
    for (const bool pillars : {false, true}) {
        const Room room = pillars ? MakeRoom(PillarsScene(96, 64), 3.0, 0.5, cycle)
                                  : MakeRoom(CornerRoomScene(96, 64), 2.5, 0.5, cycle);
        const char* name = pillars ? "pillars" : "corner_room";
        const uint32_t period = static_cast<uint32_t>(room.Phases.size());
        TemporalSettings t = DefaultTemporal();
        TemporalSettings noRecon = t;
        noRecon.Reconstruction = false;
        TemporalAccumulator acc, accNoRecon;
        uint64_t responses = 0;
        const TemporalFrame f0 = StaticFrame(room.Scene, room.Phases[0], 0);
        const auto interior = InteriorMask(f0);
        const size_t count = interior.size();
        std::vector<double> ref(count, 0.0), refA(count, 0.0);
        double meanRef = 0.0, p95 = 0.0;
        std::vector<double> lit;
        size_t n = 0;
        for (size_t i = 0; i < count; ++i)
            if (interior[i]) {
                ref[i] = Lum(room.S96[i].J), refA[i] = room.S96[i].ARaw;
                meanRef += ref[i];
                lit.push_back(ref[i]);
                ++n;
            }
        meanRef /= n;
        p95 = Summarize(lit).P95;
        // K * S96: the reference through the same reconstruction window (all 25
        // pixels compatible on interior pixels). Its tile frequencies are zero,
        // so (estimate - K * S96) isolates a pattern made by the sampling,
        // while (K * S96 - S96) is the blur bias of the reconstruction.
        std::vector<double> refK(count, 0.0), refKA(count, 0.0);
        double blurBias = 0.0, blurBiasA = 0.0;
        for (size_t i = 0; i < count; ++i) {
            if (!interior[i]) continue;
            const int x = static_cast<int>(i % 96), y = static_cast<int>(i / 96);
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const double w = (std::abs(dx) == 2 ? 0.5 : 1.0) * (std::abs(dy) == 2 ? 0.5 : 1.0) / 16.0;
                    const size_t q = static_cast<size_t>(y + dy) * 96 + (x + dx);
                    refK[i] += w * Lum(room.S96[q].J);
                    refKA[i] += w * room.S96[q].ARaw;
                }
            blurBias += std::abs(refK[i] - ref[i]) / (n * meanRef);
            blurBiasA += std::abs(refKA[i] - refA[i]) / n;
        }
        for (uint64_t frame = 0; frame < period; ++frame) {
            const TemporalFrame f = StaticFrame(room.Scene, room.At(frame), frame);
            const TemporalResult last = acc.Process(f, t);
            const TemporalResult lastNoRecon = accNoRecon.Process(f, noRecon);
            responses += last.Counters.ChangeResponses;
            if (frame + 1 != cycle && frame + 1 != period) continue;
            const auto filtered = FilterImage(MakeFilterInput(f, last), DefaultFilter());
            std::vector<double> img[4], imgA[4];
            for (auto& v : img) v.assign(count, 0.0);
            for (auto& v : imgA) v.assign(count, 0.0);
            double err[4] = {0, 0, 0, 0}, errA[4] = {0, 0, 0, 0};
            for (size_t i = 0; i < count; ++i) {
                if (!interior[i]) continue;
                img[0][i] = Lum(last.Pixels[i].J), imgA[0][i] = last.Pixels[i].A;
                img[1][i] = Lum(filtered[i].J), imgA[1][i] = filtered[i].ARaw;
                img[2][i] = Lum(lastNoRecon.Pixels[i].J), imgA[2][i] = lastNoRecon.Pixels[i].A;
                img[3][i] = Lum(room.At(frame)[i].J), imgA[3][i] = room.At(frame)[i].ARaw;
                for (int k = 0; k < 4; ++k) {
                    err[k] += std::abs(img[k][i] - ref[i]);
                    errA[k] += std::abs(imgA[k][i] - refA[i]);
                }
            }
            TileSpectrum tile[4], tileA[4];
            for (int k = 0; k < 4; ++k) {
                err[k] /= n * meanRef;
                errA[k] /= n;
                tile[k] = TileContent(Residual(img[k], refK, interior), 96, 64, n);
                tileA[k] = TileContent(Residual(imgA[k], refKA, interior), 96, 64, n);
            }
            std::printf("DGI_M3 b2_req1 F=%u scene=%s frames=%llu interior=%zu mean_J=%.4f p95_J=%.4f blur bias of the "
                        "window (K*S96 vs S96) J=%.4f A=%.4f | mean|dJ|/mean J vs S96: "
                        "recon=%.4f +filter=%.4f temporal_only=%.4f raw=%.4f | mean|dA|: %.4f %.4f %.4f %.4f | tile RMS vs K*S96 / p95 J: "
                        "%.5f %.5f %.5f %.5f (peak factor %.1f %.1f %.1f %.1f) | tile RMS A: %.5f %.5f %.5f %.5f | "
                        "responses=%llu\n",
                        cycle, name, static_cast<unsigned long long>(frame + 1), n, meanRef, p95, blurBias, blurBiasA, err[0], err[1], err[2], err[3],
                        errA[0], errA[1], errA[2], errA[3], tile[0].Rms / p95, tile[1].Rms / p95, tile[2].Rms / p95,
                        tile[3].Rms / p95, tile[0].PeakFactor, tile[1].PeakFactor, tile[2].PeakFactor, tile[3].PeakFactor,
                        tileA[0].Rms, tileA[1].Rms, tileA[2].Rms, tileA[3].Rms, static_cast<unsigned long long>(responses));
            if (cycle != kCycle) continue; // study only
            if (frame + 1 == cycle) {
                // One cycle: every one of the 16 F strata once per tile.
                EXPECT_LT(errA[0], 0.01);
                EXPECT_LT(4.0 * tileA[1].Rms, tileA[3].Rms);
            } else {
                // Full shift period: the tile component of the output is below
                // 0.5% of the bright GI level (p95 of S = 96) and 0.002 in A.
                EXPECT_LT(tile[1].Rms / p95, 0.005);
                EXPECT_LT(tileA[1].Rms, 0.002);
                EXPECT_LT(errA[1], 0.01);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// T14 and [B2] requirements 2-3: static scene, 128 frames after warm-up,
// temporal on and off, alpha min study, no blur growth (history pre-filter).

namespace {
struct RunStats {
    double TemporalStd = 0.0;    // mean per-pixel temporal std / mean J (final output)
    double TemporalStdA = 0.0;   // mean per-pixel temporal std of A (final output)
    double HistoryStd = 0.0;     // same, pre-filter history
    double Bias = 0.0;           // mean |mean_t out - S96| / mean J
    double FalseResponses = 0.0; // light responses per receiver-frame
    double Detail[3] = {0, 0, 0}; // gradient energy of the output / of S96 at frames 8, 32, 128
};

double GradientEnergy(const std::vector<double>& img, const std::vector<bool>& mask, uint32_t w, uint32_t h) {
    double e = 0.0;
    for (uint32_t y = 0; y + 1 < h; ++y)
        for (uint32_t x = 0; x + 1 < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!mask[i] || !mask[i + 1] || !mask[i + w]) continue;
            e += (img[i + 1] - img[i]) * (img[i + 1] - img[i]) + (img[i + w] - img[i]) * (img[i + w] - img[i]);
        }
    return e;
}

RunStats RunStatic(const Room& room, const TemporalSettings& t, const FilterSettings& fs, uint32_t warmup = 64,
                   uint32_t frames = 128) {
    TemporalAccumulator acc;
    TemporalFrame f0 = StaticFrame(room.Scene, room.Phases[0], 0);
    const auto interior = InteriorMask(f0);
    const size_t count = interior.size();
    std::vector<double> sum(count, 0.0), sum2(count, 0.0), sumA(count, 0.0), sumA2(count, 0.0), hs(count, 0.0),
        hs2(count, 0.0), ref(count, 0.0);
    double meanRef = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < count; ++i)
        if (interior[i]) {
            ref[i] = Lum(room.S96[i].J);
            meanRef += ref[i];
            ++n;
        }
    meanRef /= n;
    const double refEnergy = GradientEnergy(ref, interior, 96, 64);
    RunStats s;
    uint64_t responses = 0, receivers = 0;
    for (uint32_t frame = 0; frame < warmup + frames; ++frame) {
        const TemporalFrame f = StaticFrame(room.Scene, room.At(frame), frame);
        const Output o = Step(acc, f, t, fs);
        std::vector<double> img(count, 0.0);
        for (size_t i = 0; i < count; ++i)
            if (interior[i]) img[i] = Lum(o.Filtered[i].J);
        const int slot = frame == 7 ? 0 : (frame == 31 ? 1 : (frame == warmup + frames - 1 ? 2 : -1));
        if (slot >= 0) s.Detail[slot] = GradientEnergy(img, interior, 96, 64) / refEnergy;
        if (frame < warmup) continue;
        responses += o.Temporal.Counters.ChangeResponses;
        receivers += o.Temporal.Counters.Receivers;
        for (size_t i = 0; i < count; ++i) {
            if (!interior[i]) continue;
            const double h = Lum(o.Temporal.Pixels[i].J), a = o.Filtered[i].ARaw;
            sum[i] += img[i], sum2[i] += img[i] * img[i];
            sumA[i] += a, sumA2[i] += a * a;
            hs[i] += h, hs2[i] += h * h;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (!interior[i]) continue;
        const double m = sum[i] / frames, mA = sumA[i] / frames, mh = hs[i] / frames;
        s.TemporalStd += std::sqrt(std::max(sum2[i] / frames - m * m, 0.0));
        s.TemporalStdA += std::sqrt(std::max(sumA2[i] / frames - mA * mA, 0.0));
        s.HistoryStd += std::sqrt(std::max(hs2[i] / frames - mh * mh, 0.0));
        s.Bias += std::abs(m - ref[i]);
    }
    s.TemporalStd /= n * meanRef;
    s.TemporalStdA /= n;
    s.HistoryStd /= n * meanRef;
    s.Bias /= n * meanRef;
    s.FalseResponses = static_cast<double>(responses) / std::max<uint64_t>(receivers, 1);
    return s;
}
} // namespace

TEST(DiffuseGiTemporal, T14StabilityAndB2HistoryRequirements) {
    const Room room = MakeRoom(CornerRoomScene(96, 64), 2.5, 0.5);
    TemporalSettings off = DefaultTemporal();
    off.Enabled = false;
    const RunStats sOff = RunStatic(room, off, DefaultFilter());
    std::printf("DGI_M3 t14 temporal=off: output temporal std/mean J=%.5f std A=%.5f bias=%.4f detail(8,32,128)=%.3f %.3f %.3f\n",
                sOff.TemporalStd, sOff.TemporalStdA, sOff.Bias, sOff.Detail[0], sOff.Detail[1], sOff.Detail[2]);
    RunStats chosen;
    for (const double inverse : {8.0, 16.0, 32.0, 64.0}) {
        TemporalSettings t = DefaultTemporal();
        t.AlphaMin = 1.0 / inverse;
        const RunStats s = RunStatic(room, t, DefaultFilter());
        if (inverse == 32.0) chosen = s;
        std::printf("DGI_M3 t14 temporal=on alpha_min=1/%.0f: output std/mean J=%.5f std A=%.5f history std=%.5f "
                    "bias=%.4f false_responses=%.2e detail(8,32,128)=%.3f %.3f %.3f\n",
                    inverse, s.TemporalStd, s.TemporalStdA, s.HistoryStd, s.Bias, s.FalseResponses, s.Detail[0],
                    s.Detail[1], s.Detail[2]);
    }
    EXPECT_LT(chosen.TemporalStd, 0.25 * sOff.TemporalStd);
    EXPECT_LT(chosen.FalseResponses, 1.0e-2);
    // Requirement 3: no blur growth between 32 and 128 frames.
    EXPECT_GT(chosen.Detail[2], 0.9 * chosen.Detail[1]);
}

// ---------------------------------------------------------------------------
// T13: response to light changes on a static, well-sampled region.

TEST(DiffuseGiTemporal, T13LightResponse) {
    Scene onScene = CornerRoomScene(96, 64);
    Scene offScene = onScene;
    offScene.Primitives[2].Radiance = {}; // the red wall is the switched light
    const Room on = MakeRoom(onScene, 2.5, 0.5), off = MakeRoom(offScene, 2.5, 0.5);
    const TemporalFrame f0 = StaticFrame(on.Scene, on.Phases[0], 0);
    const auto interior = InteriorMask(f0);
    std::vector<bool> region(interior.size(), false);
    size_t regionCount = 0;
    for (size_t i = 0; i < region.size(); ++i)
        if (interior[i] && Lum(on.S96[i].J) > 0.05 && Lum(on.S96[i].J) > 2.0 * Lum(off.S96[i].J)) {
            region[i] = true;
            ++regionCount;
        }
    struct Change {
        const char* Name;
        double From, To;
    };
    const Change changes[] = {{"on", 0.0, 1.0}, {"off", 1.0, 0.0}, {"x2", 0.5, 1.0}, {"x0.5", 1.0, 0.5}, {"+20%", 0.8333333, 1.0}};
    for (const bool response : {true, false}) {
        for (const auto& change : changes) {
            TemporalSettings t = DefaultTemporal();
            if (!response) t.ChangeLow = 1.0e30, t.ChangeHigh = 2.0e30;
            const auto run = [&](double from, double to, uint32_t switchAt, uint32_t total) {
                TemporalAccumulator acc;
                std::vector<double> means, meansHistory;
                for (uint32_t frame = 0; frame < total; ++frame) {
                    const double s = frame < switchAt ? from : to;
                    const TemporalFrame f = StaticFrame(on.Scene, Blend(off.At(frame), on.At(frame), s), frame);
                    const Output o = Step(acc, f, t, DefaultFilter());
                    double m = 0.0, mh = 0.0;
                    for (size_t i = 0; i < region.size(); ++i)
                        if (region[i]) m += Lum(o.Filtered[i].J), mh += Lum(o.Temporal.Pixels[i].J);
                    means.push_back(m / regionCount);
                    meansHistory.push_back(mh / regionCount);
                }
                return std::make_pair(means, meansHistory);
            };
            const auto [series, history] = run(change.From, change.To, 64, 96);
            const auto [settled, settledHistory] = run(change.To, change.To, 0, 96);
            double target = 0.0, targetH = 0.0;
            for (int k = 0; k < 6; ++k) target += settled[90 + k] / 6.0, targetH += settledHistory[90 + k] / 6.0;
            const double before = series[63], beforeH = history[63];
            const auto frames90 = [](const std::vector<double>& v, double start, double end) {
                for (uint32_t k = 0; k < 32; ++k)
                    if (std::abs(v[64 + k] - end) <= 0.1 * std::abs(end - start)) return static_cast<int>(k + 1);
                return 99;
            };
            const int n = frames90(series, before, target), nh = frames90(history, beforeH, targetH);
            std::printf("DGI_M3 t13 response=%s change=%s region=%zu before=%.4f target=%.4f frames_to_90%%: output=%d "
                        "history=%d | fraction after 1,2,4,8 frames: %.3f %.3f %.3f %.3f\n",
                        response ? "on" : "off", change.Name, regionCount, before, target, n, nh,
                        (series[64] - before) / (target - before), (series[65] - before) / (target - before),
                        (series[67] - before) / (target - before), (series[71] - before) / (target - before));
            if (response && (std::string(change.Name) == "on" || std::string(change.Name) == "off")) EXPECT_LE(n, 8);
        }
    }
}

// Light-response thresholds: false responses on the static scene against the
// response to small and large changes.
TEST(DiffuseGiTemporal, LightResponseThresholdStudy) {
    Scene onScene = CornerRoomScene(96, 64);
    Scene offScene = onScene;
    offScene.Primitives[2].Radiance = {};
    const Room on = MakeRoom(onScene, 2.5, 0.5), off = MakeRoom(offScene, 2.5, 0.5);
    const auto interior = InteriorMask(StaticFrame(on.Scene, on.Phases[0], 0));
    std::vector<bool> region(interior.size(), false);
    size_t regionCount = 0;
    for (size_t i = 0; i < region.size(); ++i)
        if (interior[i] && Lum(on.S96[i].J) > 0.05 && Lum(on.S96[i].J) > 2.0 * Lum(off.S96[i].J)) region[i] = true, ++regionCount;
    struct Setting {
        double Low, High, Floor;
    };
    for (const Setting st : {Setting{2, 4, 0.02}, Setting{3, 6, 0.02}, Setting{3, 6, 0.05}, Setting{4, 8, 0.05}, Setting{4, 8, 0.1}}) {
        TemporalSettings t = DefaultTemporal();
        t.ChangeLow = st.Low, t.ChangeHigh = st.High, t.ChangeFloor = st.Floor;
        const RunStats s = RunStatic(on, t, DefaultFilter(), 48, 96);
        int frames[2] = {0, 0};
        const double from[2] = {0.8333333, 0.5};
        for (int c = 0; c < 2; ++c) {
            TemporalAccumulator acc, settledAcc;
            std::vector<double> series, settled;
            for (uint32_t frame = 0; frame < 96; ++frame) {
                const double sc = frame < 64 ? from[c] : 1.0;
                const Output o = Step(acc, StaticFrame(on.Scene, Blend(off.At(frame), on.At(frame), sc), frame), t,
                                      DefaultFilter());
                const Output q = Step(settledAcc, StaticFrame(on.Scene, on.At(frame), frame), t, DefaultFilter());
                double m = 0.0, mq = 0.0;
                for (size_t i = 0; i < region.size(); ++i)
                    if (region[i]) m += Lum(o.Filtered[i].J), mq += Lum(q.Filtered[i].J);
                series.push_back(m / regionCount);
                settled.push_back(mq / regionCount);
            }
            frames[c] = 99;
            for (uint32_t k = 0; k < 32; ++k)
                if (std::abs(series[64 + k] - settled[64 + k]) <= 0.1 * std::abs(settled[63] - series[63])) {
                    frames[c] = static_cast<int>(k + 1);
                    break;
                }
        }
        std::printf("DGI_M3 response_study z=%.0f..%.0f floor=%.2f: static false responses per receiver-frame=%.2e "
                    "output std/mean J=%.5f | frames to 90%%: +20%%=%d x2=%d\n",
                    st.Low, st.High, st.Floor, s.FalseResponses, s.TemporalStd, frames[0], frames[1]);
    }
}

// ---------------------------------------------------------------------------
// Sequences with motion (T11, T12, FP32 twin).

namespace {
SequenceScene RoomSequence(uint32_t w, uint32_t h, bool movingBox) {
    const Scene base = CornerRoomScene(w, h);
    SequenceScene s{base.Name, base.Camera, base.Primitives, std::vector<uint32_t>(base.Primitives.size(), 0U)};
    if (movingBox) {
        // A box beside the red wall (it receives strong red light), object 1.
        s.Primitives.push_back(Primitive::Box({0.0, 0.0, 0.0}, {0.35, 0.4, 0.35}, {0.05, 0.05, 0.05}));
        s.Object.push_back(1U);
    }
    return s;
}

struct SeqFrame {
    RenderedFrame Rendered;
    PreparedGeometry Geometry;
    std::vector<PixelGather> Gather;
};

SeqFrame RenderAndGather(const SequenceScene& scene, const SequenceFrame& now, const SequenceFrame& before,
                         const GatherSettings& g, uint64_t index) {
    SeqFrame f{RenderSequenceFrame(scene, now, before), {}, {}};
    f.Geometry = PrepareGeometry(f.Rendered.Input);
    f.Gather = GatherFrame(f.Rendered.Input, g, index);
    return f;
}

// Motion of the canonical convention (current to previous, UV, jitter
// included or excluded) rewritten in another declared convention.
void Convert(TemporalFrame& f, MotionDirection direction, MotionUnits units) {
    for (size_t i = 0; i < f.MotionVectors.size(); i += 2) {
        double mx = f.MotionVectors[i], my = f.MotionVectors[i + 1];
        if (units == MotionUnits::Pixels) mx *= f.Width, my *= f.Height;
        if (direction == MotionDirection::PreviousToCurrent) mx = -mx, my = -my;
        f.MotionVectors[i] = mx, f.MotionVectors[i + 1] = my;
    }
    f.Motion.Direction = direction;
    f.Motion.Units = units;
}
} // namespace

TEST(DiffuseGiTemporal, T11FlowSignUnitsAndJitter) {
    const uint32_t w = 96, h = 64;
    const SequenceScene scene = RoomSequence(w, h, false);
    const GatherSettings g = StratifiedGather(2.5, 0.5);
    // (a) Known translation: the camera moves +x so that the back wall
    // (front face z = -8) moves exactly 2 px per frame to the left.
    const double tx = scene.Camera.TanHalfY * w / h;
    const double step = 2.0 * (2.0 * tx * 8.0 / w);
    std::vector<SequenceFrame> frames(12);
    for (size_t k = 0; k < frames.size(); ++k) {
        frames[k].Camera.Position = {step * static_cast<double>(k), 0.0, 0.0};
        frames[k].JitterX = (k % 2 == 0) ? 0.25 : -0.25;
        frames[k].JitterY = (k % 2 == 0) ? -0.25 : 0.25;
    }
    std::vector<SeqFrame> rendered;
    for (size_t k = 0; k < frames.size(); ++k)
        rendered.push_back(RenderAndGather(scene, frames[k], frames[k == 0 ? 0 : k - 1], g, k));
    double wallMotion = 0.0, wallMotionMax = 0.0;
    size_t wallPixels = 0;
    const auto& r1 = rendered[1].Rendered;
    for (size_t i = 0; i < r1.SurfaceId.size(); ++i)
        if (r1.SurfaceId[i] == 2U && std::isfinite(r1.MotionExcluded[2 * i])) {
            const double px = r1.MotionExcluded[2 * i] * w;
            wallMotion += px;
            wallMotionMax = std::max(wallMotionMax, std::abs(px - 2.0));
            ++wallPixels;
        }
    // (b) Every declared convention reprojects to the same previous position.
    struct Variant {
        const char* Name;
        MotionDirection Direction;
        MotionUnits Units;
        JitterInMotion Jitter;
        JitterInMotion Declared;
    };
    const Variant variants[] = {
        {"c2p_uv_incl", MotionDirection::CurrentToPrevious, MotionUnits::NormalizedViewportUv, JitterInMotion::Included, JitterInMotion::Included},
        {"p2c_uv_incl", MotionDirection::PreviousToCurrent, MotionUnits::NormalizedViewportUv, JitterInMotion::Included, JitterInMotion::Included},
        {"c2p_px_incl", MotionDirection::CurrentToPrevious, MotionUnits::Pixels, JitterInMotion::Included, JitterInMotion::Included},
        {"p2c_px_incl", MotionDirection::PreviousToCurrent, MotionUnits::Pixels, JitterInMotion::Included, JitterInMotion::Included},
        {"c2p_uv_excl", MotionDirection::CurrentToPrevious, MotionUnits::NormalizedViewportUv, JitterInMotion::Excluded, JitterInMotion::Excluded},
        {"p2c_uv_excl", MotionDirection::PreviousToCurrent, MotionUnits::NormalizedViewportUv, JitterInMotion::Excluded, JitterInMotion::Excluded},
        {"c2p_px_excl", MotionDirection::CurrentToPrevious, MotionUnits::Pixels, JitterInMotion::Excluded, JitterInMotion::Excluded},
        {"p2c_px_excl", MotionDirection::PreviousToCurrent, MotionUnits::Pixels, JitterInMotion::Excluded, JitterInMotion::Excluded},
        // Wrong declarations (must be detectable).
        {"WRONG_sign", MotionDirection::PreviousToCurrent, MotionUnits::NormalizedViewportUv, JitterInMotion::Included, JitterInMotion::Included},
        {"WRONG_units_px_as_uv", MotionDirection::CurrentToPrevious, MotionUnits::Pixels, JitterInMotion::Included, JitterInMotion::Included},
        {"WRONG_jitter_excl_as_incl", MotionDirection::CurrentToPrevious, MotionUnits::NormalizedViewportUv, JitterInMotion::Excluded, JitterInMotion::Included},
        {"WRONG_jitter_incl_as_excl", MotionDirection::CurrentToPrevious, MotionUnits::NormalizedViewportUv, JitterInMotion::Included, JitterInMotion::Excluded},
    };
    std::vector<TemporalPixel> canonical;
    for (const Variant& v : variants) {
        for (const HistoryValidation mode : {HistoryValidation::PreviousPosition, HistoryValidation::FlowAndDepth}) {
            TemporalAccumulator acc;
            double maxError = 0.0, maxDiffJ = 0.0;
            uint64_t accepted = 0, visible = 0;
            for (size_t k = 0; k < frames.size(); ++k) {
                TemporalFrame f = MakeSequenceTemporalFrame(rendered[k].Rendered, rendered[k].Geometry, rendered[k].Gather, v.Jitter);
                f.FrameIndex = k;
                f.Validation = mode;
                const bool wrongUnits = std::string(v.Name) == "WRONG_units_px_as_uv";
                Convert(f, v.Direction, v.Units);
                if (wrongUnits) f.Motion.Units = MotionUnits::NormalizedViewportUv;
                if (std::string(v.Name) == "WRONG_sign") f.Motion.Direction = MotionDirection::CurrentToPrevious;
                f.Motion.Jitter = v.Declared;
                const TemporalResult r = acc.Process(f, DefaultTemporal());
                if (k == 0) continue;
                // Exact previous pixel position: projection of the exact previous
                // surface point through the previous (jittered) camera.
                const DepthCamera before = MakeDepthCamera(JitteredCamera(scene.Camera, frames[k - 1].JitterX, frames[k - 1].JitterY));
                const auto& rr = rendered[k].Rendered;
                const auto& prevIds = rendered[k - 1].Rendered.SurfaceId;
                for (size_t i = 0; i < r.Pixels.size(); ++i) {
                    const TemporalPixel& p = r.Pixels[i];
                    if (!p.Receiver || !std::isfinite(rr.PreviousPosition[3 * i])) continue;
                    const auto uv = ProjectToUv(before, {rr.PreviousPosition[3 * i], rr.PreviousPosition[3 * i + 1], rr.PreviousPosition[3 * i + 2]});
                    const double ex = uv[0] * w, ey = uv[1] * h;
                    if (!(ex >= 1.0 && ey >= 1.0 && ex < w - 1.0 && ey < h - 1.0)) continue;
                    const size_t pi = static_cast<size_t>(ey) * w + static_cast<size_t>(ex);
                    if (prevIds[pi] != rr.SurfaceId[i]) continue; // not visible before
                    ++visible;
                    accepted += p.HistoryAccepted;
                    maxError = std::max(maxError, std::hypot(p.PreviousX - ex, p.PreviousY - ey));
                    if (std::string(v.Name) != "c2p_uv_incl" && k == frames.size() - 1 && mode == HistoryValidation::PreviousPosition && !canonical.empty())
                        maxDiffJ = std::max(maxDiffJ, std::abs(p.J.G - canonical[i].J.G) / std::max(1e-6, std::abs(canonical[i].J.G)));
                }
                if (std::string(v.Name) == "c2p_uv_incl" && k == frames.size() - 1 && mode == HistoryValidation::PreviousPosition)
                    canonical = r.Pixels;
            }
            const bool wrong = std::string(v.Name).rfind("WRONG", 0) == 0;
            std::printf("DGI_M3 t11 convention=%s validation=%s max_reprojection_error_px=%.2e accepted=%.4f "
                        "(visible before: %llu) max_rel_dJ_vs_canonical=%.1e\n",
                        v.Name, mode == HistoryValidation::PreviousPosition ? "previous_position" : "flow_and_depth",
                        maxError, static_cast<double>(accepted) / std::max<uint64_t>(visible, 1),
                        static_cast<unsigned long long>(visible), maxDiffJ);
            if (!wrong) {
                EXPECT_LT(maxError, 1.0e-3);
                EXPECT_GT(static_cast<double>(accepted) / visible, 0.98);
                EXPECT_LT(maxDiffJ, 1.0e-6);
            } else {
                EXPECT_GT(maxError, 0.4);
            }
        }
    }
    std::printf("DGI_M3 t11 known_translation wall_pixels=%zu mean_motion_px=%.9f (expected +2) max_error_px=%.1e\n",
                wallPixels, wallMotion / std::max<size_t>(wallPixels, 1), wallMotionMax);
    EXPECT_LT(wallMotionMax, 1.0e-6);

    // (c) Alternating jitter in a still scene does not move the light. The
    // registration error e_t of frame t is the least-squares sub-pixel shift
    // between the output of the jittered run (sampled at x - j_t in unjittered
    // pixels) and the output of the unjittered run of the same frame:
    // A(x) - B(x) ~ grad B . (e_t - j_t) over interior floor pixels. The
    // jittered run without history (nothing to misregister) gives the noise
    // floor of the estimator.
    const SequenceScene still = RoomSequence(w, h, false);
    std::map<std::pair<int, uint64_t>, SeqFrame> cache;
    const auto frameAt = [&](int jitterState, uint64_t index) -> const SeqFrame& {
        const auto key = std::make_pair(jitterState, index % std::lcm<uint64_t>(16, kCycle));
        auto it = cache.find(key);
        if (it == cache.end()) {
            SequenceFrame now, before;
            const double j = jitterState == 0 ? 0.0 : (jitterState == 2 ? -0.25 : 0.25);
            now.JitterX = now.JitterY = j;
            before.JitterX = before.JitterY = jitterState == 3 ? j : -j; // 3: constant jitter
            it = cache.emplace(key, RenderAndGather(still, now, before, g, index)).first;
        }
        return it->second;
    };
    struct JitterCase {
        const char* Name;
        bool Jitter, Temporal;
        JitterInMotion Motion, Declared;
    };
    const JitterCase cases[] = {{"no_jitter", false, true, JitterInMotion::Included, JitterInMotion::Included},
                                {"no_jitter_without_history", false, false, JitterInMotion::Included, JitterInMotion::Included},
                                {"jittered_without_history", true, false, JitterInMotion::Included, JitterInMotion::Included},
                                {"constant_jitter_control", true, true, JitterInMotion::Included, JitterInMotion::Included},
                                {"included", true, true, JitterInMotion::Included, JitterInMotion::Included},
                                {"excluded_compensated", true, true, JitterInMotion::Excluded, JitterInMotion::Excluded},
                                {"WRONG_not_compensated", true, true, JitterInMotion::Excluded, JitterInMotion::Included},
                                {"WRONG_compensated_twice", true, true, JitterInMotion::Included, JitterInMotion::Excluded}};
    std::vector<std::vector<double>> plain[2]; // [temporal]
    std::vector<bool> mask;
    double floorAlternation = 0.0, floorOffset = 0.0;
    for (const JitterCase& c : cases) {
        TemporalAccumulator acc;
        TemporalSettings ts = DefaultTemporal();
        ts.Enabled = c.Temporal;
        double meanError = 0.0, meanSigned[2] = {0.0, 0.0}, blur = 0.0;
        int measured = 0;
        for (uint64_t k = 0; k < 96; ++k) {
            const bool constant = std::string(c.Name) == "constant_jitter_control";
            const int state = !c.Jitter ? 0 : (constant ? 3 : (k % 2 == 0 ? 1 : 2));
            const SeqFrame& sf = frameAt(state, k);
            TemporalFrame f = MakeSequenceTemporalFrame(sf.Rendered, sf.Geometry, sf.Gather, c.Motion);
            f.FrameIndex = k;
            f.Motion.Jitter = c.Declared;
            const Output o = Step(acc, f, ts, DefaultFilter());
            std::vector<double> img(o.Filtered.size());
            for (size_t i = 0; i < img.size(); ++i) img[i] = Lum(o.Filtered[i].J);
            if (!c.Jitter) {
                if (mask.empty()) {
                    mask = InteriorMask(f, 3);
                    for (size_t i = 0; i < mask.size(); ++i) mask[i] = mask[i] && sf.Rendered.SurfaceId[i] == 1U;
                }
                plain[c.Temporal ? 1 : 0].push_back(img);
                continue;
            }
            if (k < 48) continue;
            const auto& b = plain[c.Temporal ? 1 : 0][k];
            // Joint fit A - B ~ grad B . s + kappa lap B (shift and blur).
            double m[3][4] = {};
            for (uint32_t y = 1; y + 1 < h; ++y)
                for (uint32_t x = 1; x + 1 < w; ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (!mask[i]) continue;
                    const double r[3] = {0.5 * (b[i + 1] - b[i - 1]), 0.5 * (b[i + w] - b[i - w]),
                                         b[i + 1] + b[i - 1] + b[i + w] + b[i - w] - 4.0 * b[i]};
                    const double d = img[i] - b[i];
                    for (int u = 0; u < 3; ++u) {
                        for (int v = 0; v < 3; ++v) m[u][v] += r[u] * r[v];
                        m[u][3] += r[u] * d;
                    }
                }
            for (int c0 = 0; c0 < 3; ++c0) // Gauss-Jordan (normal matrix is positive definite)
                for (int r0 = 0; r0 < 3; ++r0) {
                    if (r0 == c0) continue;
                    const double f = m[r0][c0] / m[c0][c0];
                    for (int k = c0; k < 4; ++k) m[r0][k] -= f * m[c0][k];
                }
            const double sx = m[0][3] / m[0][0], sy = m[1][3] / m[1][1];
            blur += m[2][3] / m[2][2];
            const double ex = -(sx + sf.Rendered.JitterX), ey = -(sy + sf.Rendered.JitterY); // A - B ~ -grad F . (j + e)
            meanError += std::hypot(ex, ey);
            meanSigned[k % 2] += ex + ey;
            ++measured;
        }
        if (!c.Jitter) continue;
        meanError /= measured;
        const double even = meanSigned[0] / (measured / 2), odd = meanSigned[1] / (measured / 2);
        const double alternation = 0.5 * std::abs(odd - even), offset = 0.5 * (odd + even);
        if (!c.Temporal) floorAlternation = alternation, floorOffset = offset;
        // Alternation: the light moving back and forth with the jitter; offset:
        // constant displacement against the unjittered run of the same processing.
        std::printf("DGI_M3 t11 jitter case=%s registration along x+y: alternation=%.4f px offset=%+.4f px "
                    "(even %+.4f, odd %+.4f, mean |e| %.4f) extra blur=%.3f px^2\n",
                    c.Name, alternation, offset, even, odd, meanError, blur / measured);
        const bool wrong = std::string(c.Name).rfind("WRONG", 0) == 0;
        if (c.Temporal && !wrong) EXPECT_LT(alternation, floorAlternation + 0.03);
        if (std::string(c.Name) == "constant_jitter_control") EXPECT_LT(std::abs(offset - floorOffset), 0.1);
        if (wrong) EXPECT_GT(alternation, floorAlternation + 0.15);
    }
}

TEST(DiffuseGiTemporal, T12DisocclusionRejectsHistoryForJAndA) {
    const uint32_t w = 96, h = 64;
    const SequenceScene scene = RoomSequence(w, h, true);
    const GatherSettings g = StratifiedGather(2.5, 0.5);
    std::vector<SequenceFrame> frames(40);
    for (size_t k = 0; k < frames.size(); ++k) {
        frames[k].Objects.resize(2);
        const double travel = k < 16 ? 0.0 : 0.12 * static_cast<double>(k - 16);
        frames[k].Objects[1].Position = {-2.3 + travel, -0.6, -5.2};
    }
    std::vector<SeqFrame> rendered;
    for (size_t k = 0; k < frames.size(); ++k)
        rendered.push_back(RenderAndGather(scene, frames[k], frames[k == 0 ? 0 : k - 1], g, k));
    for (const bool ids : {true, false})
        for (const HistoryValidation mode : {HistoryValidation::PreviousPosition, HistoryValidation::FlowAndDepth})
            for (const double sine : {0.02, 0.05, 0.2}) {
                TemporalAccumulator acc;
                TemporalSettings t = DefaultTemporal();
                t.PlaneSine = sine;
                uint64_t revealed = 0, foreignTaps = 0, freshExactJ = 0, freshExactA = 0, rejected = 0, moving = 0, movingAccepted = 0;
                uint64_t staticVisible = 0, staticAccepted = 0;
                double staleJ = 0.0, staleA = 0.0;
                for (size_t k = 0; k < frames.size(); ++k) {
                    TemporalFrame f = MakeSequenceTemporalFrame(rendered[k].Rendered, rendered[k].Geometry, rendered[k].Gather);
                    f.FrameIndex = k;
                    f.Validation = mode;
                    if (!ids) f.SurfaceId.clear();
                    const TemporalResult r = acc.Process(f, t);
                    if (k < 17) continue;
                    const auto& now = rendered[k].Rendered;
                    const auto& before = rendered[k - 1].Rendered;
                    const DepthCamera camera = before.Input.Camera;
                    for (size_t i = 0; i < r.Pixels.size(); ++i) {
                        const TemporalPixel& p = r.Pixels[i];
                        if (!p.Receiver || !std::isfinite(now.PreviousPosition[3 * i])) continue;
                        const auto uv = ProjectToUv(camera, {now.PreviousPosition[3 * i], now.PreviousPosition[3 * i + 1], now.PreviousPosition[3 * i + 2]});
                        const int ex = static_cast<int>(std::floor(uv[0] * w)), ey = static_cast<int>(std::floor(uv[1] * h));
                        if (ex < 0 || ey < 0 || ex >= static_cast<int>(w) || ey >= static_cast<int>(h)) continue;
                        const bool box = now.SurfaceId[i] == static_cast<uint32_t>(scene.Primitives.size());
                        if (box) {
                            ++moving;
                            movingAccepted += p.HistoryAccepted;
                        }
                        const uint32_t before_id = before.SurfaceId[static_cast<size_t>(ey) * w + ex];
                        if (before_id == now.SurfaceId[i]) {
                            if (!box) ++staticVisible, staticAccepted += p.HistoryAccepted;
                            continue;
                        }
                        // Revealed at this frame: no accepted tap may lie on another surface.
                        ++revealed;
                        bool foreign = false;
                        for (uint32_t b = 0; b < 2; ++b)
                            for (uint32_t a = 0; a < 2; ++a) {
                                if (!(p.TapMask & (1U << (a + 2 * b)))) continue;
                                const int tx = p.TapX + static_cast<int>(a), ty = p.TapY + static_cast<int>(b);
                                foreign = foreign || before.SurfaceId[static_cast<size_t>(ty) * w + tx] != now.SurfaceId[i];
                            }
                        foreignTaps += foreign;
                        if (!p.HistoryAccepted) {
                            ++rejected;
                            freshExactJ += p.J == p.Current;
                            freshExactA += p.A == p.CurrentA;
                        }
                        // Stale light: excess of J over the current sample, relative to the box's J.
                        staleJ = std::max(staleJ, (Lum(p.J) - Lum(p.Current)) / std::max(Lum(p.Current), 1e-3));
                        staleA = std::max(staleA, std::abs(p.A - p.CurrentA));
                    }
                }
                std::printf("DGI_M3 t12 ids=%d validation=%s plane_sine=%.2f revealed=%llu with_foreign_taps=%llu rejected=%llu "
                            "(J fresh exact %llu, A fresh exact %llu) max excess J over current=%.3f max |A - current A|=%.3f | "
                            "static visible accepted=%.4f | moving box pixels accepted=%.3f\n",
                            ids, mode == HistoryValidation::PreviousPosition ? "previous_position" : "flow_and_depth", sine,
                            static_cast<unsigned long long>(revealed), static_cast<unsigned long long>(foreignTaps),
                            static_cast<unsigned long long>(rejected), static_cast<unsigned long long>(freshExactJ),
                            static_cast<unsigned long long>(freshExactA), staleJ, staleA,
                            static_cast<double>(staticAccepted) / std::max<uint64_t>(staticVisible, 1),
                            static_cast<double>(movingAccepted) / std::max<uint64_t>(moving, 1));
                if (sine == 0.05) {
                    EXPECT_GT(revealed, 50U);
                    if (ids) EXPECT_EQ(foreignTaps, 0U);
                    else EXPECT_LE(foreignTaps * 100, revealed);
                    EXPECT_EQ(freshExactJ, rejected);
                    EXPECT_EQ(freshExactA, rejected);
                    EXPECT_GT(static_cast<double>(staticAccepted) / staticVisible, 0.99);
                }
            }
}

TEST(DiffuseGiTemporal, T15ResetsWithoutLeaksOrImpulses) {
    const Room room = MakeRoom(CornerRoomScene(96, 64), 2.5, 0.5);
    const Room other = MakeRoom(PillarsScene(96, 64), 3.0, 0.5);
    const TemporalSettings t = DefaultTemporal();
    const FilterSettings fs = DefaultFilter();
    const auto fresh = [&](const Room& r, uint64_t frame) {
        TemporalAccumulator a;
        return Step(a, StaticFrame(r.Scene, r.At(frame), frame), t, fs);
    };
    const auto sameJ = [](const Output& a, const Output& b) {
        for (size_t i = 0; i < a.Filtered.size(); ++i)
            if (!(a.Filtered[i].J == b.Filtered[i].J) || a.Filtered[i].ARaw != b.Filtered[i].ARaw) return false;
        return true;
    };
    // First activation.
    TemporalAccumulator acc;
    const Output first = Step(acc, StaticFrame(room.Scene, room.At(0), 0), t, fs);
    size_t neutral = 0, receivers = 0;
    bool historyFinite = true;
    for (const double v : acc.History()) historyFinite = historyFinite && std::isfinite(v);
    for (const auto& p : first.Temporal.Pixels) {
        receivers += p.Receiver;
        if (!p.Receiver) neutral += p.J == Rgb{} && p.A == 1.0;
    }
    EXPECT_EQ(first.Temporal.ResetReasons, kResetFirstFrame);
    EXPECT_EQ(first.Temporal.Counters.Accepted, 0U);
    EXPECT_TRUE(historyFinite);
    EXPECT_EQ(neutral + receivers, first.Temporal.Pixels.size());
    for (uint64_t k = 1; k < 32; ++k) Step(acc, StaticFrame(room.Scene, room.At(k), k), t, fs);
    // Camera cut to another scene: no history, identical to a first frame.
    TemporalFrame cut = StaticFrame(other.Scene, other.At(32), 32);
    cut.CameraCut = true;
    TemporalAccumulator noFlag;
    for (uint64_t k = 0; k < 32; ++k) noFlag.Process(StaticFrame(room.Scene, room.At(k), k), t);
    TemporalFrame unflagged = cut;
    unflagged.CameraCut = false;
    const TemporalResult leak = noFlag.Process(unflagged, t);
    const Output afterCut = Step(acc, cut, t, fs);
    EXPECT_TRUE(afterCut.Temporal.ResetReasons & kResetCameraCut);
    EXPECT_EQ(afterCut.Temporal.Counters.Accepted, 0U);
    EXPECT_TRUE(sameJ(afterCut, fresh(other, 32)));
    // Exposure x4 (signal pre-exposed): rescaled history, no impulse.
    TemporalAccumulator a1, a2, a3, a4;
    double impulse[3] = {0, 0, 0};
    for (uint64_t k = 0; k < 40; ++k) {
        const TemporalFrame base = StaticFrame(room.Scene, room.At(k), k);
        TemporalFrame exposed = base;
        if (k >= 32) {
            ScaleFrame(exposed, 4.0);
            exposed.PreExposure = 4.0;
        }
        TemporalSettings invalidate = t;
        invalidate.Exposure = ExposureChange::Invalidate;
        TemporalFrame undeclared = exposed;
        undeclared.PreExposure = 1.0;
        const Output r0 = Step(a1, base, t, fs), r1 = Step(a2, exposed, t, fs), r2 = Step(a3, exposed, invalidate, fs),
                     r3 = Step(a4, undeclared, t, fs);
        if (k < 32) continue;
        double m0 = 0, m[3] = {0, 0, 0};
        for (size_t i = 0; i < r0.Filtered.size(); ++i) {
            m0 += Lum(r0.Filtered[i].J);
            m[0] += Lum(r1.Filtered[i].J) / 4.0, m[1] += Lum(r2.Filtered[i].J) / 4.0, m[2] += Lum(r3.Filtered[i].J) / 4.0;
        }
        for (int c = 0; c < 3; ++c) impulse[c] = std::max(impulse[c], std::abs(m[c] - m0) / m0);
        if (k == 32) EXPECT_TRUE(r2.Temporal.ResetReasons & kResetExposure);
    }
    // Resize: reset, no history read, everything finite.
    const Room small = MakeRoom(CornerRoomScene(80, 48), 2.5, 0.5);
    const Output resized = Step(acc, StaticFrame(small.Scene, small.At(33), 33), t, fs);
    EXPECT_TRUE(resized.Temporal.ResetReasons & kResetResize);
    EXPECT_EQ(resized.Temporal.Counters.Accepted, 0U);
    EXPECT_TRUE(AllFinite(resized));
    // Two views: separate histories equal solo runs; one shared history resets
    // at every view change and never mixes the views.
    TemporalAccumulator view0, view1, solo0, solo1, shared;
    bool independent = true, noLeak = true;
    uint64_t sharedAccepted = 0;
    for (uint64_t k = 0; k < 24; ++k) {
        TemporalFrame f0 = StaticFrame(room.Scene, room.At(k), k), f1 = StaticFrame(other.Scene, other.At(k), k);
        f0.ViewId = 0, f1.ViewId = 1;
        const Output v0 = Step(view0, f0, t, fs), v1 = Step(view1, f1, t, fs);
        const Output s0 = Step(solo0, f0, t, fs), s1 = Step(solo1, f1, t, fs);
        independent = independent && sameJ(v0, s0) && sameJ(v1, s1);
        const Output x = Step(shared, k % 2 == 0 ? f0 : f1, t, fs);
        sharedAccepted += x.Temporal.Counters.Accepted;
        if (k > 0) noLeak = noLeak && (x.Temporal.ResetReasons & kResetViewChange) && sameJ(x, fresh(k % 2 == 0 ? room : other, k));
    }
    // Transport change and explicit invalidation.
    TemporalFrame transport = StaticFrame(room.Scene, room.At(40), 40);
    transport.TransportKey = 7;
    const uint32_t transportReasons = acc.Process(transport, t).ResetReasons;
    acc.Invalidate();
    const uint32_t invalidated = acc.Process(StaticFrame(room.Scene, room.At(41), 41), t).ResetReasons;
    std::printf("DGI_M3 t15 first_frame reasons=%u neutral_non_receivers=%zu history_finite=%d | cut: accepted=%llu "
                "identical_to_first_frame=1; without the cut flag %llu of %llu receivers accept history | exposure x4: "
                "max relative jump of mean J/e rescale=%.2e invalidate=%.2e undeclared=%.2e | resize reasons=%u accepted=%llu "
                "| views: separate=solo %d, shared accepted=%llu no_leak=%d | transport reasons=%u invalidate reasons=%u\n",
                first.Temporal.ResetReasons, neutral, historyFinite,
                static_cast<unsigned long long>(afterCut.Temporal.Counters.Accepted),
                static_cast<unsigned long long>(leak.Counters.Accepted), static_cast<unsigned long long>(leak.Counters.Receivers),
                impulse[0], impulse[1], impulse[2], resized.Temporal.ResetReasons,
                static_cast<unsigned long long>(resized.Temporal.Counters.Accepted), independent,
                static_cast<unsigned long long>(sharedAccepted), noLeak, transportReasons, invalidated);
    EXPECT_LT(impulse[0], 1.0e-12);
    EXPECT_LT(impulse[1], 0.05);
    EXPECT_TRUE(independent);
    EXPECT_EQ(sharedAccepted, 0U);
    EXPECT_TRUE(noLeak);
    EXPECT_TRUE(transportReasons & kResetTransport);
    EXPECT_TRUE(invalidated & kResetExplicit);
}

TEST(DiffuseGiTemporal, T16RobustnessOddExtentsExtremeCountsNonFinite) {
    size_t runs = 0, nonFinite = 0, badNeutral = 0;
    for (const auto& extent : std::vector<std::pair<uint32_t, uint32_t>>{{1, 1}, {1, 7}, {7, 1}, {3, 2}, {13, 11}, {37, 23}})
        for (const uint32_t log2 : {0U, 2U, 3U})
            for (const uint32_t levels : {0U, 1U, 8U})
                for (const Precision precision : {Precision::Double, Precision::Float32}) {
                    const StaticScene s = MakeStatic(CornerRoomScene(extent.first, extent.second));
                    GatherSettings g = StratifiedGather(2.5, 0.5);
                    g.Rotations.TileLog2 = log2;
                    g.Rotations.Cycle = log2 == 3 ? 64 : 1;
                    TemporalSettings t = DefaultTemporal();
                    t.TileLog2 = log2;
                    t.AlphaMin = log2 == 0 ? 1.0 : 1.0 / 32.0;
                    FilterSettings fs = DefaultFilter();
                    fs.Levels = levels;
                    TemporalAccumulator acc(precision);
                    for (uint64_t k = 0; k < 4; ++k) {
                        TemporalFrame f = StaticFrame(s, GatherFrame(s.Input, g, k + (1ULL << 62)), k);
                        const size_t count = f.Depth.size();
                        // Poison: non-finite raw, NaN normal, invalid depth, non-finite or huge motion,
                        // NaN previous position.
                        if (count > 2) {
                            f.Raw[4 * (count / 2)] = std::numeric_limits<double>::infinity();
                            f.Raw[4 * (count / 3) + 3] = kNaN;
                            f.Normal[3 * (count - 1)] = kNaN;
                            f.Depth[0] = k == 2 ? -1.0 : f.Depth[0];
                            f.MotionVectors[2 * (count / 2)] = 1.0e30;
                            f.MotionVectors[2 * (count / 4) + 1] = kNaN;
                            f.PreviousPosition[3 * (count / 5)] = kNaN;
                        }
                        const Output o = Step(acc, f, t, fs, precision);
                        nonFinite += !AllFinite(o);
                        for (size_t i = 0; i < o.Filtered.size(); ++i)
                            if (!o.Filtered[i].Receiver)
                                badNeutral += !(o.Filtered[i].J == Rgb{} && o.Filtered[i].ARaw == 1.0 && o.Filtered[i].VarianceJ == 0.0);
                        ++runs;
                    }
                }
    std::printf("DGI_M3 t16 cpu frames=%zu (extents 1x1..37x23, tiles 1/4/8, cycles 1/64, levels 0/1/8, frame index 2^62, "
                "double and FP32, poisoned inputs) non_finite_frames=%zu non_neutral_non_receivers=%zu\n",
                runs, nonFinite, badNeutral);
    EXPECT_EQ(nonFinite, 0U);
    EXPECT_EQ(badNeutral, 0U);
    // Invalid declarations are rejected before any work.
    TemporalFrame bad;
    EXPECT_STRNE(ValidateTemporalFrame(bad), "");
    TemporalSettings badSettings;
    badSettings.ChangeHigh = badSettings.ChangeLow;
    EXPECT_STRNE(ValidateTemporalSettings(badSettings), "");
    TemporalAccumulator acc;
    EXPECT_TRUE(acc.Process(bad, DefaultTemporal()).Pixels.empty());
    EXPECT_FALSE(acc.HasHistory());
}

TEST(DiffuseGiTemporal, Fp32TwinAgainstDoubleOverAMovingSequence) {
    const SequenceScene scene = RoomSequence(97, 61, true);
    const GatherSettings g = StratifiedGather(2.5, 0.5);
    TemporalAccumulator d(Precision::Double), f32(Precision::Float32);
    std::vector<SequenceFrame> frames(30);
    for (size_t k = 0; k < frames.size(); ++k) {
        frames[k].Objects.resize(2);
        frames[k].Objects[1].Position = {-2.3 + 0.07 * k, -0.6, -5.2};
        frames[k].Camera.Position = {0.01 * k, 0.0, -0.02 * k};
        frames[k].Camera.Yaw = 0.002 * k;
    }
    double maxJ = 0.0, meanJ = 0.0, maxA = 0.0, maxOut = 0.0;
    size_t flips = 0, count = 0;
    for (size_t k = 0; k < frames.size(); ++k) {
        const SeqFrame sf = RenderAndGather(scene, frames[k], frames[k == 0 ? 0 : k - 1], g, k);
        TemporalFrame f = MakeSequenceTemporalFrame(sf.Rendered, sf.Geometry, sf.Gather);
        f.FrameIndex = k;
        const Output a = Step(d, f, DefaultTemporal(), DefaultFilter(), Precision::Double);
        const Output b = Step(f32, f, DefaultTemporal(), DefaultFilter(), Precision::Float32);
        for (size_t i = 0; i < a.Temporal.Pixels.size(); ++i) {
            const auto& p = a.Temporal.Pixels[i];
            const auto& q = b.Temporal.Pixels[i];
            if (!p.Valid) continue;
            flips += p.HistoryAccepted != q.HistoryAccepted;
            const double scale = std::max(1e-3, Lum(p.J));
            const double dj = std::abs(Lum(p.J) - Lum(q.J)) / scale;
            maxJ = std::max(maxJ, dj);
            meanJ += dj;
            maxA = std::max(maxA, std::abs(p.A - q.A));
            maxOut = std::max(maxOut, std::abs(Lum(a.Filtered[i].J) - Lum(b.Filtered[i].J)) / std::max(1e-3, Lum(a.Filtered[i].J)));
            ++count;
        }
    }
    std::printf("DGI_M3 fp32_twin frames=30 extent=97x61 pixels=%zu accept_flips=%zu history: mean rel dJ=%.2e max=%.2e "
                "max dA=%.2e | filtered max rel dJ=%.2e\n",
                count, flips, meanJ / count, maxJ, maxA, maxOut);
    EXPECT_LT(meanJ / count, 1.0e-4);
    EXPECT_LT(static_cast<double>(flips), 1.0e-3 * count);
}

TEST(DiffuseGiTemporal, FilterKeepsEdgesChromaAndScales) {
    // Synthetic filter input: a plane with an equal-luminance red/green split
    // (x < 24) and a second plane 30% nearer (y >= 20) with the same J.
    const uint32_t w = 48, h = 32;
    FilterInput in;
    in.Width = w, in.Height = h;
    const DepthCamera camera = MakeDepthCamera(StandardCamera(w, h));
    in.ViewX = camera.ViewX, in.ViewY = camera.ViewY;
    const size_t count = static_cast<size_t>(w) * h;
    in.Depth.assign(count, 5.0);
    in.Normal.assign(3 * count, 0.0);
    in.J.assign(3 * count, 0.0);
    in.A.assign(count, 0.8);
    in.VarianceJ.assign(count, 1.0e-4);
    in.VarianceA.assign(count, 1.0e-5);
    const double green = kLumaR * 0.5 / kLumaG; // same luminance as R = 0.5
    for (size_t i = 0; i < count; ++i) {
        const uint32_t x = i % w, y = static_cast<uint32_t>(i / w);
        in.Normal[3 * i + 2] = 1.0;
        if (y >= 20) in.Depth[i] = 3.5;
        if (x < 24) in.J[3 * i] = 0.5;
        else in.J[3 * i + 1] = green;
        if (y >= 20) in.A[i] = 0.3;
        if (x == 5 && y == 5) in.Normal[3 * i + 2] = 0.0; // null normal
        if (x == 6 && y == 5) in.Depth[i] = kNaN;            // invalid depth
        if (x == 7 && y == 5) in.J[3 * i] = std::numeric_limits<double>::infinity();
    }
    FilterSettings fs = DefaultFilter();
    fs.Levels = 5;
    const auto out = FilterImage(in, fs);
    double chromaLeak = 0.0, depthLeakA = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t x = i % w, y = static_cast<uint32_t>(i / w);
        if (!out[i].Receiver || (y >= 4 && y <= 6 && x >= 4 && x <= 8)) continue;
        const double wrong = x < 24 ? out[i].J.G : out[i].J.R;
        chromaLeak = std::max(chromaLeak, wrong / 0.5);
        depthLeakA = std::max(depthLeakA, std::abs(out[i].ARaw - (y >= 20 ? 0.3 : 0.8)));
    }
    const FilteredPixel& nullNormal = out[5 * w + 5];
    const FilteredPixel& nanDepth = out[5 * w + 6];
    // HDR scale: J x8, variance x64 -> output x8.
    FilterInput scaled = in;
    for (auto& v : scaled.J) v *= 8.0;
    for (auto& v : scaled.VarianceJ) v *= 64.0;
    const auto out8 = FilterImage(scaled, fs);
    double scaleError = 0.0;
    for (size_t i = 0; i < count; ++i)
        if (out[i].Receiver && std::isfinite(out[i].J.R))
            scaleError = std::max(scaleError, std::abs(out8[i].J.R - 8.0 * out[i].J.R) / std::max(1e-12, 8.0 * out[i].J.R));
    std::printf("DGI_M3 filter chroma_leak_across_equal_luminance_edge=%.2e A_leak_across_depth_step=%.2e "
                "null_normal_neutral=%d invalid_depth_neutral=%d hdr_x8_max_rel_error=%.1e\n",
                chromaLeak, depthLeakA, !nullNormal.Receiver && nullNormal.ARaw == 1.0,
                !nanDepth.Receiver && nanDepth.J == Rgb{}, scaleError);
    EXPECT_LT(chromaLeak, 0.01);
    EXPECT_LT(depthLeakA, 1.0e-9);
    EXPECT_FALSE(nullNormal.Receiver);
    EXPECT_FALSE(nanDepth.Receiver);
    EXPECT_LT(scaleError, 1.0e-12);
}

TEST(DiffuseGiTemporal, FilterLevelAndSigmaStudy) {
    const Room room = MakeRoom(PillarsScene(96, 64), 3.0, 0.5);
    const auto interior = InteriorMask(StaticFrame(room.Scene, room.At(0), 0));
    double meanRef = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < interior.size(); ++i)
        if (interior[i]) meanRef += Lum(room.S96[i].J), ++n;
    meanRef /= n;
    for (const double sigma : {1.0, 4.0, 16.0})
        for (const uint32_t levels : {0U, 2U, 4U, 5U}) {
            if (sigma != 4.0 && levels != 4U) continue;
            FilterSettings fs = DefaultFilter();
            fs.Levels = levels;
            fs.LuminanceSigma = sigma;
            const RunStats s = RunStatic(room, DefaultTemporal(), fs, 48, 48);
            std::printf("DGI_M3 filter_study sigma=%.0f levels=%u: output std/mean J=%.5f bias vs S96=%.4f detail(128)=%.3f\n",
                        sigma, levels, s.TemporalStd, s.Bias, s.Detail[2]);
        }
}

TEST(DiffuseGiTemporal, OctahedralFaceNormalGuideMeetsTheSelfIntersectionBudget) {
    // Open point 6: the renderer's geometry guide stores the face normal as
    // octahedral unorm16x2 (renderer3ds/pica_geometry_guide.h). Angle against
    // the exact plane normal and A bias on the sloped planes of M2.
    for (const double pitchDegrees : {30.0, 60.0, 75.0}) {
        const Scene scene = SlopedPlaneScene(128, 96, pitchDegrees * kPi / 180.0);
        GatherInput exact = Rasterize(scene);
        exact.NormalSource = GeometricNormalSource::RendererFaceNormal;
        GatherInput encoded = exact;
        double maxAngle = 0.0;
        for (size_t i = 0; i + 2 < encoded.FaceNormals.size(); i += 3) {
            const std::array<float, 3> n{encoded.FaceNormals[i], encoded.FaceNormals[i + 1], encoded.FaceNormals[i + 2]};
            const auto d = Fast::Renderer3ds::DecodePicaGeometryGuideNormal(Fast::Renderer3ds::EncodePicaGeometryGuideNormal(n));
            encoded.FaceNormals[i] = d[0], encoded.FaceNormals[i + 1] = d[1], encoded.FaceNormals[i + 2] = d[2];
            const Vec3 u = Normalize({n[0], n[1], n[2]}), v = Normalize({d[0], d[1], d[2]});
            const Vec3 c = Cross(u, v);
            maxAngle = std::max(maxAngle, std::atan2(std::sqrt(Dot(c, c)), Dot(u, v)) * 180.0 / kPi);
        }
        GatherSettings g = DeterministicGather(1.5, 0.5, 8);
        const auto a = GatherImage(exact, g), b = GatherImage(encoded, g);
        double biasExact = 0.0, biasEncoded = 0.0, maxJ = 0.0;
        size_t valid = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!a[i].Valid) continue;
            ++valid;
            biasExact += a[i].OccupiedSectors;
            biasEncoded += b[i].OccupiedSectors;
            maxJ = std::max(maxJ, std::abs(b[i].J.R - a[i].J.R));
        }
        std::printf("DGI_M3 octahedral_face_normal pitch=%.0f max_angle_deg=%.5f occupied sectors per receiver exact=%.4f "
                    "encoded=%.4f max |dJ|=%.1e\n",
                    pitchDegrees, maxAngle, biasExact / valid, biasEncoded / valid, maxJ);
        EXPECT_LT(maxAngle, 0.01);
        EXPECT_EQ(biasEncoded, 0.0); // a plane never occludes itself
    }
}
