// M2 acceptance tests T05-T09 of the diffuse screen-space GI specification:
// gathering on synthetic depth buffers, sector masks, thickness, scale and
// order, plus the [B2] cross-check, the depth-derived geometric normal on a
// sloped plane (M1 open point 3) and the sector-count study (M1 open point 5).
// Lines starting with DGI_REPORT are the reproducible numbers quoted in
// docs/TRIAEVUM_SCREEN_SPACE_GI.md.
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_scene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

namespace {

constexpr double kDegree = kPi / 180.0;

// [B2] eq. 7 (Jimenez et al.), implemented from the paper for the cross-check:
// inner slice integral between signed horizons h1 <= 0 <= h2 inside the lobe.
double B2InnerIntegral(double h1, double h2, double gamma) {
    const auto half = [gamma](double h) {
        return 0.25 * (-std::cos(2.0 * h - gamma) + std::cos(gamma) + 2.0 * h * std::sin(gamma));
    };
    return half(h1) + half(h2);
}

struct Rendered {
    Scene SceneValue;
    GatherInput Input;
    PreparedGeometry Geometry;
};

Rendered Render(const Scene& scene) {
    Rendered r{scene, Rasterize(scene), {}};
    r.Geometry = PrepareGeometry(r.Input);
    return r;
}

GatherSettings Make(double radius, double thickness, uint32_t sectors = 64, uint32_t sections = 8) {
    GatherSettings s;
    s.Radius = radius;
    s.Thickness = thickness;
    s.Sectors = sectors;
    s.Sections = sections;
    s.Threads = 4;
    return s;
}

// Pixel whose footprint contains the projection of a view-space point.
std::pair<uint32_t, uint32_t> PixelOf(const Scene& scene, const Vec3& point) {
    const DepthCamera c = MakeDepthCamera(scene.Camera);
    const double tx = point.X / -point.Z, ty = point.Y / -point.Z;
    const double u = (tx - c.ViewX[2]) / c.ViewX[0];
    const double v = (ty - c.ViewY[2]) / c.ViewY[1];
    return {static_cast<uint32_t>(u * scene.Camera.Width), static_cast<uint32_t>(v * scene.Camera.Height)};
}

size_t At(const Rendered& r, uint32_t x, uint32_t y) { return static_cast<size_t>(y) * r.Geometry.Width + x; }

// A of the estimator when nothing is occupied: (pi / S) sum_s M(n.v, n.t_s).
double OpenAccessibility(const Rendered& r, uint32_t x, uint32_t y, const GatherSettings& s) {
    double sum = 0.0;
    for (uint32_t section = 0; section < s.Sections; ++section) {
        const auto setup =
            SetupSection(r.Input, r.Geometry, x, y, kPi * (section + s.Rotation) / s.Sections, s);
        sum += SectionMass(setup.NV, setup.NT);
    }
    return kPi * sum / s.Sections;
}

SectorMask CoverOf(const Candidate& c, uint32_t sectors) {
    SectorMask mask;
    for (uint32_t piece = 0; piece < c.Pieces; ++piece) {
        const SectorMask part = CoverInterval(c.Lo[piece], c.Hi[piece], sectors);
        for (uint32_t w = 0; w < kMaskWords; ++w) mask.Words[w] |= part.Words[w];
    }
    return mask;
}

bool Has(const SectorMask& mask, uint32_t j) { return (mask.Words[j / 32] >> (j % 32)) & 1U; }

SectorMask ValidMask(uint32_t sectors) {
    SectorMask mask;
    SetSectorRange(mask, 0, sectors);
    return mask;
}

// Per-sector brute force of the centre rule and first claim (no word logic).
SectionResult BruteForce(const SectionSetup& setup, const std::vector<Candidate>& ordered, uint32_t sectors) {
    SectionResult r;
    r.Claimant.assign(sectors, -1);
    const double step = 2.0 * kPi / sectors;
    for (size_t c = 0; c < ordered.size(); ++c)
        for (uint32_t j = 0; j < sectors; ++j) {
            if (r.Claimant[j] >= 0) continue;
            const double centre = -kPi + (j + 0.5) * step;
            bool covered = false;
            for (uint32_t piece = 0; piece < ordered[c].Pieces; ++piece)
                covered = covered || (ordered[c].Lo[piece] <= centre && centre < ordered[c].Hi[piece]);
            if (!covered) continue;
            r.Claimant[j] = static_cast<int32_t>(c);
            r.Mask.Words[j / 32] |= 1U << (j % 32);
            const double w = SectorWeightAnalytic(setup.NV, setup.NT, SectorBoundary(j, sectors),
                                                  SectorBoundary(j + 1, sectors));
            r.F.R += w * ordered[c].Radiance.R;
            r.F.G += w * ordered[c].Radiance.G;
            r.F.B += w * ordered[c].Radiance.B;
        }
    for (uint32_t j = 0; j < sectors; ++j)
        if (r.Claimant[j] < 0)
            r.FA += SectorWeightAnalytic(setup.NV, setup.NT, SectorBoundary(j, sectors), SectorBoundary(j + 1, sectors));
    return r;
}

uint32_t Lcg(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}
double Uniform(uint32_t& state, double lo, double hi) {
    return lo + (hi - lo) * static_cast<double>(Lcg(state) >> 8) / 16777216.0;
}

bool Finite(const PixelGather& p) {
    return std::isfinite(p.J.R) && std::isfinite(p.J.G) && std::isfinite(p.J.B) && std::isfinite(p.ARaw) &&
           std::isfinite(p.Confidence);
}

bool SameResult(const PixelGather& a, const PixelGather& b) {
    return a.Valid == b.Valid && a.J == b.J && a.ARaw == b.ARaw && a.Confidence == b.Confidence &&
           a.OccupiedSectors == b.OccupiedSectors && a.Counters.Scheduled == b.Counters.Scheduled &&
           a.Counters.Valid == b.Counters.Valid && a.Counters.Occupying == b.Counters.Occupying;
}

// One sector claimed in one section changes J by at most pi (2 / N) L / S.
double SingleFlipBound(uint32_t sectors, uint32_t sections) { return 2.0 * kPi / (sectors * sections); }

} // namespace

TEST(DiffuseGiGather, B2CrossCheckOfM1ClosedForms) {
    // Same measure: [B2] eqs. 5 and 8 integrate cos(theta - gamma)^+ |sin theta|
    // over theta from the view vector, times |n_projected|, over phi in
    // [0, pi), divided by pi. With a = n.v = r cos(gamma), b = n.t = r sin(gamma)
    // that is exactly M1's (1/pi) integral of max(0, a cos + b sin) |sin|.
    double maxMass = 0.0, maxPiece = 0.0, maxCumulative = 0.0;
    int cases = 0;
    for (double r : {1.0, 0.7, 0.2})
        for (double gammaDeg = -89.0; gammaDeg <= 89.0; gammaDeg += 7.0) {
            const double gamma = gammaDeg * kDegree;
            const double a = r * std::cos(gamma), b = r * std::sin(gamma);
            // (1) section mass M_phi: horizons at the lobe edges.
            const double b2Mass = r * B2InnerIntegral(gamma - 0.5 * kPi, gamma + 0.5 * kPi, gamma) / kPi;
            maxMass = std::max(maxMass, std::abs(b2Mass - SectionMass(a, b)));
            // (2) per-piece integral: arbitrary horizons inside the lobe.
            for (double f1 : {0.0, 0.3, 0.9})
                for (double f2 : {0.0, 0.5, 1.0}) {
                    const double h1 = f1 * (gamma - 0.5 * kPi);
                    const double h2 = f2 * (gamma + 0.5 * kPi);
                    const double b2 = r * B2InnerIntegral(h1, h2, gamma) / kPi;
                    maxPiece = std::max(maxPiece, std::abs(b2 - SectorWeightAnalytic(a, b, h1, h2)));
                    ++cases;
                }
            // (3) cumulative C_phi(theta): mass from the lobe start to theta.
            for (double theta = gamma - 0.5 * kPi; theta <= gamma + 0.5 * kPi; theta += 0.05) {
                const double l0 = gamma - 0.5 * kPi;
                const double partial = theta <= 0.0 ? B2InnerIntegral(l0, 0.0, gamma) - B2InnerIntegral(theta, 0.0, gamma)
                                                    : B2InnerIntegral(l0, theta, gamma);
                const double c = r * partial / kPi / SectionMass(a, b);
                maxCumulative = std::max(maxCumulative, std::abs(c - Cumulative(a, b, theta)));
            }
        }
    std::printf("DGI_REPORT B2_crosscheck pieces=%d mass_max_abs=%.2e piece_max_abs=%.2e cumulative_max_abs=%.2e\n",
                cases, maxMass, maxPiece, maxCumulative);
    EXPECT_LT(maxMass, 1.0e-15);
    EXPECT_LT(maxPiece, 1.0e-15);
    EXPECT_LT(maxCumulative, 1.0e-14);
    // [B2] eq. 7 needs a signed gamma: with |gamma| the result is wrong for a
    // normal leaning toward -t (recorded in the manifest).
    const double gamma = -40.0 * kDegree;
    const double right = SectorWeightAnalytic(std::cos(gamma), std::sin(gamma), -0.2, 0.5);
    EXPECT_NEAR(B2InnerIntegral(-0.2, 0.5, gamma) / kPi, right, 1.0e-15);
    const double unsignedGamma = B2InnerIntegral(-0.2, 0.5, std::abs(gamma)) / kPi;
    std::printf("DGI_REPORT B2_signed_gamma right=%.6f with_abs_gamma=%.6f\n", right, unsignedGamma);
    EXPECT_GT(std::abs(unsignedGamma - right), 1.0e-2);
}

TEST(DiffuseGiGather, T05MasksAcrossWordsEmptyFullDisjoint) {
    uint32_t state = 7U;
    size_t randomCases = 0, randomMismatch = 0, alignedCases = 0, alignedMismatch = 0, f32Cases = 0, f32Disagree = 0;
    size_t disjointCases = 0, disjointMismatch = 0;
    double partitionError = 0.0;
    for (const uint32_t n : {1U, 2U, 3U, 31U, 32U, 33U, 63U, 64U, 65U, 96U, 100U, 128U, 255U, 256U}) {
        const double step = 2.0 * kPi / n;
        // Full, longer than the circle, empty and reversed intervals.
        EXPECT_EQ(CoverInterval(-kPi, kPi, n), ValidMask(n)) << n;
        EXPECT_EQ(CountSectors(CoverInterval(-kPi, kPi, n)), n);
        EXPECT_EQ(CoverInterval(-1.0, -1.0 + 7.0, n), ValidMask(n));
        EXPECT_EQ(CountSectors(CoverInterval(0.3, 0.3, n)), 0U);
        EXPECT_EQ(CountSectors(CoverInterval(0.5, 0.2, n)), 0U);
        // Runs of exactly [j0, j1) sectors, across word boundaries.
        for (uint32_t j0 = 0; j0 < n; ++j0)
            for (const uint32_t length : {1U, 31U, 32U, 33U, 64U, 65U}) {
                const uint32_t j1 = std::min(n, j0 + length);
                SectorMask expected;
                for (uint32_t j = j0; j < j1; ++j) expected.Words[j / 32] |= 1U << (j % 32);
                ++alignedCases;
                if (!(CoverInterval(-kPi + (j0 + 0.25) * step, -kPi + (j1 - 0.25) * step, n) == expected))
                    ++alignedMismatch;
            }
        // Interval through +-pi equals the union of its two pieces.
        SectorMask pieces = CoverInterval(kPi - 0.3, kPi, n);
        const SectorMask second = CoverInterval(-kPi, -kPi + 0.2, n);
        for (uint32_t w = 0; w < kMaskWords; ++w) pieces.Words[w] |= second.Words[w];
        EXPECT_EQ(CoverInterval(kPi - 0.3, kPi + 0.2, n), pieces);
        // Random intervals against the per-sector brute force; disjoint unions.
        for (int k = 0; k < 300; ++k) {
            double a = Uniform(state, -kPi, kPi), b = Uniform(state, -kPi, kPi);
            if (a > b) std::swap(a, b);
            SectorMask brute;
            bool nearTie = false;
            for (uint32_t j = 0; j < n; ++j) {
                const double centre = -kPi + (j + 0.5) * step;
                nearTie = nearTie || std::abs(centre - a) < 1.0e-9 || std::abs(centre - b) < 1.0e-9;
                if (a <= centre && centre < b) brute.Words[j / 32] |= 1U << (j % 32);
            }
            if (nearTie) continue;
            ++randomCases;
            const SectorMask cover = CoverInterval(a, b, n);
            if (!(cover == brute)) ++randomMismatch;
            ++f32Cases;
            if (CentreRuleRangeF32(static_cast<float>(a), static_cast<float>(b), n) != CentreRuleRange(a, b, n))
                ++f32Disagree;
            const double m = 0.5 * (a + b);
            SectorMask left = CoverInterval(a, m, n);
            const SectorMask right = CoverInterval(m, b, n);
            uint32_t overlap = 0;
            for (uint32_t w = 0; w < kMaskWords; ++w) {
                overlap |= left.Words[w] & right.Words[w];
                left.Words[w] |= right.Words[w];
            }
            ++disjointCases;
            if (overlap != 0U || !(left == cover)) ++disjointMismatch;
        }
        // Partition of the section mass into occupied and open runs.
        for (int k = 0; k < 50; ++k) {
            SectorMask mask, complement;
            for (uint32_t w = 0; w < kMaskWords; ++w) mask.Words[w] = Lcg(state) & ValidMask(n).Words[w];
            for (uint32_t w = 0; w < kMaskWords; ++w) complement.Words[w] = ~mask.Words[w] & ValidMask(n).Words[w];
            const double tilt = Uniform(state, 0.0, kPi), azimuth = Uniform(state, -kPi, kPi);
            const double nv = std::cos(tilt), nt = std::sin(tilt) * std::cos(azimuth);
            partitionError = std::max(partitionError, std::abs(MaskWeight(mask, n, nv, nt) +
                                                               MaskWeight(complement, n, nv, nt) - SectionMass(nv, nt)));
        }
    }
    std::printf("DGI_REPORT T05_masks aligned=%zu mismatches=%zu random=%zu mismatches=%zu disjoint=%zu mismatches=%zu "
                "partition_max_abs=%.2e f32_range_disagreements=%zu/%zu\n",
                alignedCases, alignedMismatch, randomCases, randomMismatch, disjointCases, disjointMismatch,
                partitionError, f32Disagree, f32Cases);
    EXPECT_EQ(alignedMismatch, 0U);
    EXPECT_EQ(randomMismatch, 0U);
    EXPECT_EQ(disjointMismatch, 0U);
    EXPECT_LT(partitionError, 1.0e-15);

    // Deterministic results: repeated runs and thread counts are bit-identical.
    const Rendered r = Render(WordBoundaryScene(96, 72));
    GatherSettings settings = Make(1.5, 0.4, 96);
    const auto first = GatherImage(r.Input, settings);
    const auto again = GatherImage(r.Input, settings);
    settings.Threads = 1;
    const auto single = GatherImage(r.Input, settings);
    size_t different = 0;
    for (size_t i = 0; i < first.size(); ++i)
        if (!SameResult(first[i], again[i]) || !SameResult(first[i], single[i])) ++different;
    EXPECT_EQ(different, 0U);
}

TEST(DiffuseGiGather, T05DuplicateCandidatesAddNoEnergy) {
    const Rendered r = Render(WordBoundaryScene(128, 96));
    GatherSettings settings = Make(1.5, 0.4, 96);
    size_t sections = 0, candidates = 0, crossing = 0, bruteMaskMismatch = 0;
    double bruteMax = 0.0;
    bool identical = true, identicalContinuous = true;
    for (uint32_t y = 40; y < 96; y += 5)
        for (uint32_t x = 4; x < 124; x += 7) {
            const Vec3& n = r.Geometry.Normal[At(r, x, y)];
            if (!(Dot(n, n) > 0.0)) continue;
            for (uint32_t s = 0; s < 8; ++s) {
                const auto built = BuildSection(r.Input, r.Geometry, x, y, kPi * (s + 0.5) / 8.0, settings);
                const auto base = AccumulateSection(built.Setup, built.Ordered, 96, MaskMode::CentreRule);
                // Every candidate twice in a row, then the whole list again.
                std::vector<Candidate> doubled;
                for (const auto& c : built.Ordered) doubled.insert(doubled.end(), {c, c});
                doubled.insert(doubled.end(), built.Ordered.begin(), built.Ordered.end());
                const auto again = AccumulateSection(built.Setup, doubled, 96, MaskMode::CentreRule);
                identical = identical && again.F == base.F && again.FA == base.FA && again.Mask == base.Mask;
                const auto exact = AccumulateSection(built.Setup, built.Ordered, 96, MaskMode::ContinuousIntervals);
                const auto exactAgain = AccumulateSection(built.Setup, doubled, 96, MaskMode::ContinuousIntervals);
                identicalContinuous = identicalContinuous && exactAgain.F == exact.F && exactAgain.FA == exact.FA;
                for (const auto& c : built.Ordered) {
                    const SectorMask cover = CoverOf(c, 96);
                    int words = 0;
                    for (uint32_t w = 0; w < kMaskWords; ++w) words += cover.Words[w] != 0U ? 1 : 0;
                    if (words >= 2) ++crossing;
                }
                const auto brute = BruteForce(built.Setup, built.Ordered, 96);
                if (!(brute.Mask == base.Mask) || brute.Claimant != base.Claimant) ++bruteMaskMismatch;
                bruteMax = std::max({bruteMax, std::abs(brute.F.R - base.F.R), std::abs(brute.F.G - base.F.G),
                                     std::abs(brute.F.B - base.F.B), std::abs(brute.FA - base.FA)});
                ++sections;
                candidates += built.Ordered.size();
            }
        }
    // Radial schedule: a texel is one surface sample, used at most once per side.
    settings.Schedule = CandidateSchedule::Radial;
    settings.CandidatesPerSide = 64;
    settings.RadialExponent = 3.0;
    uint64_t duplicates = 0;
    size_t radialCandidates = 0;
    bool unique = true;
    for (uint32_t y = 50; y < 96; y += 9)
        for (uint32_t x = 10; x < 120; x += 13)
            for (uint32_t s = 0; s < 8; ++s) {
                const auto built = BuildSection(r.Input, r.Geometry, x, y, kPi * (s + 0.5) / 8.0, settings);
                duplicates += built.Counters.Duplicates;
                std::set<std::pair<int, std::pair<int32_t, int32_t>>> seen;
                for (const auto& c : built.Ordered)
                    unique = unique && seen.insert({c.Side, {c.CellX, c.CellY}}).second;
                radialCandidates += built.Ordered.size();
            }
    std::printf("DGI_REPORT T05_duplicates sections=%zu candidates=%zu word_crossing_candidates=%zu "
                "duplicated_lists_identical=%d continuous_identical=%d brute_force_mask_mismatch=%zu brute_force_max_abs=%.2e "
                "radial_K64_duplicates_skipped=%llu radial_candidates=%zu unique_texels=%d\n",
                sections, candidates, crossing, identical ? 1 : 0, identicalContinuous ? 1 : 0, bruteMaskMismatch,
                bruteMax, static_cast<unsigned long long>(duplicates), radialCandidates, unique ? 1 : 0);
    EXPECT_TRUE(identical);
    EXPECT_TRUE(identicalContinuous);
    EXPECT_GT(crossing, 0U);
    EXPECT_EQ(bruteMaskMismatch, 0U);
    EXPECT_LT(bruteMax, 1.0e-14);
    EXPECT_GT(duplicates, 0U);
    EXPECT_TRUE(unique);
}

TEST(DiffuseGiGather, T06BlackOccluderBlocksTheSourceBehind) {
    const Rendered with = Render(BlackOccluderScene(192, 128, true));
    const Rendered without = Render(BlackOccluderScene(192, 128, false));
    const GatherSettings settings = Make(5.0, 1.0, 64);
    // (1) Explicit candidates on a real section: black first, bright second.
    const auto [x0, y0] = PixelOf(with.SceneValue, {0.2, -1.0, -5.6});
    const SectionSetup setup = SetupSection(with.Input, with.Geometry, x0, y0, 0.3 * kPi, settings);
    // Intervals inside the cosine lobe centred on atan2(n.t, n.v).
    const double lobe = std::atan2(setup.NT, setup.NV);
    Candidate black, bright;
    black.Pieces = bright.Pieces = 1;
    black.Lo[0] = lobe - 0.6, black.Hi[0] = lobe + 0.1;
    bright.Lo[0] = lobe - 0.3, bright.Hi[0] = lobe + 0.8;
    bright.Radiance = {5.0, 5.0, 5.0};
    const SectorMask coverBlack = CoverOf(black, 64), coverBright = CoverOf(bright, 64);
    SectorMask brightOnly, open;
    for (uint32_t w = 0; w < kMaskWords; ++w) {
        brightOnly.Words[w] = coverBright.Words[w] & ~coverBlack.Words[w];
        open.Words[w] = ~(coverBright.Words[w] | coverBlack.Words[w]) & ValidMask(64).Words[w];
    }
    const auto combined = AccumulateSection(setup, {black, bright}, 64, MaskMode::CentreRule);
    const auto alone = AccumulateSection(setup, {bright}, 64, MaskMode::CentreRule);
    const auto reversed = AccumulateSection(setup, {bright, black}, 64, MaskMode::CentreRule);
    EXPECT_NEAR(combined.F.R, 5.0 * MaskWeight(brightOnly, 64, setup.NV, setup.NT), 1.0e-15);
    EXPECT_NEAR(combined.FA, MaskWeight(open, 64, setup.NV, setup.NT), 1.0e-15);
    EXPECT_NEAR(alone.F.R, 5.0 * MaskWeight(coverBright, 64, setup.NV, setup.NT), 1.0e-15);
    EXPECT_LT(combined.F.R, alone.F.R - 1.0e-3);
    EXPECT_EQ(reversed.F, alone.F); // order matters: the first claimant wins
    const auto exact = AccumulateSection(setup, {black, bright}, 64, MaskMode::ContinuousIntervals);
    EXPECT_NEAR(exact.F.R, 5.0 * SectorWeightAnalytic(setup.NV, setup.NT, lobe + 0.1, lobe + 0.8), 1.0e-15);
    std::printf("DGI_REPORT T06_unit F_black_then_bright=%.6f F_bright_alone=%.6f F_bright_then_black=%.6f "
                "F_continuous=%.6f FA=%.6f\n",
                combined.F.R, alone.F.R, reversed.F.R, exact.F.R, combined.FA);

    // (2) Depth scene: floor receivers whose 5x5 neighbourhood is identical in
    // both scenes. Black geometry can only remove light, sector by sector.
    const auto imageWith = GatherImage(with.Input, settings);
    const auto imageWithout = GatherImage(without.Input, settings);
    size_t compared = 0, increased = 0, band = 0, blocked = 0, blackClaims = 0;
    double sumWith = 0.0, sumWithout = 0.0;
    const uint32_t w = 192, h = 128;
    for (uint32_t y = 2; y + 2 < h; ++y)
        for (uint32_t x = 2; x + 2 < w; ++x) {
            bool same = true;
            for (int dy = -2; dy <= 2 && same; ++dy)
                for (int dx = -2; dx <= 2 && same; ++dx) {
                    const size_t i = At(with, x + dx, y + dy);
                    same = with.Geometry.Class[i] == without.Geometry.Class[i] &&
                           with.Geometry.ViewDepth[i] == without.Geometry.ViewDepth[i];
                }
            const size_t i = At(with, x, y);
            if (!same || !imageWith[i].Valid || with.Geometry.Normal[i].Y < 0.99) continue;
            ++compared;
            if (imageWith[i].J.R > imageWithout[i].J.R + 1.0e-12) ++increased;
            const Vec3 p = with.Geometry.Position(with.Input.Camera, x, y);
            if (p.Z < -5.2 && p.Z > -6.4 && std::abs(p.X - 0.3) < 1.2) {
                ++band;
                sumWith += imageWith[i].J.R;
                sumWithout += imageWithout[i].J.R;
                if ((x + y) % 3 != 0) continue;
                for (uint32_t s = 0; s < settings.Sections; ++s) {
                    const auto built =
                        BuildSection(with.Input, with.Geometry, x, y, kPi * (s + 0.5) / settings.Sections, settings);
                    const auto acc = AccumulateSection(built.Setup, built.Ordered, 64, MaskMode::CentreRule);
                    for (uint32_t j = 0; j < 64; ++j) {
                        const int32_t c = acc.Claimant[j];
                        if (c < 0 || !(built.Ordered[c].Radiance == Rgb{})) continue;
                        ++blackClaims;
                        for (size_t later = c + 1; later < built.Ordered.size(); ++later)
                            if (built.Ordered[later].Radiance.R > 0.0 && Has(CoverOf(built.Ordered[later], 64), j)) {
                                ++blocked; // a bright candidate behind, refused in this sector
                                break;
                            }
                    }
                }
            }
        }
    std::printf("DGI_REPORT T06 floor_receivers_compared=%zu increased=%zu band=%zu band_J_with=%.4f band_J_without=%.4f "
                "zero_radiance_claims=%zu blocked_bright_sectors=%zu\n",
                compared, increased, band, sumWith / band, sumWithout / band, blackClaims, blocked);
    EXPECT_GT(compared, 1000U);
    EXPECT_EQ(increased, 0U);
    EXPECT_LT(sumWith, 0.9 * sumWithout);
    EXPECT_GT(blocked, 100U);

    // (3) Against the 3D first hit (finite-radius model).
    for (const Vec3 point : {Vec3{0.2, -1.0, -5.6}, Vec3{-0.6, -1.0, -5.9}, Vec3{1.0, -1.0, -6.0}}) {
        const auto [x, y] = PixelOf(with.SceneValue, point);
        GatherSettings continuous = settings;
        continuous.Mask = MaskMode::ContinuousIntervals;
        continuous.Sections = 32;
        const auto screen = GatherPixel(with.Input, with.Geometry, x, y, continuous);
        const auto truth = IntegrateGroundTruth(with.SceneValue, with.Input, with.Geometry, x, y, settings.Radius);
        const auto screenWithout = GatherPixel(without.Input, without.Geometry, x, y, continuous);
        const auto truthWithout =
            IntegrateGroundTruth(without.SceneValue, without.Input, without.Geometry, x, y, settings.Radius);
        std::printf("DGI_REPORT T06 receiver=(%.1f,%.1f) J_centre_S8=%.4f J_cont_S32=%.4f J_truth=%.4f A_cont=%.4f "
                    "A_truth=%.4f | without_box J_cont=%.4f J_truth=%.4f\n",
                    point.X, point.Z, imageWith[At(with, x, y)].J.R, screen.J.R, truth.J.R, screen.ARaw, truth.A,
                    screenWithout.J.R, truthWithout.J.R);
        EXPECT_TRUE(truth.Converged);
        EXPECT_LT(screen.J.R, screenWithout.J.R);
        EXPECT_LT(truth.J.R, truthWithout.J.R);
    }
}

TEST(DiffuseGiGather, T06OneSidedSourcesEmitOnlyTowardTheirFront) {
    // A bright panel faces the camera (+Z). Receivers in front of its plane see
    // its emitting side; receivers behind it see its back: a one-sided panel
    // still occupies those sectors but brings no light (no source cosine).
    const Rendered one = Render(PanelScene(288, 192, false));
    const Rendered two = Render(PanelScene(288, 192, true));
    GatherSettings s = Make(1.5, 0.3, 64, 32);
    s.Mask = MaskMode::ContinuousIntervals;
    for (const Vec3 point : {Vec3{0.2, -1.0, -5.6}, Vec3{0.1, -1.0, -6.4}}) {
        const auto [x, y] = PixelOf(one.SceneValue, point);
        const auto a = GatherPixel(one.Input, one.Geometry, x, y, s);
        const auto b = GatherPixel(two.Input, two.Geometry, x, y, s);
        const auto truthA = IntegrateGroundTruth(one.SceneValue, one.Input, one.Geometry, x, y, s.Radius);
        const auto truthB = IntegrateGroundTruth(two.SceneValue, two.Input, two.Geometry, x, y, s.Radius);
        std::printf("DGI_REPORT T06_one_sided receiver=(%.1f,%.1f) one_sided J=%.4f truth=%.4f A=%.4f back_facing=%llu | "
                    "two_sided J=%.4f truth=%.4f A=%.4f\n",
                    point.X, point.Z, a.J.R, truthA.J.R, a.ARaw, static_cast<unsigned long long>(a.Counters.BackFacing),
                    b.J.R, truthB.J.R, b.ARaw);
        EXPECT_EQ(a.ARaw, b.ARaw); // same occupancy either way
        if (point.Z > -6.0) {
            EXPECT_GT(a.J.R, 0.01); // front side: light
            EXPECT_EQ(a.J, b.J);
        } else {
            EXPECT_EQ(a.J, Rgb{}); // back side: occupied, black
            EXPECT_GT(a.Counters.BackFacing, 0U);
            EXPECT_GT(b.J.R, 0.01);
            EXPECT_EQ(truthA.J, Rgb{});
        }
    }
}

TEST(DiffuseGiGather, T07ThinBarLeavesDirectionsOpen) {
    // 864 x 576 (9 x the 96 x 64 base): the wall's own samples close for tau of
    // a few hundredths, so the bar's thickness is what is being measured.
    const Rendered r = Render(ThinBarScene(864, 576));
    const std::vector<double> thicknesses{0.0, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 1000.0};
    bool monotone = true;
    for (const Vec3 point : {Vec3{0.0, -1.0, -5.5}, Vec3{0.6, -1.0, -5.6}, Vec3{-1.0, -1.0, -5.3}}) {
        const auto [x, y] = PixelOf(r.SceneValue, point);
        const auto truth = IntegrateGroundTruth(r.SceneValue, r.Input, r.Geometry, x, y, 4.0);
        std::printf("DGI_REPORT T07_bar receiver=(%.1f,%.1f) truth J=%.4f A=%.4f |", point.X, point.Z, truth.J.B,
                    truth.A);
        double previousA = 2.0, previousCentreA = 2.0, jFinite = 0.0, jInfinite = 0.0;
        for (const double tau : thicknesses) {
            GatherSettings s = Make(4.0, tau, 64, 32);
            const auto centre = GatherPixel(r.Input, r.Geometry, x, y, s);
            s.Mask = MaskMode::ContinuousIntervals;
            const auto cont = GatherPixel(r.Input, r.Geometry, x, y, s);
            monotone = monotone && cont.ARaw <= previousA + 1.0e-12 && centre.ARaw <= previousCentreA + 1.0e-12;
            previousA = cont.ARaw;
            previousCentreA = centre.ARaw;
            if (tau == 0.1) jFinite = cont.J.B;
            if (tau == 1000.0) jInfinite = cont.J.B;
            std::printf(" tau=%g J=%.4f A=%.4f", tau, cont.J.B, cont.ARaw);
        }
        std::printf("\n");
        EXPECT_TRUE(truth.Converged);
        EXPECT_GT(jFinite, 1.05 * jInfinite) << point.X; // finite thickness leaves directions open
    }
    EXPECT_TRUE(monotone);
    // Coverage curve over the floor band in front of the bar (192 x 128, S = 8).
    const Rendered image = Render(ThinBarScene(192, 128));
    std::printf("DGI_REPORT T07_bar_coverage mean_occluded_fraction_by_tau:");
    double previous = -1.0;
    bool curveMonotone = true;
    for (const double tau : thicknesses) {
        const auto result = GatherImage(image.Input, Make(4.0, tau, 64, 8));
        double occluded = 0.0;
        size_t count = 0;
        for (uint32_t y = 0; y < 128; ++y)
            for (uint32_t x = 0; x < 192; ++x) {
                const size_t i = At(image, x, y);
                if (!result[i].Valid || image.Geometry.Normal[i].Y < 0.99) continue;
                const Vec3 p = image.Geometry.Position(image.Input.Camera, x, y);
                if (p.Z < -4.8 && p.Z > -5.95 && std::abs(p.X) < 2.0) {
                    occluded += 1.0 - result[i].ARaw;
                    ++count;
                }
            }
        occluded /= std::max<size_t>(count, 1);
        curveMonotone = curveMonotone && occluded >= previous - 1.0e-12;
        previous = occluded;
        std::printf(" %g:%.4f", tau, occluded);
    }
    std::printf("\n");
    EXPECT_TRUE(curveMonotone);
}

TEST(DiffuseGiGather, T07ThickWallLeaksBehindFiniteThickness) {
    // Receivers beside a 3.5-deep dark block; the bright wall behind it is
    // visible to the camera beyond the block's silhouette.
    const Rendered r = Render(ThickBlockScene(288, 192));
    const std::vector<double> thicknesses{0.25, 0.5, 1.0, 2.0, 4.0, 8.0};
    for (const Vec3 point : {Vec3{0.9, -1.0, -6.5}, Vec3{0.8, -1.0, -7.6}, Vec3{1.3, -1.0, -6.0}}) {
        const auto [x, y] = PixelOf(r.SceneValue, point);
        const auto truth = IntegrateGroundTruth(r.SceneValue, r.Input, r.Geometry, x, y, 8.0);
        std::printf("DGI_REPORT T07_block receiver=(%.1f,%.1f) truth J=%.4f A=%.4f | leak=J-J_truth by tau:", point.X,
                    point.Z, truth.J.R, truth.A);
        double previousA = 2.0, firstLeak = 0.0, lastLeak = 0.0;
        for (const double tau : thicknesses) {
            GatherSettings s = Make(8.0, tau, 64, 32);
            s.Mask = MaskMode::ContinuousIntervals;
            const auto cont = GatherPixel(r.Input, r.Geometry, x, y, s);
            EXPECT_LE(cont.ARaw, previousA + 1.0e-12);
            previousA = cont.ARaw;
            const double leak = cont.J.R - truth.J.R;
            if (tau == thicknesses.front()) firstLeak = leak;
            lastLeak = leak;
            std::printf(" %g:%+.4f(A%+.4f)", tau, leak, cont.ARaw - truth.A);
        }
        std::printf("\n");
        EXPECT_TRUE(truth.Converged);
        EXPECT_GT(firstLeak, lastLeak); // more thickness, less light behind the wall
    }
}

TEST(DiffuseGiGather, T08ScaleInvariance) {
    const Scene base = BlackOccluderScene(160, 108, true);
    const Rendered unit = Render(base);
    const GatherSettings settings = Make(5.0, 1.0, 64);
    const auto reference = GatherImage(unit.Input, settings);
    const auto reference32 = GatherImageF32(unit.Input, settings);
    for (const double factor : {0.01, 100.0}) {
        // Scene, near, far, R and tau scaled together.
        const Rendered scaled = Render(Scaled(base, factor));
        GatherSettings s = settings;
        s.Radius *= factor;
        s.Thickness *= factor;
        const auto image = GatherImage(scaled.Input, s);
        const auto image32 = GatherImageF32(scaled.Input, s);
        double maxJ = 0.0, maxA = 0.0, max32 = 0.0;
        size_t counterMismatch = 0, flips32 = 0;
        for (size_t i = 0; i < image.size(); ++i) {
            maxJ = std::max(maxJ, std::abs(image[i].J.R - reference[i].J.R) / std::max(1.0, reference[i].J.R));
            maxA = std::max(maxA, std::abs(image[i].ARaw - reference[i].ARaw));
            if (image[i].Counters.Scheduled != reference[i].Counters.Scheduled ||
                image[i].Counters.Occupying != reference[i].Counters.Occupying)
                ++counterMismatch;
            const double d32 = std::abs(image32[i].J.R - reference32[i].J.R) / std::max(1.0, reference32[i].J.R);
            max32 = std::max(max32, d32);
            if (d32 > 1.0e-4) ++flips32;
        }
        std::printf("DGI_REPORT T08_scale factor=%g f64_max_rel_J=%.2e f64_max_abs_A=%.2e counter_mismatch=%zu "
                    "f32_max_rel_J=%.2e f32_pixels_over_1e-4=%zu/%zu\n",
                    factor, maxJ, maxA, counterMismatch, max32, flips32, image.size());
        EXPECT_LT(maxJ, 1.0e-9);
        EXPECT_LT(maxA, 1.0e-9);
        EXPECT_EQ(counterMismatch, 0U);
        EXPECT_LT(flips32, image.size() / 200);
    }
    // Geometry doubled with fixed near/far planes: only depth precision changes.
    Scene doubled = Scaled(base, 2.0);
    doubled.Camera.Near = base.Camera.Near;
    doubled.Camera.Far = base.Camera.Far;
    const Rendered fixedPlanes = Render(doubled);
    GatherSettings s = settings;
    s.Radius *= 2.0;
    s.Thickness *= 2.0;
    const auto image = GatherImage(fixedPlanes.Input, s);
    double mean = 0.0, maxJ = 0.0;
    for (size_t i = 0; i < image.size(); ++i) {
        const double d = std::abs(image[i].J.R - reference[i].J.R) / std::max(1.0, reference[i].J.R);
        mean += d;
        maxJ = std::max(maxJ, d);
    }
    std::printf("DGI_REPORT T08_scale fixed_near_far x2 mean_rel_J=%.2e max_rel_J=%.2e\n", mean / image.size(), maxJ);
    EXPECT_LT(mean / image.size(), 1.0e-3);
}

TEST(DiffuseGiGather, T08ResolutionConvergence) {
    // Wall fully visible to the camera: the screen-space model converges to the
    // 3D first hit as the footprint shrinks (odd factors keep the pixel centre).
    std::vector<double> errors[2];
    for (const uint32_t k : {1U, 3U, 9U}) {
        const Rendered r = Render(BlackOccluderScene(96 * k, 64 * k, false));
        GatherSettings s = Make(5.0, 1.0, 64, 64);
        s.Mask = MaskMode::ContinuousIntervals;
        int index = 0;
        for (const auto [x, y] : {std::pair{60U, 42U}, std::pair{30U, 41U}}) {
            const uint32_t xx = x * k + k / 2, yy = y * k + k / 2;
            const auto p = GatherPixel(r.Input, r.Geometry, xx, yy, s);
            const auto truth = IntegrateGroundTruth(r.SceneValue, r.Input, r.Geometry, xx, yy, s.Radius);
            const double relative = (p.J.R - truth.J.R) / truth.J.R;
            errors[index++].push_back(relative);
            std::printf("DGI_REPORT T08_resolution k=%u pixel=(%u,%u) J_cont=%.5f J_truth=%.5f rel=%+.3e A_cont=%.5f "
                        "A_truth=%.5f\n",
                        k, x, y, p.J.R, truth.J.R, relative, p.ARaw, truth.A);
        }
    }
    for (const auto& e : errors) {
        EXPECT_LT(std::abs(e[1]), std::abs(e[0]));
        EXPECT_LT(std::abs(e[2]), std::abs(e[1]));
        EXPECT_LT(std::abs(e[2]), 0.025);
    }
}

TEST(DiffuseGiGather, T08BordersViewportNearPlaneSkyAndRadius) {
    const Scene base = CornerRoomScene(160, 108);
    const Rendered r = Render(base);
    const GatherSettings settings = Make(2.5, 0.5, 64);
    const auto image = GatherImage(r.Input, settings);
    size_t nonFinite = 0, skyNotNeutral = 0, sky = 0;
    double borderC = 0.0, interiorC = 0.0;
    size_t border = 0, interior = 0;
    uint64_t skyCandidates = 0;
    for (uint32_t y = 0; y < 108; ++y)
        for (uint32_t x = 0; x < 160; ++x) {
            const auto& p = image[At(r, x, y)];
            if (!Finite(p)) ++nonFinite;
            skyCandidates += p.Counters.Sky;
            if (r.Geometry.Class[At(r, x, y)] == static_cast<uint8_t>(TexelClass::Sky)) {
                ++sky;
                if (p.Valid || !(p.J == Rgb{}) || p.ARaw != 1.0 || p.Confidence != 0.0) ++skyNotNeutral;
                continue;
            }
            if (!p.Valid) continue;
            const bool edge = x < 3 || y < 3 || x >= 157 || y >= 105;
            (edge ? borderC : interiorC) += p.Confidence;
            ++(edge ? border : interior);
        }
    std::printf("DGI_REPORT T08_borders nonfinite=%zu sky=%zu sky_not_neutral=%zu mean_C_border=%.3f mean_C_interior=%.3f "
                "sky_candidates=%llu\n",
                nonFinite, sky, skyNotNeutral, borderC / border, interiorC / interior,
                static_cast<unsigned long long>(skyCandidates));
    EXPECT_EQ(nonFinite, 0U);
    EXPECT_GT(sky, 0U);
    EXPECT_EQ(skyNotNeutral, 0U);
    EXPECT_LT(borderC / border, interiorC / interior);
    EXPECT_GT(skyCandidates, 0U);

    // Shifted viewport inside a poisoned buffer: bit-identical results.
    Scene padded = base;
    padded.PadLeft = 7, padded.PadTop = 5, padded.PadRight = 3, padded.PadBottom = 11;
    const auto shifted = GatherImage(Rasterize(padded), settings);
    size_t shiftedDifferent = 0;
    for (size_t i = 0; i < image.size(); ++i)
        if (!SameResult(image[i], shifted[i])) ++shiftedDifferent;
    EXPECT_EQ(shiftedDifferent, 0U);

    // Unavailable (HUD) rectangle: neutral inside, lower confidence around it.
    Scene hud = base;
    hud.Unavailable.push_back({60, 50, 100, 80});
    const auto withHud = GatherImage(Rasterize(hud), settings);
    size_t inside = 0, insideNeutral = 0, ring = 0, lowered = 0;
    for (uint32_t y = 40; y < 90; ++y)
        for (uint32_t x = 50; x < 110; ++x) {
            const size_t i = At(r, x, y);
            if (x >= 60 && x < 100 && y >= 50 && y < 80) {
                ++inside;
                if (!withHud[i].Valid && withHud[i].J == Rgb{} && withHud[i].ARaw == 1.0) ++insideNeutral;
            } else if (image[i].Valid) {
                ++ring;
                if (withHud[i].Confidence < image[i].Confidence) ++lowered;
                EXPECT_TRUE(Finite(withHud[i]));
            }
        }
    std::printf("DGI_REPORT T08_hud inside=%zu neutral=%zu ring=%zu ring_with_lower_C=%zu\n", inside, insideNeutral, ring,
                lowered);
    EXPECT_EQ(inside, insideNeutral);
    EXPECT_GT(lowered, ring / 2);

    // Near plane: receivers 0.1% beyond it and an R-ball that contains the
    // camera (R > |P|): the projected extents are capped at the viewport
    // diagonal; decoded depth, outputs and C stay finite.
    const Rendered nearScene = Render(NearPlaneScene(128, 96));
    const GatherSettings nearSettings = Make(0.5, 0.05, 64);
    const auto nearImage = GatherImage(nearScene.Input, nearSettings);
    size_t nearReceivers = 0, insideBall = 0, capped = 0, nearNonFinite = 0;
    const double diagonal = std::sqrt(128.0 * 128.0 + 96.0 * 96.0);
    for (uint32_t y = 0; y < 96; ++y)
        for (uint32_t x = 0; x < 128; ++x) {
            const size_t i = At(nearScene, x, y);
            if (!Finite(nearImage[i])) ++nearNonFinite;
            if (!nearImage[i].Valid) continue;
            if (nearScene.Geometry.ViewDepth[i] < 0.1002) ++nearReceivers;
            const auto setup = SetupSection(nearScene.Input, nearScene.Geometry, x, y, 0.4, nearSettings);
            if (std::sqrt(Dot(setup.P, setup.P)) >= nearSettings.Radius) continue;
            ++insideBall;
            if (setup.Extent[0] == diagonal && setup.Extent[1] == diagonal) ++capped;
        }
    const auto [nx, ny] = PixelOf(nearScene.SceneValue, {0.006, 0.0, -0.13});
    GatherSettings nearContinuous = nearSettings;
    nearContinuous.Mask = MaskMode::ContinuousIntervals;
    nearContinuous.Sections = 32;
    const auto nearPixel = GatherPixel(nearScene.Input, nearScene.Geometry, nx, ny, nearContinuous);
    const auto nearTruth = IntegrateGroundTruth(nearScene.SceneValue, nearScene.Input, nearScene.Geometry, nx, ny, 0.5);
    std::printf("DGI_REPORT T08_near receivers_at_near_plane=%zu receivers_inside_R_ball=%zu capped_extents=%zu "
                "nonfinite=%zu beside_box J_cont=%.4f J_truth=%.4f A_cont=%.4f A_truth=%.4f\n",
                nearReceivers, insideBall, capped, nearNonFinite, nearPixel.J.R, nearTruth.J.R, nearPixel.ARaw,
                nearTruth.A);
    EXPECT_GT(nearReceivers, 200U);
    EXPECT_GT(insideBall, 5000U);
    EXPECT_EQ(capped, insideBall);
    EXPECT_EQ(nearNonFinite, 0U);
    EXPECT_GT(nearPixel.J.R, 0.0);

    // Segments crossing the R limit are clipped to the sphere.
    const Rendered t06 = Render(BlackOccluderScene(128, 96, true));
    const GatherSettings t06Settings = Make(5.0, 1.0, 64);
    size_t segments = 0, clipped = 0, outside = 0;
    double clipError = 0.0;
    for (uint32_t y = 50; y < 96; y += 4)
        for (uint32_t x = 4; x < 124; x += 6)
            for (uint32_t s = 0; s < 8; ++s) {
                const auto built = BuildSection(t06.Input, t06.Geometry, x, y, kPi * (s + 0.5) / 8.0, t06Settings);
                for (const auto& c : built.Ordered) {
                    ++segments;
                    const double front = std::sqrt(Dot(Add(c.Front, Scale(built.Setup.P, -1.0)),
                                                       Add(c.Front, Scale(built.Setup.P, -1.0))));
                    const Vec3 back = Add(c.Back, Scale(built.Setup.P, -1.0));
                    const double distance = std::sqrt(Dot(back, back));
                    const Vec3 segment = Add(c.Back, Scale(c.Front, -1.0));
                    if (front > 5.0 * (1.0 + 1.0e-12) || distance > 5.0 * (1.0 + 1.0e-12)) ++outside;
                    if (std::sqrt(Dot(segment, segment)) < 1.0 - 1.0e-9) {
                        ++clipped;
                        clipError = std::max(clipError, std::abs(distance - 5.0));
                    }
                }
            }
    std::printf("DGI_REPORT T08_radius segments=%zu clipped_at_R=%zu outside_R=%zu clip_max_abs=%.2e\n", segments,
                clipped, outside, clipError);
    EXPECT_EQ(outside, 0U);
    EXPECT_GT(clipped, 0U);
    EXPECT_LT(clipError, 1.0e-9);

    // Zero thickness: empty intervals, no light, A of the open estimator.
    const Rendered pillars = Render(PillarsScene(96, 64));
    const GatherSettings zero = Make(3.0, 0.0, 64);
    const auto zeroImage = GatherImage(pillars.Input, zero);
    size_t lit = 0;
    double openError = 0.0;
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 96; ++x) {
            const auto& p = zeroImage[At(pillars, x, y)];
            if (!(p.J == Rgb{}) || !Finite(p)) ++lit;
            if (p.Valid) openError = std::max(openError, std::abs(p.ARaw - OpenAccessibility(pillars, x, y, zero)));
        }
    std::printf("DGI_REPORT T08_zero_thickness lit_pixels=%zu open_A_max_abs=%.2e\n", lit, openError);
    EXPECT_EQ(lit, 0U);
    EXPECT_LT(openError, 1.0e-12);
}

TEST(DiffuseGiGather, T09ScreenOrderEqualsFirstHitAmongSegments) {
    // Within a section, every candidate segment lies on its own camera ray, and
    // along any direction from the receiver the screen distance grows with the
    // distance from the receiver: screen order is the first hit among the
    // represented segments. Oracle: nearest segment along each sector centre.
    struct Item {
        Scene SceneValue;
        double Radius, Thickness;
    };
    const std::vector<Item> items{{BlackOccluderScene(128, 96, true), 5.0, 1.0},
                                  {ThinBarScene(128, 96), 4.0, 0.2},
                                  {PillarsScene(128, 96), 3.0, 0.5},
                                  {ThickBlockScene(128, 96), 8.0, 0.5}};
    size_t claimed = 0, mismatched = 0, tieImages = 0, tieDifferent = 0;
    for (const auto& item : items) {
        const Rendered r = Render(item.SceneValue);
        GatherSettings settings = Make(item.Radius, item.Thickness, 64);
        for (uint32_t y = 1; y < 96; y += 3)
            for (uint32_t x = 1; x < 128; x += 3) {
                const Vec3& n = r.Geometry.Normal[At(r, x, y)];
                if (!(Dot(n, n) > 0.0)) continue;
                for (uint32_t s = 0; s < 8; ++s) {
                    const auto built = BuildSection(r.Input, r.Geometry, x, y, kPi * (s + 0.5) / 8.0, settings);
                    const auto acc = AccumulateSection(built.Setup, built.Ordered, 64, MaskMode::CentreRule);
                    for (uint32_t j = 0; j < 64; ++j) {
                        if (acc.Claimant[j] < 0) continue;
                        ++claimed;
                        const double theta = -kPi + (j + 0.5) * 2.0 * kPi / 64.0;
                        const double ux = std::cos(theta), uy = std::sin(theta);
                        int32_t nearest = -1;
                        double nearestDistance = 0.0;
                        for (size_t c = 0; c < built.Ordered.size(); ++c) {
                            if (!Has(CoverOf(built.Ordered[c], 64), j)) continue;
                            const Vec3 f = Add(built.Ordered[c].Front, Scale(built.Setup.P, -1.0));
                            const Vec3 b = Add(built.Ordered[c].Back, Scale(built.Setup.P, -1.0));
                            const double fx = Dot(f, built.Setup.V), fy = Dot(f, built.Setup.T);
                            const double ex = Dot(b, built.Setup.V) - fx, ey = Dot(b, built.Setup.T) - fy;
                            const double denominator = ux * ey - uy * ex;
                            if (!(std::abs(denominator) > 0.0)) continue;
                            const double distance = (fx * ey - fy * ex) / denominator;
                            if (nearest < 0 || distance < nearestDistance) {
                                nearest = static_cast<int32_t>(c);
                                nearestDistance = distance;
                            }
                        }
                        if (nearest != acc.Claimant[j]) ++mismatched;
                    }
                }
            }
        // Tie-break: + side first. The two sides are disjoint in theta, so
        // interleaving or processing the + side entirely first claims the
        // same sectors; only the floating-point summation order changes.
        const auto interleaved = GatherImage(r.Input, settings);
        settings.InterleaveSides = false;
        const auto sequential = GatherImage(r.Input, settings);
        ++tieImages;
        for (size_t i = 0; i < interleaved.size(); ++i) {
            const auto& a = interleaved[i];
            const auto& b = sequential[i];
            const double scale = std::max({1.0, a.J.R, a.J.G, a.J.B});
            if (a.OccupiedSectors != b.OccupiedSectors || a.Counters.Scheduled != b.Counters.Scheduled ||
                a.Counters.Occupying != b.Counters.Occupying || std::abs(a.J.R - b.J.R) > 1.0e-14 * scale ||
                std::abs(a.J.G - b.J.G) > 1.0e-14 * scale || std::abs(a.J.B - b.J.B) > 1.0e-14 * scale ||
                std::abs(a.ARaw - b.ARaw) > 1.0e-14)
                ++tieDifferent;
        }
    }
    std::printf("DGI_REPORT T09_oracle claimed_sectors=%zu nearest_segment_mismatches=%zu tie_break_images=%zu "
                "pixels_changed_by_side_order=%zu\n",
                claimed, mismatched, tieImages, tieDifferent);
    EXPECT_GT(claimed, 100000U);
    EXPECT_EQ(mismatched, 0U);
    EXPECT_EQ(tieDifferent, 0U);
}

TEST(DiffuseGiGather, T09ErrorsAgainstTheThreeDimensionalFirstHit) {
    // Where the screen order differs from the true first 3D hit, by cause.
    struct Probe {
        const char* Cause;
        Scene SceneValue;
        Vec3 Point;
        double Radius, Thickness;
    };
    const std::vector<Probe> probes{
        {"hidden_from_camera(T06 wall behind box)", BlackOccluderScene(288, 192, true), {0.9, -1.0, -6.0}, 5.0, 1.0},
        {"thickness(T07 block, tau 0.5 of 3.5)", ThickBlockScene(288, 192), {0.9, -1.0, -6.5}, 8.0, 0.5},
        {"front_beyond_R(overhang underside)", OverhangScene(288, 192), {0.2, -1.0, -6.2}, 2.0, 0.5},
        {"reference(T06 without box)", BlackOccluderScene(288, 192, false), {0.9, -1.0, -6.0}, 5.0, 1.0}};
    for (const auto& probe : probes) {
        const Rendered r = Render(probe.SceneValue);
        const auto [x, y] = PixelOf(r.SceneValue, probe.Point);
        GatherSettings s = Make(probe.Radius, probe.Thickness, 64, 32);
        s.Mask = MaskMode::ContinuousIntervals;
        const auto screen = GatherPixel(r.Input, r.Geometry, x, y, s);
        const auto truth = IntegrateGroundTruth(r.SceneValue, r.Input, r.Geometry, x, y, probe.Radius);
        std::printf("DGI_REPORT T09_vs_3D cause=%s J_screen=%.4f J_truth=%.4f dJ=%+.4f A_screen=%.4f A_truth=%.4f "
                    "dA=%+.4f\n",
                    probe.Cause, screen.J.R, truth.J.R, screen.J.R - truth.J.R, screen.ARaw, truth.A,
                    screen.ARaw - truth.A);
        EXPECT_TRUE(truth.Converged);
        if (std::string(probe.Cause).rfind("front_beyond_R", 0) == 0) EXPECT_GT(screen.ARaw, truth.A + 0.01);
        if (std::string(probe.Cause).rfind("hidden", 0) == 0) EXPECT_LT(screen.J.R, truth.J.R);
    }
}

TEST(DiffuseGiGather, GeometricNormalSlopedPlaneSelfIntersection) {
    // M1 open point 3. A plane cannot light or occlude itself: J = 0 and A
    // equals the open estimator. Bias of each geometric-normal source.
    struct Variant {
        const char* Name;
        bool Face, Quantize, Reject;
        double Sine;
    };
    const std::vector<Variant> variants{{"depth_reconstruction", false, false, true, 2.0e-3},
                                        {"face_normal_f32", true, false, true, 2.0e-3},
                                        {"guide_rgb8_as_geometric", true, true, true, 2.0e-3},
                                        {"guide_rgb8_sine_8e-3", true, true, true, 8.0e-3},
                                        {"depth_without_rejection", false, false, false, 2.0e-3}};
    for (const double pitchDeg : {30.0, 60.0, 75.0})
        for (const DepthStorage storage : {DepthStorage::Float32, DepthStorage::Unorm24}) {
            if (storage == DepthStorage::Unorm24 && pitchDeg != 60.0) continue;
            Scene scene = SlopedPlaneScene(128, 96, pitchDeg * kDegree);
            scene.Camera.Storage = storage;
            const GatherInput base = Rasterize(scene);
            for (const auto& variant : variants) {
                Rendered r{scene, base, {}};
                if (variant.Face) r.Input.NormalSource = GeometricNormalSource::RendererFaceNormal;
                if (variant.Quantize) QuantizeFaceNormals8(r.Input);
                r.Geometry = PrepareGeometry(r.Input);
                GatherSettings s = Make(1.5, 0.5, 64);
                s.RejectSelfIntersection = variant.Reject;
                s.SelfIntersectionSine = variant.Sine;
                const auto image = GatherImage(r.Input, s);
                // Exact plane normal, oriented toward the eye.
                Vec3 exact = scene.Primitives.front().Axes[2];
                if (Dot(exact, scene.Primitives.front().Centre) > 0.0) exact = Scale(exact, -1.0);
                double sumJ = 0.0, maxJ = 0.0, sumBias = 0.0, maxBias = 0.0, sumAngle = 0.0, maxAngle = 0.0;
                size_t count = 0;
                for (uint32_t y = 8; y < 88; y += 2)
                    for (uint32_t x = 8; x < 120; x += 2) {
                        const auto& p = image[At(r, x, y)];
                        if (!p.Valid) continue;
                        const double bias = OpenAccessibility(r, x, y, s) - p.ARaw;
                        const Vec3& n = r.Geometry.Normal[At(r, x, y)];
                        const double angle =
                            std::atan2(std::sqrt(Dot(Cross(n, exact), Cross(n, exact))), Dot(n, exact)) / kDegree;
                        sumJ += p.J.R;
                        maxJ = std::max(maxJ, p.J.R);
                        sumBias += bias;
                        maxBias = std::max(maxBias, std::abs(bias));
                        sumAngle += angle;
                        maxAngle = std::max(maxAngle, angle);
                        ++count;
                    }
                std::printf("DGI_REPORT normal_sloped pitch=%.0f depth=%s normal=%s receivers=%zu mean_J=%.2e max_J=%.2e "
                            "mean_A_bias=%.2e max_abs_A_bias=%.2e normal_error_deg mean=%.2e max=%.2e\n",
                            pitchDeg, storage == DepthStorage::Float32 ? "f32" : "unorm24", variant.Name, count,
                            sumJ / count, maxJ, sumBias / count, maxBias, sumAngle / count, maxAngle);
                // Requirement (geometric-normal decision): the depth-derived
                // normal stays comparable with the renderer face normal.
                if (!variant.Face) EXPECT_LT(maxAngle, 0.05) << variant.Name;
                if (!variant.Face && variant.Reject) {
                    EXPECT_EQ(maxJ, 0.0);
                    EXPECT_LT(maxBias, 1.0e-6);
                }
                if (variant.Quantize && variant.Sine == 2.0e-3 && pitchDeg != 75.0) EXPECT_GT(sumBias / count, 1.0e-4);
                if (!variant.Reject) EXPECT_GT(sumBias / count, 1.0e-2);
            }
        }
    // The tolerance ignores occluders below asin(2e-3) of elevation: at most
    // sin^2 of that angle of the cosine-weighted hemisphere.
    std::printf("DGI_REPORT normal_tolerance sine=2e-3 ignored_band_weight=%.2e sine=8e-3 ignored_band_weight=%.2e\n",
                std::pow(2.0e-3, 2.0), std::pow(8.0e-3, 2.0));
    // A genuine low step (height 0.05) in front of floor receivers: rejection
    // removes the receiver's own floor and never a step candidate.
    for (const uint32_t k : {3U, 9U}) {
        Scene step = BlackOccluderScene(96 * k, 64 * k, false);
        step.Primitives.push_back(Primitive::Box({0.0, -0.975, -5.0}, {1.0, 0.025, 0.3}, {}));
        const Rendered r = Render(step);
        for (const Vec3 point : {Vec3{0.2, -1.0, -4.55}, Vec3{-0.3, -1.0, -4.62}}) {
            const auto [x, y] = PixelOf(step, point);
            GatherSettings s = Make(1.0, 0.6, 64, 32);
            size_t stepWith = 0, stepWithout = 0, floorWith = 0, floorWithout = 0;
            // Classify by the texel-centre point (the exact surface sample; the
            // line point carries the footprint displacement).
            const auto onStep = [&](const Candidate& c) {
                return r.Geometry.Position(r.Input.Camera, x + c.CellX, y + c.CellY).Y > -0.999;
            };
            for (uint32_t section = 0; section < s.Sections; ++section) {
                const double phi = kPi * (section + 0.5) / s.Sections;
                s.RejectSelfIntersection = true;
                for (const auto& c : BuildSection(r.Input, r.Geometry, x, y, phi, s).Ordered)
                    ++(onStep(c) ? stepWith : floorWith);
                s.RejectSelfIntersection = false;
                for (const auto& c : BuildSection(r.Input, r.Geometry, x, y, phi, s).Ordered)
                    ++(onStep(c) ? stepWithout : floorWithout);
            }
            s.RejectSelfIntersection = true;
            s.Mask = MaskMode::ContinuousIntervals;
            const auto screen = GatherPixel(r.Input, r.Geometry, x, y, s);
            const auto truth = IntegrateGroundTruth(step, r.Input, r.Geometry, x, y, 1.0);
            const double open = OpenAccessibility(r, x, y, s);
            std::printf("DGI_REPORT normal_step k=%u receiver=(%.2f,%.2f) step_candidates_kept=%zu/%zu "
                        "floor_candidates_kept=%zu/%zu A_open=%.4f A_screen=%.4f A_truth=%.4f\n",
                        k, point.X, point.Z, stepWith, stepWithout, floorWith, floorWithout, open, screen.ARaw, truth.A);
            EXPECT_GT(stepWithout, 0U);
            EXPECT_EQ(stepWith, stepWithout);
            EXPECT_EQ(floorWith, 0U);
            EXPECT_LT(screen.ARaw, open - 0.01);
            if (k == 9U) EXPECT_NEAR(screen.ARaw, truth.A, 0.03);
        }
    }
}

TEST(DiffuseGiGather, GeometricNormalNeverMixesTwoPlanes) {
    // Wherever the texel's own face extends over a quadrant's full stencil
    // (two texels along each axis and the diagonal), the depth-derived normal
    // must be the exact face normal of the texel or of a 3x3 neighbour (a
    // texel on an edge may take either plane), never a vector mixed from the
    // tangents of two planes. Faces thinner than three texels (seen at grazing
    // angles) are ambiguous for any 5-tap depth stencil: counted separately,
    // and the reason renderer face normals stay a declared input.
    size_t texels = 0, own = 0, neighbour = 0, mixed = 0, missing = 0, subPixel = 0, subPixelMixed = 0;
    for (const Scene& scene : {CornerRoomScene(96, 64), CornerRoomScene(160, 108), PillarsScene(192, 128),
                               PanelScene(288, 192, false), ThickBlockScene(131, 77), WordBoundaryScene(128, 96),
                               BlackOccluderScene(192, 128, true), NearPlaneScene(128, 96)}) {
        const Rendered r = Render(scene);
        const uint32_t w = scene.Camera.Width, h = scene.Camera.Height;
        const auto face = [&](uint32_t x, uint32_t y) {
            const size_t bi = static_cast<size_t>(y) * r.Input.BufferWidth + x;
            return Vec3{r.Input.FaceNormals[3 * bi], r.Input.FaceNormals[3 * bi + 1], r.Input.FaceNormals[3 * bi + 2]};
        };
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t i = At(r, x, y);
                if (r.Geometry.Class[i] != static_cast<uint8_t>(TexelClass::OneSided) &&
                    r.Geometry.Class[i] != static_cast<uint8_t>(TexelClass::TwoSided))
                    continue;
                ++texels;
                // Same plane: same face normal and same offset n . X.
                const Vec3 nf = face(x, y);
                const double offset = Dot(nf, r.Geometry.Position(r.Input.Camera, x, y));
                const auto sameFace = [&](int xx, int yy) {
                    if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h)) return false;
                    if (r.Geometry.Class[At(r, xx, yy)] != r.Geometry.Class[i] || Dot(face(xx, yy), nf) < 0.9999)
                        return false;
                    const Vec3 q = r.Geometry.Position(r.Input.Camera, xx, yy);
                    return std::abs(Dot(nf, q) - offset) < 1.0e-4 * std::sqrt(Dot(q, q));
                };
                bool resolvable = false;
                for (const int sx : {1, -1})
                    for (const int sy : {1, -1})
                        resolvable = resolvable || (sameFace(x + sx, y) && sameFace(x + 2 * sx, y) &&
                                                    sameFace(x, y + sy) && sameFace(x, y + 2 * sy) &&
                                                    sameFace(x + sx, y + sy));
                const Vec3& n = r.Geometry.Normal[i];
                if (!(Dot(n, n) > 0.0)) {
                    ++missing;
                    continue;
                }
                bool found = Dot(n, face(x, y)) > 0.9999;
                if (found) ++own;
                for (int dy = -1; dy <= 1 && !found; ++dy)
                    for (int dx = -1; dx <= 1 && !found; ++dx) {
                        const int xx = static_cast<int>(x) + dx, yy = static_cast<int>(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h)) continue;
                        found = Dot(n, face(xx, yy)) > 0.9999;
                        if (found) ++neighbour;
                    }
                if (!resolvable) {
                    ++subPixel;
                    if (!found) ++subPixelMixed;
                } else if (!found) {
                    ++mixed;
                    if (mixed <= 8) {
                        std::printf("mixed scene=%s pixel=(%u,%u) n=(%.3f %.3f %.3f) face=(%.3f %.3f %.3f) z:",
                                    scene.Name.c_str(), x, y, n.X, n.Y, n.Z, face(x, y).X, face(x, y).Y, face(x, y).Z);
                        for (int dy = -2; dy <= 2; ++dy) {
                            std::printf(" |");
                            for (int dx = -2; dx <= 2; ++dx) {
                                const int xx = static_cast<int>(x) + dx, yy = static_cast<int>(y) + dy;
                                if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
                                    std::printf("   --  ");
                                else
                                    std::printf(" %6.3f", r.Geometry.ViewDepth[At(r, xx, yy)]);
                            }
                        }
                        std::printf("\n");
                    }
                }
            }
    }
    std::printf("DGI_REPORT normal_planes texels=%zu own_face=%zu neighbour_face=%zu mixed_where_resolvable=%zu "
                "missing=%zu faces_thinner_than_3_texels=%zu mixed_there=%zu\n",
                texels, own, neighbour, mixed, missing, subPixel, subPixelMixed);
    EXPECT_EQ(mixed, 0U);
    EXPECT_EQ(missing, 0U);
    EXPECT_LT(neighbour + subPixel, texels / 50);
}

TEST(DiffuseGiGather, SectorCountStudy) {
    // M1 open point 5: centre-rule sectors against the continuous intervals of
    // the same candidates, on scenes of realistic angular size (S = 8).
    struct Item {
        Scene SceneValue;
        double Radius, Thickness;
    };
    const std::vector<Item> items{{CornerRoomScene(192, 128), 2.5, 0.5},
                                  {PillarsScene(192, 128), 3.0, 0.5},
                                  {BlackOccluderScene(192, 128, true), 5.0, 1.0},
                                  {ThinBarScene(192, 128), 4.0, 0.5}};
    const std::vector<uint32_t> counts{8, 16, 24, 32, 48, 64, 96, 128, 256};
    struct Stats {
        double SumJ = 0.0, SumRef = 0.0, SumAbsRel = 0.0, SumAbsA = 0.0, MaxAbsA = 0.0;
        std::vector<double> Rel;
        size_t Worse = 0, Pairs = 0, Pixels = 0;
        [[nodiscard]] double Bias() const { return SumJ / SumRef - 1.0; }
        [[nodiscard]] double MeanRel() const { return SumAbsRel / std::max<size_t>(Rel.size(), 1); }
    };
    // S = 8: one frame. S = 64 at rotation 1/2 is the union of the S = 8 sets of
    // the 8 rotations (k + 1/2) / 8: what temporal accumulation of 8 frames sees.
    const auto study = [&](uint32_t sections, uint32_t stride) {
        std::vector<Stats> stats(counts.size());
        for (const auto& item : items) {
            const Rendered r = Render(item.SceneValue);
            const GatherSettings settings = Make(item.Radius, item.Thickness, 64, sections);
            for (uint32_t y = 0; y < 128; y += stride)
                for (uint32_t x = 0; x < 192; x += stride) {
                    const Vec3& n = r.Geometry.Normal[At(r, x, y)];
                    if (!(Dot(n, n) > 0.0)) continue;
                    std::vector<double> j(counts.size(), 0.0), a(counts.size(), 0.0);
                    double jRef = 0.0, aRef = 0.0;
                    for (uint32_t s = 0; s < sections; ++s) {
                        const auto built =
                            BuildSection(r.Input, r.Geometry, x, y, kPi * (s + 0.5) / sections, settings);
                        const auto exact =
                            AccumulateSection(built.Setup, built.Ordered, 64, MaskMode::ContinuousIntervals);
                        jRef += kPi * (exact.F.R + exact.F.G + exact.F.B) / sections;
                        aRef += kPi * exact.FA / sections;
                        for (size_t k = 0; k < counts.size(); ++k) {
                            const auto quantized =
                                AccumulateSection(built.Setup, built.Ordered, counts[k], MaskMode::CentreRule);
                            j[k] += kPi * (quantized.F.R + quantized.F.G + quantized.F.B) / sections;
                            a[k] += kPi * quantized.FA / sections;
                        }
                    }
                    for (size_t k = 0; k < counts.size(); ++k) {
                        Stats& st = stats[k];
                        ++st.Pixels;
                        st.SumJ += j[k];
                        st.SumRef += jRef;
                        st.SumAbsA += std::abs(a[k] - aRef);
                        st.MaxAbsA = std::max(st.MaxAbsA, std::abs(a[k] - aRef));
                        if (jRef > 0.02) {
                            const double rel = std::abs(j[k] - jRef) / jRef;
                            st.SumAbsRel += rel;
                            st.Rel.push_back(rel);
                            if (k > 0) {
                                ++st.Pairs;
                                if (rel > std::abs(j[k - 1] - jRef) / jRef + 1.0e-12) ++st.Worse;
                            }
                        }
                    }
                }
        }
        for (size_t k = 0; k < counts.size(); ++k) {
            Stats& st = stats[k];
            std::sort(st.Rel.begin(), st.Rel.end());
            const double p95 = st.Rel.empty() ? 0.0 : st.Rel[st.Rel.size() * 95 / 100];
            std::printf("DGI_REPORT sectors S=%u N=%u energy_bias=%+.2e mean_abs_rel_J=%.2e p95_rel_J=%.2e "
                        "mean_abs_A=%.2e max_abs_A=%.2e worse_than_previous_N=%.1f%% (pixels=%zu lit=%zu)\n",
                        sections, counts[k], st.Bias(), st.MeanRel(), p95, st.SumAbsA / st.Pixels, st.MaxAbsA,
                        st.Pairs ? 100.0 * st.Worse / st.Pairs : 0.0, st.Pixels, st.Rel.size());
        }
        return stats;
    };
    const auto frame = study(8, 2);
    const auto accumulated = study(64, 6);
    // Decision (document): N = 64 by default. Aggregate energy bias below
    // 0.5%; the per-pixel quantization error is noise-like (it shrinks under
    // accumulation) and decreases monotonically with N in aggregate, although
    // individual pixels do not.
    const size_t chosen = 5;
    ASSERT_EQ(counts[chosen], 64U);
    EXPECT_EQ(GatherSettings{}.Sectors, 64U);
    EXPECT_LT(std::abs(frame[chosen].Bias()), 0.005);
    EXPECT_LT(std::abs(accumulated[chosen].Bias()), 0.005);
    EXPECT_LT(accumulated[chosen].MeanRel(), 0.02);
    EXPECT_LT(accumulated[chosen].MeanRel(), 0.5 * frame[chosen].MeanRel());
    for (const auto* stats : {&frame, &accumulated})
        for (size_t k = 1; k < counts.size(); ++k) EXPECT_LT((*stats)[k].MeanRel(), (*stats)[k - 1].MeanRel());
}

TEST(DiffuseGiGather, Fp32TwinAgainstReference) {
    // FP32 tolerance on the CPU (the GPU kernel follows the same operations):
    // isolated discrete decisions flip; each flip is bounded by one sector.
    struct Item {
        Scene SceneValue;
        double Radius, Thickness;
        uint32_t Sectors;
    };
    const std::vector<Item> items{{CornerRoomScene(131, 77), 2.5, 0.5, 32},
                                  {BlackOccluderScene(131, 77, true), 5.0, 1.0, 64},
                                  {ThinBarScene(131, 77), 4.0, 0.1, 256}};
    for (const auto& item : items) {
        const Rendered r = Render(item.SceneValue);
        const GatherSettings s = Make(item.Radius, item.Thickness, item.Sectors);
        const auto f64 = GatherImage(r.Input, s);
        const auto f32 = GatherImageF32(r.Input, s);
        double mean = 0.0, maxJ = 0.0, maxA = 0.0;
        size_t over = 0, validMismatch = 0;
        for (size_t i = 0; i < f64.size(); ++i) {
            const double d = std::max({std::abs(f32[i].J.R - f64[i].J.R), std::abs(f32[i].J.G - f64[i].J.G),
                                       std::abs(f32[i].J.B - f64[i].J.B)}) /
                             std::max({1.0, f64[i].J.R, f64[i].J.G, f64[i].J.B});
            mean += d;
            maxJ = std::max(maxJ, d);
            maxA = std::max(maxA, std::abs(f32[i].ARaw - f64[i].ARaw));
            if (d > 1.0e-4 || std::abs(f32[i].ARaw - f64[i].ARaw) > 1.0e-4) ++over;
            if (f32[i].Valid != f64[i].Valid) ++validMismatch;
        }
        const double bound = SingleFlipBound(item.Sectors, s.Sections);
        std::printf("DGI_REPORT fp32 scene=%s N=%u mean_rel_J=%.2e max_rel_J=%.2e max_abs_A=%.2e pixels_over_1e-4=%zu/%zu "
                    "single_flip_bound=%.2e valid_mismatch=%zu\n",
                    item.SceneValue.Name.c_str(), item.Sectors, mean / f64.size(), maxJ, maxA, over, f64.size(), bound,
                    validMismatch);
        EXPECT_EQ(validMismatch, 0U);
        EXPECT_LT(mean / f64.size(), 2.0e-5);
        EXPECT_LT(over, f64.size() / 200);
        EXPECT_LT(maxA, 2.0 * bound);
    }
}
