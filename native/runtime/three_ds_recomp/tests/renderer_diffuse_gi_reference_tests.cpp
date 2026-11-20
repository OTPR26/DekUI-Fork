// M1 acceptance tests T01-T04 of the diffuse screen-space GI specification,
// on synthetic angular fields (no depth buffer). Lines starting with
// DGI_REPORT are the reproducible numbers quoted in
// docs/TRIAEVUM_SCREEN_SPACE_GI.md.
#include "fast/renderer/diffuse_gi_reference.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Fast::Renderer::DiffuseGi;

namespace {

FieldSample Hit(Rgb radiance, Rgb albedo = {}) { return {DirectionState::Hit, radiance, albedo}; }
FieldSample Open() { return {}; }

const SectionFrame& Frame() {
    static const SectionFrame frame = SectionFrame::FromView(Normalize({0.3, -0.2, 0.93}));
    return frame;
}

// Unit vector at `angle` from v toward t(azimuth).
Vec3 Tilted(double angle, double azimuth = 0.7) {
    return Normalize(Add(Scale(Frame().V, std::cos(angle)), Scale(Frame().Tangent(azimuth), std::sin(angle))));
}

double Rel(double value, double reference) { return std::abs(value - reference) / std::abs(reference); }
double MaxRel(const Rgb& value, const Rgb& reference) {
    return std::max({Rel(value.R, reference.R), Rel(value.G, reference.G), Rel(value.B, reference.B)});
}
bool Finite(const Estimate& e) {
    return std::isfinite(e.J.R) && std::isfinite(e.J.G) && std::isfinite(e.J.B) && std::isfinite(e.ARaw) &&
           std::isfinite(e.Confidence);
}
Rgb Times(const Rgb& c, double s) { return {c.R * s, c.G * s, c.B * s}; }
constexpr double kDegree = kPi / 180.0;

} // namespace

TEST(DiffuseGiReference, AnalyticSectorWeightsMatchQuadrature) {
    double maxError = 0.0, maxError32 = 0.0, maxMassError = 0.0, maxUnsplitError = 0.0;
    uint32_t maxOrder = 0, maxUnsplitOrder = 0, cases = 0;
    bool unsplitConverged = true;
    QuadratureOptions unsplit;
    unsplit.SplitAtKinks = false;
    for (double tilt : {0.0, 10.0, 45.0, 80.0, 89.9, 90.0, 120.0, 179.0}) {
        for (int k = 0; k < 37; ++k) {
            const double phi = kPi * (k + 0.25) / 37.0;
            const Vec3 n = Tilted(tilt * kDegree, 1.1);
            const double a = Dot(n, Frame().V), b = Dot(n, Frame().Tangent(phi));
            double sum = 0.0;
            for (uint32_t j = 0; j < 32; ++j) {
                const double lo = -kPi + 2.0 * kPi * j / 32.0, hi = -kPi + 2.0 * kPi * (j + 1) / 32.0;
                const double analytic = SectorWeightAnalytic(a, b, lo, hi);
                const auto reference = SectorWeightQuadrature(a, b, lo, hi);
                ASSERT_TRUE(reference.Converged);
                maxError = std::max(maxError, std::abs(analytic - reference.Value));
                maxOrder = std::max(maxOrder, reference.MaxOrder);
                const float single = SectorWeightAnalyticF32(static_cast<float>(a), static_cast<float>(b),
                                                             static_cast<float>(lo), static_cast<float>(hi));
                maxError32 = std::max(maxError32, std::abs(static_cast<double>(single) - reference.Value));
                const auto rough = SectorWeightQuadrature(a, b, lo, hi, unsplit);
                maxUnsplitError = std::max(maxUnsplitError, std::abs(rough.Value - reference.Value));
                maxUnsplitOrder = std::max(maxUnsplitOrder, rough.MaxOrder);
                unsplitConverged = unsplitConverged && rough.Converged;
                sum += analytic;
                ++cases;
            }
            maxMassError = std::max(maxMassError, std::abs(sum - SectionMass(a, b)));
        }
    }
    std::printf("DGI_REPORT weights cases=%u analytic_f64_max_abs_error=%.3e quadrature_max_order=%u "
                "analytic_f32_max_abs_error=%.3e mass_closed_form_max_abs_error=%.3e\n",
                cases, maxError, maxOrder, maxError32, maxMassError);
    std::printf("DGI_REPORT rejected_unsplit_quadrature max_abs_error=%.3e max_order=%u all_converged=%d\n",
                maxUnsplitError, maxUnsplitOrder, unsplitConverged ? 1 : 0);
    EXPECT_LT(maxError, 1.0e-14);
    EXPECT_LT(maxError32, 2.0e-6);
    EXPECT_LT(maxMassError, 1.0e-14);
}

TEST(DiffuseGiReference, SectorWeightsIntegrateToOneAndCumulativeIsWellFormed) {
    const auto& rule = GaussLegendre(16);
    double maxError = 0.0;
    for (double tilt : {0.0, 30.0, 60.0, 89.0, 90.0, 135.0, 180.0}) {
        const Vec3 n = Tilted(tilt * kDegree, 0.3);
        // M(phi) has a kink where n.t(phi) = 0 when n.v = 0: split there.
        const double kink = std::fmod(std::atan2(-Dot(n, Frame().E1), Dot(n, Frame().E2)) + 2.0 * kPi, kPi);
        double total = 0.0;
        for (const auto [from, to] : {std::pair{0.0, kink}, std::pair{kink, kPi}})
            for (uint32_t panel = 0; panel < 64; ++panel)
                for (size_t i = 0; i < rule.Nodes.size(); ++i) {
                    const double width = (to - from) / 64.0;
                    const double phi = from + width * (panel + 0.5 + 0.5 * rule.Nodes[i]);
                    total += rule.Weights[i] * 0.5 * width *
                             SectionMass(Dot(n, Frame().V), Dot(n, Frame().Tangent(phi)));
                }
        maxError = std::max(maxError, std::abs(total - 1.0));
    }
    std::printf("DGI_REPORT normalization integral_over_phi_max_abs_error=%.3e\n", maxError);
    EXPECT_LT(maxError, 1.0e-12);

    // Cumulative (section 12): monotone, 0 and 1 at the ends, no division at
    // zero mass, wrap-around intervals split at +-pi.
    for (const auto [a, b] : {std::pair{0.6, -0.8}, std::pair{-0.9, 0.1}, std::pair{0.0, 1.0}}) {
        double previous = 0.0;
        for (int i = 0; i <= 1000; ++i) {
            const double c = Cumulative(a, b, -kPi + 2.0 * kPi * i / 1000.0);
            EXPECT_GE(c, previous);
            previous = c;
        }
        EXPECT_EQ(Cumulative(a, b, -kPi), 0.0);
        EXPECT_NEAR(Cumulative(a, b, kPi), 1.0, 1.0e-15);
        EXPECT_NEAR(IntervalWeight(a, b, 3.0, 3.5),
                    SectorWeightAnalytic(a, b, 3.0, kPi) + SectorWeightAnalytic(a, b, -kPi, 3.5 - 2.0 * kPi), 1e-16);
    }
    EXPECT_EQ(SectionMass(0.0, 0.0), 0.0);
    EXPECT_EQ(Cumulative(0.0, 0.0, 1.0), 0.0);
    EXPECT_EQ(SectorWeightAnalytic(0.0, 0.0, -kPi, kPi), 0.0);
}

TEST(DiffuseGiReference, T01NormalizationConvergesToTheConstantField) {
    const Rgb colour{0.25, 1.0, 3.5};
    const CapField hits(Hit(colour));
    const CapField open(Open());
    // Normal along v: every section is exact, so a single section suffices.
    SectorEstimatorSettings settings;
    settings.Sections = 1;
    const auto exact = EstimateSectors(hits, Frame(), Frame().V, settings);
    EXPECT_LT(MaxRel(exact.J, colour), 1.0e-12);
    EXPECT_EQ(exact.ARaw, 0.0);
    settings.Fallback = colour;
    const auto exactOpen = EstimateSectors(open, Frame(), Frame().V, settings);
    EXPECT_LT(MaxRel(exactOpen.J, colour), 1.0e-12);
    EXPECT_NEAR(exactOpen.A, 1.0, 1.0e-12);
    for (double tilt : {30.0, 60.0, 85.0, 120.0}) {
        const Vec3 n = Tilted(tilt * kDegree);
        std::printf("DGI_REPORT T01 tilt_deg=%.0f", tilt);
        for (uint32_t sections : {1U, 2U, 4U, 8U, 16U, 64U, 256U}) {
            settings.Sections = sections;
            const auto e = EstimateSectors(open, Frame(), n, settings);
            std::printf(" S%u=%.2e", sections, std::max(MaxRel(e.J, colour), std::abs(e.ARaw - 1.0)));
            if (sections >= 16U) {
                EXPECT_LT(MaxRel(e.J, colour), 0.01) << tilt << " " << sections;
                EXPECT_LT(std::abs(e.ARaw - 1.0), 0.01);
            }
        }
        // Randomized rotations (golden-ratio sequence), one section each: the
        // mean converges; single estimates need not be exact.
        SectionAccumulator mean;
        double worstSingle = 0.0;
        for (int k = 0; k < 4096; ++k) {
            const double u = std::fmod(0.5 + k * 0.6180339887498949, 1.0);
            const auto c = EvaluateSection(open, Frame(), n, kPi * u, 32, colour);
            mean.Add(c);
            worstSingle = std::max(worstSingle, std::abs(c.FA / c.Pdf - 1.0));
        }
        const auto m = mean.Result();
        const auto continuous = IntegrateContinuous(open, Frame(), n, colour);
        std::printf(" mean4096=%.2e worst_single=%.2e continuous=%.2e\n",
                    std::max(MaxRel(m.J, colour), std::abs(m.ARaw - 1.0)), worstSingle, MaxRel(continuous.J, colour));
        EXPECT_LT(MaxRel(m.J, colour), 0.01);
        EXPECT_LT(MaxRel(continuous.J, colour), 1.0e-9);
        EXPECT_NEAR(continuous.A, 1.0, 1.0e-9);
    }
}

TEST(DiffuseGiReference, T02KnownRegionsWithoutRenormalization) {
    const Rgb light{2.0, 1.0, 0.5};
    struct Case {
        const char* Name;
        CapField Field;
        Vec3 Normal;
        double Expected; // J / light
    };
    const Vec3 n = Tilted(40.0 * kDegree);
    const Vec3 side = Normalize(Cross(n, Frame().V));
    const Vec3 tiltedAxis = Normalize(Add(Scale(n, std::cos(30.0 * kDegree)), Scale(side, std::sin(30.0 * kDegree))));
    std::vector<Case> cases;
    for (double alpha : {15.0, 45.0, 75.0})
        cases.push_back({"cap_about_n", CapField(Open(), {{n, alpha * kDegree, Hit(light)}}), n,
                         std::pow(std::sin(alpha * kDegree), 2.0)});
    cases.push_back({"tilted_cap", CapField(Open(), {{tiltedAxis, 20.0 * kDegree, Hit(light)}}), n,
                     std::pow(std::sin(20.0 * kDegree), 2.0) * std::cos(30.0 * kDegree)});
    for (double beta : {0.0, 60.0, 120.0, 170.0}) {
        const Vec3 sky = Normalize(Add(Scale(n, std::cos(beta * kDegree)), Scale(side, std::sin(beta * kDegree))));
        cases.push_back({"half_space", CapField(Open(), {{sky, 0.5 * kPi, Hit(light)}}), n,
                         0.5 * (1.0 + std::cos(beta * kDegree))});
    }
    for (const auto& c : cases) {
        // The reference is the continuous integrator; the sector estimator's
        // quantization error is measured as N grows (centre rule).
        const auto continuous = IntegrateContinuous(c.Field, Frame(), c.Normal, {});
        const Rgb expected = Times(light, c.Expected);
        std::printf("DGI_REPORT T02 %s expected=%.6f continuous_rel=%.2e panels=%u last_correction=%.1e sectors",
                    c.Name, c.Expected, MaxRel(continuous.J, expected), continuous.Panels, continuous.LastCorrection);
        double sectorError = 0.0;
        for (uint32_t sectorCount : {32U, 64U, 128U, 256U}) {
            SectorEstimatorSettings settings;
            settings.Sectors = sectorCount;
            settings.Sections = 128;
            sectorError = MaxRel(EstimateSectors(c.Field, Frame(), c.Normal, settings).J, expected);
            std::printf(" N%u=%.2e", sectorCount, sectorError);
        }
        std::printf("\n");
        EXPECT_LT(continuous.LastCorrection, 1.0e-5) << c.Name;
        EXPECT_LT(MaxRel(continuous.J, expected), 0.02) << c.Name;
        EXPECT_LT(sectorError, 0.02) << c.Name; // N = 256, S = 128
        EXPECT_NEAR(continuous.A, 1.0 - c.Expected, 0.02);
    }
    // Sector-aligned angular interval about v (|theta| in [2, 5] sectors of
    // 32): the centre rule is then exact and so is every single section.
    const double step = 2.0 * kPi / 32.0;
    const CapField band(Open(), {{Frame().V, 2.0 * step, Open()}, {Frame().V, 5.0 * step, Hit(light)}});
    SectorEstimatorSettings settings;
    settings.Sections = 1;
    const auto e = EstimateSectors(band, Frame(), Frame().V, settings);
    const double expected = std::pow(std::sin(5.0 * step), 2.0) - std::pow(std::sin(2.0 * step), 2.0);
    std::printf("DGI_REPORT T02 sector_aligned_band expected=%.6f single_section_rel=%.2e\n", expected,
                MaxRel(e.J, Times(light, expected)));
    EXPECT_LT(MaxRel(e.J, Times(light, expected)), 1.0e-12);
    // No renormalization over the lit sectors: half the cosine mass lit gives
    // half the light, not all of it.
    const auto half = IntegrateContinuous(CapField(Open(), {{n, 45.0 * kDegree, Hit(light)}}), Frame(), n, {});
    EXPECT_NEAR(half.J.R / light.R, 0.5, 1.0e-6);
}

TEST(DiffuseGiReference, T03BlackSourcesAlbedoAndReset) {
    const Vec3 grazing = Normalize(Cross(Frame().V, Frame().E1)); // n . v = 0: some sections have M = 0
    SectionAccumulator accumulator;
    const CapField bright(Hit({5.0, 5.0, 5.0}));
    const CapField black(Hit({0.0, 0.0, 0.0}));
    for (uint32_t s = 0; s < 16; ++s) accumulator.Add(EvaluateSection(bright, Frame(), grazing, kPi * s / 16.0, 32, {}));
    accumulator.Reset();
    EXPECT_EQ(accumulator.Result().Sections, 0U);
    for (uint32_t s = 0; s < 16; ++s) accumulator.Add(EvaluateSection(black, Frame(), grazing, kPi * s / 16.0, 32, {}));
    const auto zero = accumulator.Result();
    EXPECT_EQ(zero.J, (Rgb{0.0, 0.0, 0.0}));
    EXPECT_TRUE(Finite(zero));
    const auto openBlack = EstimateSectors(CapField(Open()), Frame(), grazing, {});
    EXPECT_EQ(openBlack.J, (Rgb{0.0, 0.0, 0.0})); // zero fallback: no local GI
    EXPECT_TRUE(Finite(openBlack));

    // Source albedo at fixed radiance does not change J (it is metadata).
    const Vec3 n = Tilted(35.0 * kDegree);
    const auto withAlbedo = [&](Rgb albedo) {
        return EstimateSectors(CapField(Open(), {{n, 50.0 * kDegree, Hit({0.7, 0.4, 0.2}, albedo)}}), Frame(), n, {});
    };
    EXPECT_EQ(withAlbedo({0.1, 0.1, 0.1}).J, withAlbedo({0.9, 0.5, 0.05}).J);
    // Irradiance is converted once: the same rho_s E gives the same J.
    const Rgb l1 = SourceRadianceFromIrradiance({2.0, 3.0, 4.0}, {0.5, 0.5, 0.5});
    const Rgb l2 = SourceRadianceFromIrradiance({4.0, 6.0, 8.0}, {0.25, 0.25, 0.25});
    const auto j1 = EstimateSectors(CapField(Open(), {{n, 50.0 * kDegree, Hit(l1)}}), Frame(), n, {});
    const auto j2 = EstimateSectors(CapField(Open(), {{n, 50.0 * kDegree, Hit(l2)}}), Frame(), n, {});
    EXPECT_LT(MaxRel(j1.J, j2.J), 1.0e-15);
    // The receiver albedo acts once, at composition.
    const Rgb rho{0.3, 0.6, 0.9};
    const Rgb once = ComposeIndirect(rho, j1.J);
    EXPECT_EQ(once, (Rgb{0.3 * j1.J.R, 0.6 * j1.J.G, 0.9 * j1.J.B}));
    EXPECT_EQ(ComposeIndirect(Times(rho, 2.0), j1.J), Times(once, 2.0));
    NativeCompositionTerms terms;
    terms.DirectDiffuse = {0.1, 0.1, 0.1};
    terms.AmbientDiffuse = {0.2, 0.2, 0.2};
    terms.Accessibility = 0.5;
    terms.ReceiverAlbedo = rho;
    terms.LocalJ = j1.J;
    const Rgb composed = ComposeNative(terms);
    terms.ReceiverAlbedo = Times(rho, 2.0);
    const Rgb doubled = ComposeNative(terms);
    EXPECT_NEAR(doubled.R - composed.R, once.R, 1.0e-15);
    EXPECT_NEAR(composed.G, 0.1 + 0.5 * 0.2 + once.G, 1.0e-15);

    // Raw accessibility is not clipped per section; the final A is.
    const auto c = EvaluateSection(CapField(Open()), Frame(), Tilted(90.0 * kDegree, 0.0), 0.0, 32, {});
    SectionAccumulator single;
    single.Add(c);
    EXPECT_GT(single.Result().ARaw, 1.0);
    EXPECT_EQ(single.Result().A, 1.0);
    // Unobservable directions are optimistic for A and lower C only.
    const CapField unseen(Open(), {{Frame().V, 30.0 * kDegree, FieldSample{DirectionState::Unobservable, {}, {}}}});
    const auto withUnseen = EstimateSectors(unseen, Frame(), n, {});
    EXPECT_LT(withUnseen.Confidence, 1.0);
    EXPECT_NEAR(withUnseen.ARaw, EstimateSectors(CapField(Open()), Frame(), n, {}).ARaw, 1.0e-15);
}

TEST(DiffuseGiReference, T04HdrScalesLinearlyInFp32) {
    const Vec3 n = Tilted(25.0 * kDegree);
    const Vec3 side = Normalize(Cross(n, Frame().V));
    const Vec3 sunAxis = Normalize(Add(Scale(n, std::cos(40.0 * kDegree)), Scale(side, std::sin(40.0 * kDegree))));
    const CapField base(Hit({0.02, 0.015, 0.01}), {{sunAxis, 5.0 * kDegree, Hit({5.0e4, 4.0e4, 3.0e4})},
                                                   {n, 0.5 * kPi, Hit({0.2, 0.3, 0.6})}});
    SectorEstimatorSettings settings;
    settings.Sections = 32;
    const auto reference = EstimateSectors(base, Frame(), n, settings);
    const auto unit = EstimateSectorsF32(base, Frame(), n, settings);
    ASSERT_TRUE(Finite(unit));
    EXPECT_GT(unit.J.R, 1.0); // HDR passes through: no clamp
    const double fp32VsDouble = MaxRel(unit.J, reference.J);
    double worstRatio = 0.0;
    for (double factor : {2.0, 8.0, 3.0, 1000.0}) {
        const auto scaled = EstimateSectorsF32(base.Scaled(factor), Frame(), n, settings);
        ASSERT_TRUE(Finite(scaled));
        worstRatio = std::max(worstRatio, MaxRel(scaled.J, Times(unit.J, factor)));
    }
    double worstExtreme = 0.0;
    for (double magnitude : {1.0e30, 1.0e-30}) {
        const auto low = EstimateSectorsF32(base.Scaled(magnitude), Frame(), n, settings);
        const auto high = EstimateSectorsF32(base.Scaled(8.0 * magnitude), Frame(), n, settings);
        ASSERT_TRUE(Finite(low) && Finite(high));
        worstExtreme = std::max(worstExtreme, MaxRel(high.J, Times(low.J, 8.0)));
    }
    std::printf("DGI_REPORT T04 fp32_vs_f64_rel=%.2e worst_ratio_rel_x2_x8_x3_x1000=%.2e "
                "worst_ratio_rel_extreme_1e30_1e-30=%.2e J_raw=(%.4g,%.4g,%.4g)\n",
                fp32VsDouble, worstRatio, worstExtreme, unit.J.R, unit.J.G, unit.J.B);
    EXPECT_LT(fp32VsDouble, 1.0e-4);
    EXPECT_LT(worstRatio, 0.01);
    EXPECT_LT(worstExtreme, 0.01);
}

TEST(DiffuseGiReference, InputDeclarationRejectsUndeclaredConventions) {
    InputDeclaration d;
    EXPECT_FALSE(ValidateInputDeclaration(d).empty());
    d.Depth = DepthEncoding::PerspectiveZeroToOne;
    d.Forward = ViewForward::NegativeZ;
    d.GeometricNormal = VectorSpace::View;
    d.Motion = MotionDirection::CurrentToPrevious;
    d.MotionUnit = MotionUnits::NormalizedViewportUv;
    d.Jitter = JitterInMotion::Included;
    d.Source = RadianceProfile::LdrApproximation;
    d.SourceEncoding = ColorEncoding::Srgb;
    d.SourceIsOutgoingRadiance = true;
    d.ReceiverAlbedoEncoding = ColorEncoding::Srgb;
    d.Exposure = ExposurePolicy::None;
    d.SkyDistinguishable = true;
    d.UnavailableDistinguishable = true;
    EXPECT_TRUE(ValidateInputDeclaration(d).empty()) << ValidateInputDeclaration(d);
    EXPECT_FALSE(IsCalibratedRadiance(d)); // the LDR profile is never calibrated radiance
    d.Source = RadianceProfile::LinearHdr;
    EXPECT_FALSE(ValidateInputDeclaration(d).empty()); // HDR must be linear
    d.SourceEncoding = ColorEncoding::Linear;
    EXPECT_TRUE(IsCalibratedRadiance(d));
    d.SourceIsOutgoingRadiance = false;
    EXPECT_FALSE(ValidateInputDeclaration(d).empty());
    d.SourceIsOutgoingRadiance = true;
    d.GeometricNormal = VectorSpace::World;
    EXPECT_FALSE(ValidateInputDeclaration(d).empty());
}
