#pragma once

// Inline host-float fast paths for the scalar binary32 VFP operations that
// dominate translated title code. Each returns exactly what the authoritative
// software implementation in oot3d_native_a32_vfp_ops.h returns, value and
// cumulative FPSCR flags, and calls it for every case outside the fast path.
//
// The fast path requires round-to-nearest and operands that are +/-0 or
// normal with a biased exponent of at least 24. Nonzero results must have a
// biased exponent of at least 2. Under those bounds:
// - flush-to-zero and default-NaN modes cannot change the result: no operand
//   is denormal or NaN, and no exact result lies near the denormal range;
// - host FTZ/DAZ cannot change the result or the error term: every nonzero
//   intermediate is at least 2^-126;
// - overflow reaches the fallback because the host result is infinite.
// The only exception flag possible on the fast path is IXC. It is derived
// from an exact error term: TwoSum for addition, a double-precision product
// (24x24 bits fit in 53) for multiplication.
//
// Requires strict floating-point code generation (/fp:strict), which every
// translated-title build uses; value-changing reassociation would break TwoSum.

#include "oot3d_native_a32_vfp_ops.h"

#include <bit>
#include <cstdint>

namespace Oot3dNativeGame {

namespace VfpFastDetail {

inline constexpr std::uint32_t kRoundingModeMask = 0x00C00000U;
inline constexpr std::uint32_t kIxc = 1U << 4U;

[[nodiscard]] constexpr std::uint32_t BiasedExponent(std::uint32_t bits) noexcept {
    return (bits >> 23U) & 0xFFU;
}

[[nodiscard]] constexpr bool OperandEligible(std::uint32_t bits) noexcept {
    const std::uint32_t exponent = BiasedExponent(bits);
    return (exponent >= 24U && exponent != 0xFFU) || (bits & 0x7FFFFFFFU) == 0U;
}

[[nodiscard]] constexpr bool NonzeroResultEligible(std::uint32_t bits) noexcept {
    const std::uint32_t exponent = BiasedExponent(bits);
    return exponent >= 2U && exponent != 0xFFU;
}

[[nodiscard]] inline float AsFloat(std::uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}

[[nodiscard]] inline std::uint32_t AsBits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

// Round-to-nearest product with its IXC flag. False leaves the operation to
// the software implementation.
[[nodiscard]] inline bool Multiply(
    std::uint32_t left, std::uint32_t right,
    oot3d::recomp::a32::VfpBinary32Result& result) noexcept {
    if (!OperandEligible(left) || !OperandEligible(right)) {
        return false;
    }
    const double exact = static_cast<double>(AsFloat(left)) *
                         static_cast<double>(AsFloat(right));
    const float rounded = static_cast<float>(exact);
    const std::uint32_t bits = AsBits(rounded);
    if (exact == 0.0) {
        result = {bits, 0U};
        return true;
    }
    if (!NonzeroResultEligible(bits)) {
        return false;
    }
    result = {bits, static_cast<double>(rounded) == exact ? 0U : kIxc};
    return true;
}

[[nodiscard]] inline bool Add(
    std::uint32_t left, std::uint32_t right,
    oot3d::recomp::a32::VfpBinary32Result& result) noexcept {
    if (!OperandEligible(left) || !OperandEligible(right)) {
        return false;
    }
    const float a = AsFloat(left);
    const float b = AsFloat(right);
    const float sum = a + b;
    const std::uint32_t bits = AsBits(sum);
    if ((bits & 0x7FFFFFFFU) != 0U && !NonzeroResultEligible(bits)) {
        return false;
    }
    // Knuth TwoSum: the exact rounding error of a + b.
    const float virtualRight = sum - a;
    const float error = (a - (sum - virtualRight)) + (b - virtualRight);
    result = {bits, error == 0.0F ? 0U : kIxc};
    return true;
}

// Multiply-accumulate forms round the product, then the sum, and report the
// union of both steps' flags.
[[nodiscard]] inline bool MultiplyThenAdd(
    std::uint32_t accumulator, std::uint32_t left, std::uint32_t right,
    std::uint32_t negateAccumulator, std::uint32_t negateProduct,
    oot3d::recomp::a32::VfpBinary32Result& result) noexcept {
    oot3d::recomp::a32::VfpBinary32Result product{};
    oot3d::recomp::a32::VfpBinary32Result sum{};
    if (!Multiply(left, right, product) ||
        !Add(accumulator ^ negateAccumulator, product.value ^ negateProduct, sum)) {
        return false;
    }
    result = {sum.value, product.exception_flags | sum.exception_flags};
    return true;
}

} // namespace VfpFastDetail

inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpAdd(
    std::uint32_t left, std::uint32_t right, std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::Add(left, right, result)) [[likely]] {
        return result;
    }
    return oot3d::recomp::a32::VfpBinary32Add(left, right, fpscr);
}

inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpSubtract(
    std::uint32_t left, std::uint32_t right, std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::Add(left, right ^ 0x80000000U, result)) [[likely]] {
        return result;
    }
    return oot3d::recomp::a32::VfpBinary32Subtract(left, right, fpscr);
}

inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpMultiply(
    std::uint32_t left, std::uint32_t right, std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::Multiply(left, right, result)) [[likely]] {
        return result;
    }
    return oot3d::recomp::a32::VfpBinary32Multiply(left, right, fpscr);
}

inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpNegativeMultiply(
    std::uint32_t left, std::uint32_t right, std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::Multiply(left, right, result)) [[likely]] {
        return {result.value ^ 0x80000000U, result.exception_flags};
    }
    return oot3d::recomp::a32::VfpBinary32NegativeMultiply(left, right, fpscr);
}

// VMLA: accumulator + product.
inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpMultiplyAccumulate(
    std::uint32_t accumulator, std::uint32_t left, std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::MultiplyThenAdd(accumulator, left, right, 0U, 0U, result))
        [[likely]] {
        return result;
    }
    return oot3d::recomp::a32::VfpBinary32MultiplyAccumulate(
        accumulator, left, right, fpscr);
}

// VMLS: accumulator - product.
inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpMultiplySubtract(
    std::uint32_t accumulator, std::uint32_t left, std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::MultiplyThenAdd(
            accumulator, left, right, 0U, 0x80000000U, result)) [[likely]] {
        return result;
    }
    return oot3d::recomp::a32::VfpBinary32MultiplySubtract(
        accumulator, left, right, fpscr);
}

// VNMLA: -accumulator - product.
inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpNegativeMultiplyAccumulate(
    std::uint32_t accumulator, std::uint32_t left, std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::MultiplyThenAdd(
            accumulator, left, right, 0x80000000U, 0x80000000U, result)) [[likely]] {
        return result;
    }
    return oot3d::recomp::a32::VfpBinary32NegativeMultiplyAccumulate(
        accumulator, left, right, fpscr);
}

// VNMLS: -accumulator + product.
inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpNegativeMultiplySubtract(
    std::uint32_t accumulator, std::uint32_t left, std::uint32_t right,
    std::uint32_t fpscr) noexcept {
    oot3d::recomp::a32::VfpBinary32Result result{};
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U &&
        VfpFastDetail::MultiplyThenAdd(
            accumulator, left, right, 0x80000000U, 0U, result)) [[likely]] {
        return result;
    }
    return oot3d::recomp::a32::VfpBinary32NegativeMultiplySubtract(
        accumulator, left, right, fpscr);
}

// VCVT.F32.S32: exact unless |value| needs more than 24 significant bits.
inline oot3d::recomp::a32::VfpBinary32Result Oot3dVfpFromSigned(
    std::uint32_t value, std::uint32_t fpscr) noexcept {
    if ((fpscr & VfpFastDetail::kRoundingModeMask) == 0U) [[likely]] {
        const std::int32_t integer = static_cast<std::int32_t>(value);
        const float converted = static_cast<float>(integer);
        return {VfpFastDetail::AsBits(converted),
                static_cast<double>(converted) == static_cast<double>(integer)
                    ? 0U : VfpFastDetail::kIxc};
    }
    return oot3d::recomp::a32::VfpBinary32FromSigned(value, fpscr);
}

} // namespace Oot3dNativeGame
