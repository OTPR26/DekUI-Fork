#pragma once

// Diffuse screen-space GI, milestone M4: optional guiding of the section
// angle phi per screen region (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M4";
// specification section 6, formula p(phi) = (1 - eta) / pi + eta g(phi) with
// integral_0^pi g = 1, 0 <= eta < 1). Own construction; no path-guiding,
// reservoir or proprietary distribution is used.
//
// - Regions: square tiles of 2^RegionLog2 viewport pixels anchored at the
//   viewport origin. Each holds g as a piecewise-constant density over B
//   equal bins of [0, pi) (bin width Delta = pi / B).
// - Draw: u = (s + rotation) / S (the stratified or deterministic rotation of
//   M1-M3) is mapped through the inverse of the cumulative P of p, bin by bin:
//   phi = (b + (u - P_b) / (P_(b+1) - P_b)) Delta for P_b <= u < P_(b+1).
//   The density of that map is p_b = (P_(b+1) - P_b) / Delta, and exactly this
//   p_b weights the sample: J = (1/S) sum_s F(phi_s) / p_b(phi_s); A uses the
//   same correction (its own FA(phi_s) / p_b). A uniform table reproduces the
//   uniform estimator in distribution. The guide only chooses phi; the
//   visibility mask inside the section is unchanged.
// - Frozen: the table of a frame never changes while the frame samples. After
//   the estimates of the frame are complete, Update() reads one record per
//   pixel and section (luminance of F, the p_b of the draw, the bin) and
//   writes the next state into separate storage: per region and bin the
//   estimate e_b = (1 / n) sum_(samples in b) lum(F) / p_b of integral_b lum(F),
//   n the region's samples (unbiased for any frozen p > 0), blended into the
//   region's memory E_b <- (1 - beta) E_b + beta e_b (E = e on the first data),
//   then g_b = E_b / (Delta sum E). Initialization, a zero or non-finite sum, a
//   negative or non-finite record, Invalidate() (scene change: camera cut,
//   view change, resize, transport change, explicit reset) return a region or
//   the whole guide to the uniform distribution.
// - The guiding state is a distribution over phi: it is not a history of light
//   energy and it never enters J or A except through p_b.

#include "fast/renderer/diffuse_gi_reference.h"

#include <cstdint>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kNoGuideBin = 0xFFFFFFFFU;
inline constexpr uint32_t kMaxGuideBins = 32;
inline constexpr uint32_t kMaxGuideRegionLog2 = 6;

struct GuidingSettings {
    uint32_t RegionLog2 = 2; // 4 x 4 pixels; chosen by GuidingParameterStudy (document)
    uint32_t Bins = 8;
    double Mix = 0.25;       // eta in [0, 1)
    double Rate = 0.25;      // beta in (0, 1]: memory of the region estimates
};

[[nodiscard]] const char* ValidateGuidingSettings(const GuidingSettings& settings) noexcept;

// Frozen distribution of one frame.
struct GuideTable {
    uint32_t Width = 0, Height = 0; // viewport
    uint32_t RegionLog2 = 3, RegionsX = 0, RegionsY = 0, Bins = 8;
    std::vector<double> Cdf; // regions x (Bins + 1): P at the bin edges, P_0 = 0, P_B = 1
    std::vector<double> Pdf; // regions x Bins: (P_(b+1) - P_b) / Delta, the density of the draw

    [[nodiscard]] uint32_t Region(uint32_t x, uint32_t y) const noexcept {
        return (y >> RegionLog2) * RegionsX + (x >> RegionLog2);
    }
};

// One section of one pixel (records of non-receivers have Bin = kNoGuideBin).
struct GuideSample {
    double Contribution = 0.0; // luminance of F(phi) (Rec. 709 weights), before 1 / p
    double Pdf = 0.0;          // p_b of the draw
    double Phi = 0.0;
    uint32_t Bin = kNoGuideBin;
};

// Inverse-cumulative draw (double reference and FP32 twin share the operation
// order with the GLSL): returns phi, sets the density and the bin.
template <typename T>
inline T GuideDraw(const GuideTable& table, uint32_t region, T u, T& pdf, uint32_t& bin) noexcept {
    const double* cdf = &table.Cdf[static_cast<size_t>(region) * (table.Bins + 1U)];
    uint32_t b = 0;
    while (b + 1U < table.Bins && !(u < static_cast<T>(cdf[b + 1U]))) ++b;
    const T lo = static_cast<T>(cdf[b]);
    const T hi = static_cast<T>(cdf[b + 1U]);
    T fraction = (u - lo) / (hi - lo);
    fraction = fraction < T(0) ? T(0) : (fraction > T(1) ? T(1) : fraction);
    pdf = static_cast<T>(table.Pdf[static_cast<size_t>(region) * table.Bins + b]);
    bin = b;
    return (static_cast<T>(b) + fraction) * (T(3.14159265358979323846) / static_cast<T>(table.Bins));
}

struct GuideUpdateReport {
    uint32_t Regions = 0;
    uint32_t Updated = 0;        // regions with samples this frame
    uint32_t ResetNonFinite = 0; // regions reset by a negative or non-finite record
    uint32_t ResetZero = 0;      // regions whose memory sums to zero (uniform)
    uint64_t Samples = 0;
};

// Persistent guiding state of one view. Float32 runs the update with float
// arithmetic (twin of the GLSL update kernel).
class GuideField {
  public:
    GuideField() = default;
    GuideField(uint32_t width, uint32_t height, const GuidingSettings& settings, bool float32 = false);
    // Every region uniform, memory cleared (initialization and scene change).
    void Invalidate();
    // The frozen table of the current frame.
    [[nodiscard]] const GuideTable& Frozen() const noexcept { return mTable; }
    // After the frame: samples of the whole viewport (pixel-major, `sections`
    // per pixel), drawn from Frozen(). Writes the next state, then swaps.
    GuideUpdateReport Update(const std::vector<GuideSample>& samples, uint32_t sections);
    [[nodiscard]] const std::vector<double>& Memory() const noexcept { return mMemory; } // E, regions x bins
    [[nodiscard]] const std::vector<uint8_t>& Valid() const noexcept { return mValid; }  // per region
    [[nodiscard]] const GuidingSettings& Settings() const noexcept { return mSettings; }

  private:
    void BuildTable();
    uint32_t mWidth = 0, mHeight = 0;
    GuidingSettings mSettings;
    bool mFloat32 = false;
    std::vector<double> mMemory;
    std::vector<uint8_t> mValid;
    GuideTable mTable;
};

// A uniform table for a viewport (every p_b = 1 / pi up to rounding).
[[nodiscard]] GuideTable UniformGuideTable(uint32_t width, uint32_t height, const GuidingSettings& settings);
// A table from explicit per-bin densities g (each region the same), for tests.
[[nodiscard]] GuideTable MakeGuideTable(uint32_t width, uint32_t height, const GuidingSettings& settings,
                                        const std::vector<double>& density);

} // namespace Fast::Renderer::DiffuseGi
