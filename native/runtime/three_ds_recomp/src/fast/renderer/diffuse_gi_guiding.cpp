#include "fast/renderer/diffuse_gi_guiding.h"

// Guiding of the section angle (milestone M4). Own code from the
// specification (section 6, p(phi) = (1 - eta) / pi + eta g(phi)) and the
// derivations of docs/TRIAEVUM_SCREEN_SPACE_GI.md ("M4"). The update runs in
// double (reference) or float (twin of the GLSL update kernel).

#include <algorithm>
#include <cmath>

namespace Fast::Renderer::DiffuseGi {
namespace {

uint32_t RegionsAlong(uint32_t extent, uint32_t log2) { return (extent + (1U << log2) - 1U) >> log2; }

// Table of one region from its memory E (bins values). Bin probabilities
// q_b = (1 - eta) / B + eta E_b / sum E (the integral of p over the bin, with
// g = E / (Delta sum E)); P_(b+1) = P_b + q_b with P_B set to 1 (no division by
// a rounded total, no pi round trip: probabilities that are exact in binary,
// such as (1 - eta) / B, give exact edges in every precision, so an edge that
// coincides with a deterministic u is resolved alike by all twins). The stored
// density is (P_(b+1) - P_b) / Delta, the density of the draw itself.
template <typename T>
void RegionTable(const double* memory, bool valid, uint32_t bins, double mix, double* cdf, double* pdf) {
    const T pi = T(3.14159265358979323846);
    const T delta = pi / static_cast<T>(bins);
    T total = T(0);
    if (valid)
        for (uint32_t b = 0; b < bins; ++b) total += static_cast<T>(memory[b]);
    const bool uniform = !valid || !(total > T(0)) || !std::isfinite(total);
    const T eta = static_cast<T>(mix);
    const T base = (T(1) - eta) / static_cast<T>(bins);
    T running = T(0);
    cdf[0] = 0.0;
    for (uint32_t b = 0; b < bins; ++b) {
        const T share = uniform ? T(1) / static_cast<T>(bins) : static_cast<T>(memory[b]) / total;
        running += base + eta * share;
        cdf[b + 1U] = static_cast<double>(b + 1U == bins ? T(1) : std::min(running, T(1)));
    }
    for (uint32_t b = 0; b < bins; ++b)
        pdf[b] = static_cast<double>((static_cast<T>(cdf[b + 1U]) - static_cast<T>(cdf[b])) / delta);
}

} // namespace

const char* ValidateGuidingSettings(const GuidingSettings& s) noexcept {
    if (s.RegionLog2 > kMaxGuideRegionLog2) return "guide region larger than 64 x 64";
    if (s.Bins == 0U || s.Bins > kMaxGuideBins) return "guide bins outside [1, 32]";
    if (!(s.Mix >= 0.0 && s.Mix < 1.0)) return "guide mix eta outside [0, 1)";
    if (!(s.Rate > 0.0 && s.Rate <= 1.0)) return "guide rate beta outside (0, 1]";
    return "";
}

GuideField::GuideField(uint32_t width, uint32_t height, const GuidingSettings& settings, bool float32)
    : mWidth(width), mHeight(height), mSettings(settings), mFloat32(float32) {
    Invalidate();
}

void GuideField::Invalidate() {
    const size_t regions = static_cast<size_t>(RegionsAlong(mWidth, mSettings.RegionLog2)) *
                           RegionsAlong(mHeight, mSettings.RegionLog2);
    mMemory.assign(regions * mSettings.Bins, 0.0);
    mValid.assign(regions, 0U);
    BuildTable();
}

void GuideField::BuildTable() {
    GuideTable t;
    t.Width = mWidth;
    t.Height = mHeight;
    t.RegionLog2 = mSettings.RegionLog2;
    t.RegionsX = RegionsAlong(mWidth, mSettings.RegionLog2);
    t.RegionsY = RegionsAlong(mHeight, mSettings.RegionLog2);
    t.Bins = mSettings.Bins;
    const size_t regions = static_cast<size_t>(t.RegionsX) * t.RegionsY;
    t.Cdf.assign(regions * (t.Bins + 1U), 0.0);
    t.Pdf.assign(regions * t.Bins, 0.0);
    for (size_t r = 0; r < regions; ++r) {
        if (mFloat32)
            RegionTable<float>(&mMemory[r * t.Bins], mValid[r] != 0U, t.Bins, mSettings.Mix, &t.Cdf[r * (t.Bins + 1U)],
                               &t.Pdf[r * t.Bins]);
        else
            RegionTable<double>(&mMemory[r * t.Bins], mValid[r] != 0U, t.Bins, mSettings.Mix,
                                &t.Cdf[r * (t.Bins + 1U)], &t.Pdf[r * t.Bins]);
    }
    mTable = std::move(t);
}

GuideUpdateReport GuideField::Update(const std::vector<GuideSample>& samples, uint32_t sections) {
    GuideUpdateReport report;
    const GuideTable& t = mTable;
    const size_t regions = static_cast<size_t>(t.RegionsX) * t.RegionsY;
    report.Regions = static_cast<uint32_t>(regions);
    // The next state is written to separate storage; the frozen table and the
    // current memory are only read.
    std::vector<double> memory = mMemory;
    std::vector<uint8_t> valid = mValid;
    const uint32_t size = 1U << t.RegionLog2;
    const uint32_t bins = t.Bins;
    for (uint32_t ry = 0; ry < t.RegionsY; ++ry)
        for (uint32_t rx = 0; rx < t.RegionsX; ++rx) {
            const size_t r = static_cast<size_t>(ry) * t.RegionsX + rx;
            double sumsD[kMaxGuideBins] = {};
            float sumsF[kMaxGuideBins] = {};
            uint32_t n = 0;
            bool broken = false;
            for (uint32_t y = ry * size; y < std::min((ry + 1U) * size, mHeight); ++y)
                for (uint32_t x = rx * size; x < std::min((rx + 1U) * size, mWidth); ++x)
                    for (uint32_t s = 0; s < sections; ++s) {
                        const GuideSample& sample = samples[(static_cast<size_t>(y) * mWidth + x) * sections + s];
                        if (sample.Bin == kNoGuideBin) continue;
                        ++n;
                        if (mFloat32) {
                            const float value = static_cast<float>(sample.Contribution) / static_cast<float>(sample.Pdf);
                            if (!std::isfinite(value) || value < 0.0F || sample.Bin >= bins) broken = true;
                            else sumsF[sample.Bin] += value;
                        } else {
                            const double value = sample.Contribution / sample.Pdf;
                            if (!std::isfinite(value) || value < 0.0 || sample.Bin >= bins) broken = true;
                            else sumsD[sample.Bin] += value;
                        }
                    }
            report.Samples += n;
            if (broken) {
                ++report.ResetNonFinite;
                std::fill(&memory[r * bins], &memory[r * bins] + bins, 0.0);
                valid[r] = 0U;
                continue;
            }
            if (n == 0U) continue; // no receiver this frame: the memory is kept
            ++report.Updated;
            const double beta = mSettings.Rate;
            double total = 0.0;
            for (uint32_t b = 0; b < bins; ++b) {
                double estimate;
                if (mFloat32) {
                    const float e = sumsF[b] / static_cast<float>(n);
                    estimate = valid[r] != 0U ? static_cast<double>((1.0F - static_cast<float>(beta)) *
                                                                        static_cast<float>(memory[r * bins + b]) +
                                                                    static_cast<float>(beta) * e)
                                              : static_cast<double>(e);
                } else {
                    const double e = sumsD[b] / static_cast<double>(n);
                    estimate = valid[r] != 0U ? (1.0 - beta) * memory[r * bins + b] + beta * e : e;
                }
                memory[r * bins + b] = estimate;
                total += estimate;
            }
            if (!(total > 0.0) || !std::isfinite(total)) {
                ++report.ResetZero;
                std::fill(&memory[r * bins], &memory[r * bins] + bins, 0.0);
                valid[r] = 0U;
                continue;
            }
            valid[r] = 1U;
        }
    mMemory.swap(memory);
    mValid.swap(valid);
    BuildTable();
    return report;
}

GuideTable UniformGuideTable(uint32_t width, uint32_t height, const GuidingSettings& settings) {
    GuideField field(width, height, settings);
    return field.Frozen();
}

GuideTable MakeGuideTable(uint32_t width, uint32_t height, const GuidingSettings& settings,
                          const std::vector<double>& density) {
    GuideTable t = UniformGuideTable(width, height, settings);
    const size_t regions = static_cast<size_t>(t.RegionsX) * t.RegionsY;
    for (size_t r = 0; r < regions; ++r)
        RegionTable<double>(density.data(), true, t.Bins, settings.Mix, &t.Cdf[r * (t.Bins + 1U)], &t.Pdf[r * t.Bins]);
    return t;
}

} // namespace Fast::Renderer::DiffuseGi
