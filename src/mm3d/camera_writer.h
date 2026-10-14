// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
class QString;
namespace MM3D::CameraWriter {
inline std::atomic<bool> active{false};
struct Instruction {
    std::uint32_t pc{}, lr{}, sp{};
    bool thumb{};
};
struct Hit {
    Instruction instruction;
    std::uint32_t address{}, value{}, core{}, process{}, instruction_word{};
    std::array<std::uint32_t,16> stack{};
    std::uint32_t stack_valid{};
};
inline bool Matches(std::uint32_t value) {
    const float f = std::abs(std::bit_cast<float>(value));
    return std::abs(f - 2.41421356f) < .00001f || std::abs(f - 1.44852814f) < .00001f;
}
void Start(unsigned delay_seconds = 3);
void Stop();
bool ShouldTrace();
void Observe(const Hit& hit);
QString Status();
bool Export(const QString& path);
}
