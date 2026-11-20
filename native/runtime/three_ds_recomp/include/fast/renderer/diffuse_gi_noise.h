#pragma once

// Diffuse screen-space GI, milestone M4: own generator of spatiotemporal
// blue-noise masks (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M4"; specification
// section 6 "Rumore" and section 12).
//
// Construction from the description in [B4] (Wolfe, Morrical,
// Akenine-Moller, Ramamoorthi, EGSR 2022): the void-and-cluster ordering of
// [B4] section 3.1 (initial pattern, tightest cluster / largest void swaps,
// three ranking phases) driven by the spatiotemporal energy of [B4] eq. 4,
//   E(p, q) = exp(-|p - q|^2 / (2 sigma^2)) if p and q share the slice or the
//   pixel (toroidal distances), 0 otherwise.
// Own code: no mask, texture or code of [B4]'s SDK or of any other source was
// read or imported. Deterministic: the only randomness is the initial binary
// pattern from a seeded SplitMix64 generator. The mask holds one rank per
// voxel, a permutation of [0, W H D); the value of a voxel is
// (rank + 1/2) / (W H D). Spatiotemporal = false gives independent 2D masks per
// slice (energy only within the slice; each slice ranked on its own), the
// "2D blue noise" control of [B4].

#include <cstdint>
#include <string>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

struct NoiseMaskSettings {
    uint32_t Width = 32, Height = 32, Depth = 32;
    double Sigma = 1.9;          // [B4] section 4.1 value; checked by NoiseMaskStudy
    double InitialDensity = 0.1; // [B4] section 4.1
    uint64_t Seed = 0x5DEECE66DULL;
    bool Spatiotemporal = true;
};

struct NoiseMask {
    uint32_t Width = 0, Height = 0, Depth = 0;
    NoiseMaskSettings Settings;
    std::vector<uint32_t> Rank; // x + W (y + H z)
    [[nodiscard]] uint32_t At(uint32_t x, uint32_t y, uint32_t z) const noexcept {
        return Rank[(static_cast<size_t>(z % Depth) * Height + (y % Height)) * Width + (x % Width)];
    }
    [[nodiscard]] uint32_t Count() const noexcept { return Width * Height * Depth; }
};

[[nodiscard]] const char* ValidateNoiseMaskSettings(const NoiseMaskSettings& settings) noexcept;
[[nodiscard]] NoiseMask GenerateBlueNoiseMask(const NoiseMaskSettings& settings);
// Control: a uniformly random permutation from the same generator.
[[nodiscard]] NoiseMask GenerateWhiteNoiseMask(const NoiseMaskSettings& settings);

// Value of the rotation (offset = false) or of the radial jitter (offset =
// true: the same slice read at (x + W/2, y + H/2)); double divides by the
// count, float multiplies by its float reciprocal like the GLSL.
template <typename T>
[[nodiscard]] inline T NoiseValue(const NoiseMask& mask, uint32_t x, uint32_t y, uint64_t frame, bool offset) noexcept {
    const uint32_t ox = offset ? mask.Width / 2U : 0U;
    const uint32_t oy = offset ? mask.Height / 2U : 0U;
    const uint32_t rank = mask.At(x + ox, y + oy, static_cast<uint32_t>(frame % mask.Depth));
    if constexpr (sizeof(T) == sizeof(double))
        return (static_cast<double>(rank) + 0.5) / static_cast<double>(mask.Count());
    else
        return (static_cast<float>(rank) + 0.5F) * (1.0F / static_cast<float>(mask.Count()));
}

// Asset format (own, little endian): "DGIBNM01", uint32 width, height, depth,
// flags (bit 0 spatiotemporal), double sigma, double initial density, uint64
// seed, uint32 rank[W H D], uint64 FNV-1a 64 of every preceding byte.
[[nodiscard]] std::vector<uint8_t> SerializeNoiseMask(const NoiseMask& mask);
// Empty on success; rejects a wrong magic, sizes, a checksum mismatch, extents
// outside [1, 256] or ranks that are not a permutation of [0, W H D).
[[nodiscard]] std::string ParseNoiseMask(const std::vector<uint8_t>& bytes, NoiseMask& mask);
[[nodiscard]] uint64_t Fnv1a64(const uint8_t* data, size_t size) noexcept;

// Spectral checks (an even histogram does not prove blue noise): power of the
// mean-free values, spatial 2D DFT per slice (mean over slices) and temporal
// 1D DFT per pixel (mean over pixels), as the mean power of the lowest
// frequencies relative to the mean power of all non-zero frequencies
// (white noise: about 1).
struct NoiseSpectrum {
    double SpatialLow = 0.0;  // |f| <= 1/8 cycles per pixel, f != 0
    double TemporalLow = 0.0; // |f| <= 1/8 cycles per frame, f != 0
    double SpatialHigh = 0.0; // |f| >= 3/8
    double TemporalHigh = 0.0;
    double SliceUniformity = 0.0;   // max over slices of the Kolmogorov distance of the slice values to U(0,1)
    double ColumnCoverage = 0.0;    // mean over pixels of the largest gap between sorted temporal values
    double NeighbourCorrelation = 0.0; // spatial lag-1 correlation of the values (mean over slices)
    double TemporalCorrelation = 0.0;  // temporal lag-1 correlation (mean over pixels)
};
[[nodiscard]] NoiseSpectrum AnalyzeNoiseMask(const NoiseMask& mask);

} // namespace Fast::Renderer::DiffuseGi
