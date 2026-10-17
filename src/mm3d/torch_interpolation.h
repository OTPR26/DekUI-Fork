// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace MM3D::TorchInterpolation {
using Vertex = std::array<std::array<float, 4>, 3>; // position, UV, normal
using Point = std::array<float, 3>;
using Key = std::array<std::uint32_t, 4>; // three textures and scene generation
inline constexpr std::array<unsigned, 6> Rows{10, 11, 14, 15, 17, 18};
using Values = std::array<std::array<float, 4>, 6>;
using Offsets = std::array<float, 2>; // scrolling layers at rows 15 and 18
inline bool Near(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < .001f;
}

// The captured upright torch quad has two scrolling UV layers, the last moving
// four times faster. Require its geometry as well as all three UV bases;
// six vertices alone also
// describes many unrelated effects. Indices refer to resolved CPU inputs.
inline bool Recognized(std::span<const Vertex> inputs, std::span<const std::uint32_t> order,
                       const Values& uv) {
    if (inputs.size() != 4 || order.size() != 6) return false;
    constexpr unsigned corners[]{0, 1, 2, 0, 3, 1};
    constexpr float positions[4][2]{{-4000, 0}, {4000, 16000}, {-4000, 16000}, {4000, 0}};
    constexpr float texcoords[4][2]{{0, 0}, {1, 1}, {0, 1}, {1, 0}};
    const std::array<std::uint32_t, 4> indices{order[0], order[1], order[2], order[4]};
    for (unsigned i = 0; i < 4; ++i) {
        if (indices[i] >= inputs.size()) return false;
        for (unsigned j = 0; j < i; ++j) if (indices[i] == indices[j]) return false;
        const auto& v = inputs[indices[i]];
        for (unsigned c = 0; c < 4; ++c) {
            const float position = c < 2 ? positions[i][c] : c == 3 ? 1.f : 0.f;
            const float texcoord = c < 2 ? texcoords[i][c] : c == 3 ? 1.f : 0.f;
            if (!Near(v[0][c], position) || !Near(v[1][c], texcoord) ||
                !Near(v[2][c], c == 2 ? 1.f : 0.f)) return false;
        }
    }
    for (unsigned i = 0; i < 6; ++i) if (order[i] != indices[corners[i]]) return false;
    constexpr float basis[]{1, 1, 1, 1, -2, 2};
    for (unsigned r = 0; r < Rows.size(); ++r)
        for (unsigned c = 0; c < 4; ++c) {
            if ((r == 3 || r == 5) && c == 3) continue;
            if (!Near(uv[r][c], c == r % 2 ? basis[r] : 0.f)) return false;
        }
    return std::isfinite(uv[3][3]) && uv[3][3] >= -1 && uv[3][3] <= 0 &&
           Near(uv[5][3], 4 * uv[3][3]);
}

class History {
    struct Entry {
        Key key;
        Point origin;
        std::uint64_t frame;
        float phase;
        std::optional<Offsets> half;
        bool ambiguous{};
    };
    std::vector<Entry> entries;
public:
    void Reset() { entries.clear(); }
    std::optional<Offsets> Apply(std::uint64_t frame, const Key& key, const Point& origin, float phase) {
        if (!std::isfinite(phase) || phase < -1 || phase > 0) return {};
        for (float x : origin) if (!std::isfinite(x)) return {};
        for (auto it = entries.begin(); it != entries.end();)
            if (it->frame > frame || frame - it->frame > 1) it = entries.erase(it); else ++it;
        Entry* match = nullptr;
        for (auto& e : entries) {
            if (e.key != key) continue;
            bool same = true;
            for (unsigned c = 0; c < 3; ++c) same &= std::abs(e.origin[c] - origin[c]) < .05f;
            if (!same) continue;
            if (match) return {};
            match = &e;
        }
        if (!match) {
            if (entries.size() < 64) entries.push_back({key, origin, frame, phase, {}});
            return {};
        }
        auto& e = *match;
        if (e.frame == frame) {
            if (!Near(phase, e.phase)) { e.ambiguous = true; e.half.reset(); }
            return e.half;
        }
        // Measured torch scroll is -1/60 texture repeat per guest frame. Accept
        // stationary phases too, but reject resets and unrelated animations.
        float delta = phase - e.phase;
        delta -= std::round(delta);
        std::optional<Offsets> half;
        if (!e.ambiguous && e.frame + 1 == frame &&
            (std::abs(delta) < .00001f || std::abs(delta + 1.f / 60) < .00001f)) {
            float value = phase - delta * .5f;
            if (value > 0) value -= 1;
            if (value < -1) value += 1;
            // Keep both layers in phase, including when the slow layer wraps
            // by one repeat and the fast layer wraps by four repeats.
            half = Offsets{value, 4 * value};
        }
        e = {key, origin, frame, phase, half};
        return half;
    }
};
}
