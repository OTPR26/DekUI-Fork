// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace MM3D::Trace {
struct Usage {
    std::array<bool, 96> direct{}, dot{}, relative_base{};
    bool known{};
};
// Conservative scan of uploaded instructions, NOT executed control-flow analysis.
Usage AnalyzeUsage(std::span<const std::uint32_t> code);
}
