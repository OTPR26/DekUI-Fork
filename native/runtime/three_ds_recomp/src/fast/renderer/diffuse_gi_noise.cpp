#include "fast/renderer/diffuse_gi_noise.h"

// Spatiotemporal blue-noise mask generator (milestone M4). Own
// implementation of the void-and-cluster ordering as described in [B4]
// section 3.1, with the energy of [B4] eq. 4 (scalar spatiotemporal masks).
// No code, mask or texture of [B4]'s SDK or of any other implementation was
// consulted. See diffuse_gi_noise.h and docs/TRIAEVUM_SCREEN_SPACE_GI.md ("M4").

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <numeric>

namespace Fast::Renderer::DiffuseGi {
namespace {

struct SplitMix64 {
    uint64_t State;
    uint64_t Next() {
        uint64_t z = (State += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    // Uniform integer in [0, n) by rejection (no modulo bias).
    uint64_t Below(uint64_t n) {
        const uint64_t limit = std::numeric_limits<uint64_t>::max() - std::numeric_limits<uint64_t>::max() % n;
        uint64_t value;
        do value = Next();
        while (value >= limit);
        return value % n;
    }
};

// Energy field of a set of "on" voxels: E(p) = sum_q E(p, q) over on voxels q
// (q = p included: a constant that changes no argmax among on voxels and no
// argmin among off voxels). Toggling a voxel updates its slice and its pixel.
class EnergyField {
  public:
    EnergyField(uint32_t w, uint32_t h, uint32_t d, double sigma, bool spatiotemporal)
        : mW(w), mH(h), mD(d), mSpatiotemporal(spatiotemporal) {
        mSlice.resize(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const double dx = std::min(x, w - x), dy = std::min(y, h - y);
                mSlice[static_cast<size_t>(y) * w + x] = std::exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma));
            }
        mTime.resize(d);
        for (uint32_t z = 0; z < d; ++z) {
            const double dz = std::min(z, d - z);
            mTime[z] = std::exp(-(dz * dz) / (2.0 * sigma * sigma));
        }
        mEnergy.assign(static_cast<size_t>(w) * h * d, 0.0);
    }
    void Toggle(size_t index, double sign) {
        const uint32_t x = static_cast<uint32_t>(index % mW);
        const uint32_t y = static_cast<uint32_t>((index / mW) % mH);
        const uint32_t z = static_cast<uint32_t>(index / (static_cast<size_t>(mW) * mH));
        const size_t base = static_cast<size_t>(z) * mW * mH;
        for (uint32_t yy = 0; yy < mH; ++yy) {
            const uint32_t ky = (yy + mH - y) % mH;
            for (uint32_t xx = 0; xx < mW; ++xx) {
                const uint32_t kx = (xx + mW - x) % mW;
                mEnergy[base + static_cast<size_t>(yy) * mW + xx] += sign * mSlice[static_cast<size_t>(ky) * mW + kx];
            }
        }
        if (!mSpatiotemporal) return;
        for (uint32_t zz = 0; zz < mD; ++zz) {
            if (zz == z) continue; // the voxel itself is in its slice already
            const uint32_t kz = (zz + mD - z) % mD;
            mEnergy[static_cast<size_t>(zz) * mW * mH + static_cast<size_t>(y) * mW + x] += sign * mTime[kz];
        }
    }
    [[nodiscard]] double operator[](size_t i) const { return mEnergy[i]; }
    [[nodiscard]] size_t Size() const { return mEnergy.size(); }

  private:
    uint32_t mW, mH, mD;
    bool mSpatiotemporal;
    std::vector<double> mSlice, mTime, mEnergy;
};

// Tightest cluster: the on voxel of largest energy (first index on ties);
// largest void: the off voxel of smallest energy. `first`/`end` restrict the
// search (one slice for independent 2D masks).
size_t Tightest(const EnergyField& e, const std::vector<uint8_t>& on, size_t first, size_t end) {
    size_t best = end;
    for (size_t i = first; i < end; ++i)
        if (on[i] != 0U && (best == end || e[i] > e[best])) best = i;
    return best;
}
size_t Largest(const EnergyField& e, const std::vector<uint8_t>& on, size_t first, size_t end) {
    size_t best = end;
    for (size_t i = first; i < end; ++i)
        if (on[i] == 0U && (best == end || e[i] < e[best])) best = i;
    return best;
}

// Void and cluster over the voxels [first, end) ([B4] section 3.1): phase 0
// makes the initial pattern blue by swapping the tightest cluster into the
// largest void until they coincide; phase 1 ranks the initial pattern by
// removing tightest clusters; phase 2 adds largest voids up to half; phase 3
// ranks the rest by removing the tightest clusters of the remaining zeros.
// Ranks are written as `rankBase + order`.
void VoidAndCluster(EnergyField& e, std::vector<uint8_t>& on, size_t first, size_t end, uint32_t rankBase,
                    std::vector<uint32_t>& rank) {
    const size_t count = end - first;
    // Phase 0.
    for (size_t iteration = 0; iteration < 4U * count; ++iteration) {
        const size_t cluster = Tightest(e, on, first, end);
        on[cluster] = 0U;
        e.Toggle(cluster, -1.0);
        const size_t hole = Largest(e, on, first, end);
        on[hole] = 1U;
        e.Toggle(hole, 1.0);
        if (hole == cluster) break;
    }
    const std::vector<uint8_t> initial(on.begin() + static_cast<std::ptrdiff_t>(first),
                                       on.begin() + static_cast<std::ptrdiff_t>(end));
    size_t ones = 0;
    for (size_t i = first; i < end; ++i) ones += on[i];
    // Phase 1: rank the initial pattern (on a copy of the field).
    {
        EnergyField copy = e;
        std::vector<uint8_t> pattern = on;
        for (size_t remaining = ones; remaining > 0; --remaining) {
            const size_t cluster = Tightest(copy, pattern, first, end);
            pattern[cluster] = 0U;
            copy.Toggle(cluster, -1.0);
            rank[cluster] = rankBase + static_cast<uint32_t>(remaining - 1U);
        }
    }
    // Phase 2: insert the largest voids up to half.
    size_t next = ones;
    while (next < (count + 1U) / 2U) {
        const size_t hole = Largest(e, on, first, end);
        on[hole] = 1U;
        e.Toggle(hole, 1.0);
        rank[hole] = rankBase + static_cast<uint32_t>(next++);
    }
    // Phase 3: the zeros are the minority; rank them by removing their
    // tightest clusters (energy of the zeros = total kernel sum - energy of
    // the ones, so the tightest zero is the off voxel of least ones-energy).
    while (next < count) {
        const size_t hole = Largest(e, on, first, end);
        on[hole] = 1U;
        e.Toggle(hole, 1.0);
        rank[hole] = rankBase + static_cast<uint32_t>(next++);
    }
    (void)initial;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int k = 0; k < 8; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
}
void PutF64(std::vector<uint8_t>& out, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    PutU64(out, bits);
}
uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8U | static_cast<uint32_t>(p[2]) << 16U |
           static_cast<uint32_t>(p[3]) << 24U;
}
uint64_t GetU64(const uint8_t* p) { return static_cast<uint64_t>(GetU32(p)) | static_cast<uint64_t>(GetU32(p + 4)) << 32U; }
double GetF64(const uint8_t* p) {
    const uint64_t bits = GetU64(p);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
}

constexpr char kMagic[8] = {'D', 'G', 'I', 'B', 'N', 'M', '0', '1'};
constexpr size_t kHeader = 8 + 16 + 24;

// Mean power of the lowest / highest frequencies relative to all non-zero
// frequencies, of a DFT over `n` points (1D) per line.
void Dft(const std::vector<double>& in, std::vector<std::complex<double>>& out) {
    const size_t n = in.size();
    out.assign(n, {});
    for (size_t k = 0; k < n; ++k) {
        std::complex<double> sum = 0.0;
        for (size_t t = 0; t < n; ++t) sum += in[t] * std::polar(1.0, -2.0 * 3.14159265358979323846 * k * t / n);
        out[k] = sum;
    }
}

} // namespace

const char* ValidateNoiseMaskSettings(const NoiseMaskSettings& s) noexcept {
    if (s.Width == 0U || s.Height == 0U || s.Depth == 0U || s.Width > 256U || s.Height > 256U || s.Depth > 256U)
        return "mask extents outside [1, 256]";
    if (!(s.Sigma > 0.0) || !std::isfinite(s.Sigma)) return "energy sigma must be finite and positive";
    if (!(s.InitialDensity > 0.0 && s.InitialDensity < 0.5)) return "initial density outside (0, 0.5)";
    return "";
}

NoiseMask GenerateBlueNoiseMask(const NoiseMaskSettings& s) {
    NoiseMask mask;
    mask.Width = s.Width;
    mask.Height = s.Height;
    mask.Depth = s.Depth;
    mask.Settings = s;
    const size_t slice = static_cast<size_t>(s.Width) * s.Height;
    const size_t count = slice * s.Depth;
    mask.Rank.assign(count, 0U);
    EnergyField energy(s.Width, s.Height, s.Depth, s.Sigma, s.Spatiotemporal);
    std::vector<uint8_t> on(count, 0U);
    SplitMix64 random{s.Seed};
    const auto seed = [&](size_t first, size_t end) {
        const size_t n = end - first;
        const size_t ones = std::max<size_t>(1U, static_cast<size_t>(std::floor(s.InitialDensity * n)));
        for (size_t placed = 0; placed < ones;) {
            const size_t i = first + static_cast<size_t>(random.Below(n));
            if (on[i] != 0U) continue;
            on[i] = 1U;
            energy.Toggle(i, 1.0);
            ++placed;
        }
    };
    if (s.Spatiotemporal) {
        seed(0, count);
        VoidAndCluster(energy, on, 0, count, 0U, mask.Rank);
    } else {
        // Independent 2D masks: each slice ranked on its own; global ranks
        // interleave the slices so that every slice's values stay uniform.
        std::vector<uint32_t> local(count, 0U);
        for (uint32_t z = 0; z < s.Depth; ++z) {
            seed(z * slice, (z + 1U) * slice);
            VoidAndCluster(energy, on, z * slice, (z + 1U) * slice, 0U, local);
        }
        for (size_t i = 0; i < count; ++i)
            mask.Rank[i] = local[i] * s.Depth + static_cast<uint32_t>(i / slice);
    }
    return mask;
}

NoiseMask GenerateWhiteNoiseMask(const NoiseMaskSettings& s) {
    NoiseMask mask;
    mask.Width = s.Width;
    mask.Height = s.Height;
    mask.Depth = s.Depth;
    mask.Settings = s;
    const size_t count = static_cast<size_t>(s.Width) * s.Height * s.Depth;
    mask.Rank.resize(count);
    std::iota(mask.Rank.begin(), mask.Rank.end(), 0U);
    SplitMix64 random{s.Seed ^ 0xA5A5A5A5A5A5A5A5ULL};
    for (size_t i = count; i > 1; --i) std::swap(mask.Rank[i - 1], mask.Rank[static_cast<size_t>(random.Below(i))]);
    return mask;
}

uint64_t Fnv1a64(const uint8_t* data, size_t size) noexcept {
    uint64_t hash = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

std::vector<uint8_t> SerializeNoiseMask(const NoiseMask& mask) {
    std::vector<uint8_t> out(kMagic, kMagic + 8);
    PutU32(out, mask.Width);
    PutU32(out, mask.Height);
    PutU32(out, mask.Depth);
    PutU32(out, mask.Settings.Spatiotemporal ? 1U : 0U);
    PutF64(out, mask.Settings.Sigma);
    PutF64(out, mask.Settings.InitialDensity);
    PutU64(out, mask.Settings.Seed);
    for (const uint32_t r : mask.Rank) PutU32(out, r);
    PutU64(out, Fnv1a64(out.data(), out.size()));
    return out;
}

std::string ParseNoiseMask(const std::vector<uint8_t>& bytes, NoiseMask& mask) {
    if (bytes.size() < kHeader + 8U) return "file too short";
    if (std::memcmp(bytes.data(), kMagic, 8) != 0) return "wrong magic";
    const uint32_t w = GetU32(&bytes[8]), h = GetU32(&bytes[12]), d = GetU32(&bytes[16]);
    const uint32_t flags = GetU32(&bytes[20]);
    if (w == 0U || h == 0U || d == 0U || w > 256U || h > 256U || d > 256U) return "extents outside [1, 256]";
    if ((flags & ~1U) != 0U) return "unknown flags";
    const size_t count = static_cast<size_t>(w) * h * d;
    if (bytes.size() != kHeader + 4U * count + 8U) return "size does not match the extents";
    if (GetU64(&bytes[bytes.size() - 8U]) != Fnv1a64(bytes.data(), bytes.size() - 8U)) return "checksum mismatch";
    NoiseMask parsed;
    parsed.Width = w, parsed.Height = h, parsed.Depth = d;
    parsed.Settings.Width = w, parsed.Settings.Height = h, parsed.Settings.Depth = d;
    parsed.Settings.Spatiotemporal = (flags & 1U) != 0U;
    parsed.Settings.Sigma = GetF64(&bytes[24]);
    parsed.Settings.InitialDensity = GetF64(&bytes[32]);
    parsed.Settings.Seed = GetU64(&bytes[40]);
    parsed.Rank.resize(count);
    std::vector<uint8_t> seen(count, 0U);
    for (size_t i = 0; i < count; ++i) {
        const uint32_t r = GetU32(&bytes[kHeader + 4U * i]);
        if (r >= count || seen[r] != 0U) return "ranks are not a permutation";
        seen[r] = 1U;
        parsed.Rank[i] = r;
    }
    mask = std::move(parsed);
    return "";
}

NoiseSpectrum AnalyzeNoiseMask(const NoiseMask& mask) {
    NoiseSpectrum out;
    const uint32_t w = mask.Width, h = mask.Height, d = mask.Depth;
    const double n = static_cast<double>(mask.Count());
    const auto value = [&](uint32_t x, uint32_t y, uint32_t z) { return (mask.At(x, y, z) + 0.5) / n - 0.5; };
    // Spatial: 2D DFT per slice by rows then columns.
    std::vector<double> power(static_cast<size_t>(w) * h, 0.0);
    std::vector<std::complex<double>> rows(static_cast<size_t>(w) * h);
    std::vector<double> line;
    std::vector<std::complex<double>> spectrum;
    double uniformity = 0.0, neighbour = 0.0;
    for (uint32_t z = 0; z < d; ++z) {
        for (uint32_t y = 0; y < h; ++y) {
            line.assign(w, 0.0);
            for (uint32_t x = 0; x < w; ++x) line[x] = value(x, y, z);
            Dft(line, spectrum);
            for (uint32_t k = 0; k < w; ++k) rows[static_cast<size_t>(y) * w + k] = spectrum[k];
        }
        for (uint32_t k = 0; k < w; ++k) {
            std::vector<std::complex<double>> column(h);
            for (uint32_t ky = 0; ky < h; ++ky) {
                std::complex<double> sum = 0.0;
                for (uint32_t y = 0; y < h; ++y)
                    sum += rows[static_cast<size_t>(y) * w + k] * std::polar(1.0, -2.0 * 3.14159265358979323846 * ky * y / h);
                column[ky] = sum;
            }
            for (uint32_t ky = 0; ky < h; ++ky) power[static_cast<size_t>(ky) * w + k] += std::norm(column[ky]);
        }
        // Slice uniformity (Kolmogorov distance) and lag-1 correlation.
        std::vector<double> values;
        double c = 0.0, v = 0.0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const double a = value(x, y, z);
                values.push_back(a + 0.5);
                c += a * value((x + 1U) % w, y, z) + a * value(x, (y + 1U) % h, z);
                v += 2.0 * a * a;
            }
        std::sort(values.begin(), values.end());
        for (size_t i = 0; i < values.size(); ++i)
            uniformity = std::max({uniformity, std::abs(values[i] - static_cast<double>(i) / values.size()),
                                   std::abs(values[i] - static_cast<double>(i + 1) / values.size())});
        neighbour += c / v;
    }
    out.SliceUniformity = uniformity;
    out.NeighbourCorrelation = neighbour / d;
    double all = 0.0, low = 0.0, high = 0.0;
    size_t allCount = 0, lowCount = 0, highCount = 0;
    for (uint32_t ky = 0; ky < h; ++ky)
        for (uint32_t kx = 0; kx < w; ++kx) {
            if (kx == 0U && ky == 0U) continue;
            const double fx = std::min(kx, w - kx) / static_cast<double>(w);
            const double fy = std::min(ky, h - ky) / static_cast<double>(h);
            const double f = std::sqrt(fx * fx + fy * fy);
            const double p = power[static_cast<size_t>(ky) * w + kx];
            all += p, ++allCount;
            if (f <= 0.125) low += p, ++lowCount;
            if (f >= 0.375) high += p, ++highCount;
        }
    const double mean = all / std::max<size_t>(allCount, 1U);
    out.SpatialLow = lowCount > 0U ? (low / lowCount) / mean : 0.0;
    out.SpatialHigh = highCount > 0U ? (high / highCount) / mean : 0.0;
    // Temporal: 1D DFT per pixel.
    std::vector<double> temporal(d, 0.0);
    double gaps = 0.0, tc = 0.0, tv = 0.0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            line.assign(d, 0.0);
            for (uint32_t z = 0; z < d; ++z) line[z] = value(x, y, z);
            Dft(line, spectrum);
            for (uint32_t k = 0; k < d; ++k) temporal[k] += std::norm(spectrum[k]);
            std::vector<double> sorted(line.begin(), line.end());
            std::sort(sorted.begin(), sorted.end());
            double gap = sorted.front() + 0.5 + (0.5 - sorted.back()); // wrap-around gap
            for (size_t i = 1; i < sorted.size(); ++i) gap = std::max(gap, sorted[i] - sorted[i - 1]);
            gaps += gap;
            for (uint32_t z = 0; z < d; ++z) {
                tc += line[z] * line[(z + 1U) % d];
                tv += line[z] * line[z];
            }
        }
    out.ColumnCoverage = gaps / (static_cast<double>(w) * h);
    out.TemporalCorrelation = tv > 0.0 ? tc / tv : 0.0;
    double tAll = 0.0, tLow = 0.0, tHigh = 0.0;
    size_t tAllCount = 0, tLowCount = 0, tHighCount = 0;
    for (uint32_t k = 1; k < d; ++k) {
        const double f = std::min(k, d - k) / static_cast<double>(d);
        tAll += temporal[k], ++tAllCount;
        if (f <= 0.125) tLow += temporal[k], ++tLowCount;
        if (f >= 0.375) tHigh += temporal[k], ++tHighCount;
    }
    const double tMean = tAll / std::max<size_t>(tAllCount, 1U);
    out.TemporalLow = tLowCount > 0U ? (tLow / tLowCount) / tMean : 0.0;
    out.TemporalHigh = tHighCount > 0U ? (tHigh / tHighCount) / tMean : 0.0;
    return out;
}

} // namespace Fast::Renderer::DiffuseGi
