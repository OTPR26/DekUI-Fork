// Differential test: every inline host-float VFP fast path must return the
// same value and cumulative flags as the software implementation, in every
// FPSCR rounding/flush/default-NaN mode and with host FTZ/DAZ on or off.

#include "oot3d_native_a32_vfp_fast.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {

namespace a32 = oot3d::recomp::a32;
using Oot3dNativeGame::Oot3dVfpAdd;
using Oot3dNativeGame::Oot3dVfpFromSigned;
using Oot3dNativeGame::Oot3dVfpMultiply;
using Oot3dNativeGame::Oot3dVfpMultiplyAccumulate;
using Oot3dNativeGame::Oot3dVfpMultiplySubtract;
using Oot3dNativeGame::Oot3dVfpNegativeMultiply;
using Oot3dNativeGame::Oot3dVfpNegativeMultiplyAccumulate;
using Oot3dNativeGame::Oot3dVfpNegativeMultiplySubtract;
using Oot3dNativeGame::Oot3dVfpSubtract;

int gFailures = 0;
std::uint64_t gChecks = 0;
std::uint64_t gFastAdds = 0;
std::uint64_t gFastMultiplies = 0;

void Expect(const char* name, const a32::VfpBinary32Result& fast,
            const a32::VfpBinary32Result& reference,
            std::uint32_t a, std::uint32_t b, std::uint32_t c,
            std::uint32_t fpscr) {
    ++gChecks;
    if (fast.value == reference.value &&
        fast.exception_flags == reference.exception_flags) {
        return;
    }
    if (++gFailures <= 20) {
        std::cerr << std::hex << name << " a=" << a << " b=" << b << " c=" << c
                  << " fpscr=" << fpscr << " fast=" << fast.value << '/'
                  << fast.exception_flags << " reference=" << reference.value
                  << '/' << reference.exception_flags << std::dec << '\n';
    }
}

// Biased toward the boundaries of the fast path: exponents around 1-2 and
// 23-25, denormals, zeros, the overflow edge, infinities and NaNs.
std::uint32_t Operand(std::mt19937_64& random) {
    const std::uint32_t sign = static_cast<std::uint32_t>(random() & 1U) << 31U;
    const std::uint32_t fraction = static_cast<std::uint32_t>(random()) & 0x007FFFFFU;
    static constexpr std::array<std::uint32_t, 14> kEdgeExponents = {
        0U, 1U, 2U, 3U, 23U, 24U, 25U, 26U, 100U, 127U, 150U, 253U, 254U, 255U};
    std::uint32_t exponent = 0U;
    switch (random() % 8U) {
    case 0:
        return sign;
    case 1:
        exponent = kEdgeExponents[random() % kEdgeExponents.size()];
        break;
    case 2:
        // Short significands make exact results and cancellation frequent.
        return sign | ((static_cast<std::uint32_t>(96U + random() % 64U)) << 23U) |
               (fraction & 0x007F0000U);
    default:
        exponent = static_cast<std::uint32_t>(random() % 256U);
        break;
    }
    return sign | (exponent << 23U) | fraction;
}

void CheckOperands(std::uint32_t a, std::uint32_t b, std::uint32_t c,
                   std::uint32_t fpscr) {
    if (fpscr == 0U) {
        a32::VfpBinary32Result ignored{};
        gFastAdds += Oot3dNativeGame::VfpFastDetail::Add(a, b, ignored) ? 1U : 0U;
        gFastMultiplies +=
            Oot3dNativeGame::VfpFastDetail::Multiply(a, b, ignored) ? 1U : 0U;
    }
    Expect("add", Oot3dVfpAdd(a, b, fpscr), a32::VfpBinary32Add(a, b, fpscr), a, b, c, fpscr);
    Expect("sub", Oot3dVfpSubtract(a, b, fpscr), a32::VfpBinary32Subtract(a, b, fpscr), a, b, c, fpscr);
    Expect("mul", Oot3dVfpMultiply(a, b, fpscr), a32::VfpBinary32Multiply(a, b, fpscr), a, b, c, fpscr);
    Expect("nmul", Oot3dVfpNegativeMultiply(a, b, fpscr),
           a32::VfpBinary32NegativeMultiply(a, b, fpscr), a, b, c, fpscr);
    Expect("mla", Oot3dVfpMultiplyAccumulate(c, a, b, fpscr),
           a32::VfpBinary32MultiplyAccumulate(c, a, b, fpscr), a, b, c, fpscr);
    Expect("mls", Oot3dVfpMultiplySubtract(c, a, b, fpscr),
           a32::VfpBinary32MultiplySubtract(c, a, b, fpscr), a, b, c, fpscr);
    Expect("nmla", Oot3dVfpNegativeMultiplyAccumulate(c, a, b, fpscr),
           a32::VfpBinary32NegativeMultiplyAccumulate(c, a, b, fpscr), a, b, c, fpscr);
    Expect("nmls", Oot3dVfpNegativeMultiplySubtract(c, a, b, fpscr),
           a32::VfpBinary32NegativeMultiplySubtract(c, a, b, fpscr), a, b, c, fpscr);
    Expect("from_signed", Oot3dVfpFromSigned(a, fpscr),
           a32::VfpBinary32FromSigned(a, fpscr), a, b, c, fpscr);
}

void RunCampaign(std::uint64_t seed, int iterations) {
    // RN, RP, RM, RZ, each with and without FZ and DN.
    std::array<std::uint32_t, 16> modes{};
    for (std::uint32_t i = 0; i < modes.size(); ++i) {
        modes[i] = ((i & 3U) << 22U) | (((i >> 2U) & 1U) << 24U) |
                   (((i >> 3U) & 1U) << 25U);
    }
    std::mt19937_64 random(seed);
    for (int i = 0; i < iterations; ++i) {
        const std::uint32_t a = Operand(random);
        std::uint32_t b = Operand(random);
        std::uint32_t c = Operand(random);
        switch (random() % 6U) {
        case 0:
            b = a ^ 0x80000000U;  // exact cancellation
            break;
        case 1:
            c = a32::VfpBinary32Multiply(a, b, 0U).value ^ 0x80000000U;
            break;
        case 2:
            // Operand pairs whose sum or product lands near 2^-126.
            b = (b & 0x807FFFFFU) | ((static_cast<std::uint32_t>(random() % 8U)) << 23U);
            break;
        default:
            break;
        }
        for (const std::uint32_t fpscr : modes) {
            CheckOperands(a, b, c, fpscr);
        }
    }
}

}  // namespace

int main() {
    // A few exact values the random campaign could miss.
    for (const std::uint32_t fpscr : {0U, 1U << 24U, 1U << 25U}) {
        CheckOperands(0x3F800000U, 0xBF800000U, 0x00000000U, fpscr);   // 1 + -1
        CheckOperands(0x80000000U, 0x80000000U, 0x80000000U, fpscr);   // -0 cases
        CheckOperands(0x7F7FFFFFU, 0x7F7FFFFFU, 0x7F7FFFFFU, fpscr);   // overflow
        CheckOperands(0x0C000000U, 0x33800000U, 0x00800000U, fpscr);   // near 2^-126
        CheckOperands(0x80000000U, 0x4B000001U, 0x7FC00000U, fpscr);   // INT_MIN, NaN
        CheckOperands(0x7FFFFFFFU, 0x3F800001U, 0xFF800000U, fpscr);
    }
    RunCampaign(0x0A32F00DULL, 400000);
#if defined(_M_X64) || defined(__x86_64__)
    // Host FTZ/DAZ must not reach the fast path's result or error term.
    const unsigned int saved = _mm_getcsr();
    _mm_setcsr(saved | 0x8040U);
    RunCampaign(0xDA2F72ULL, 200000);
    _mm_setcsr(saved);
#endif
    // The campaign must exercise the fast paths, not only their fallbacks.
    if (gFastAdds < 100000U || gFastMultiplies < 100000U) {
        std::cerr << "fast paths taken too rarely: add " << gFastAdds
                  << ", multiply " << gFastMultiplies << '\n';
        return 1;
    }
    if (gFailures != 0) {
        std::cerr << gFailures << " of " << gChecks << " VFP fast-path checks differ\n";
        return 1;
    }
    std::cout << "VFP fast paths match the software implementation (" << gChecks
              << " checks; fast add " << gFastAdds << ", fast multiply "
              << gFastMultiplies << ")\n";
    return 0;
}
