// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace MM3D::ForestParticleInterpolation {
using Vertex = std::array<std::array<float, 4>, 3>; // position, color, UV
struct Quad {
    std::array<Vertex, 4> vertices;
    std::array<std::uint32_t, 4> indices;
};
inline bool Near(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < .001f;
}
inline unsigned Kind(const Vertex& v) {
    const auto& c = v[1];
    if (Near(c[0], 5.f/255) && Near(c[1], 75.f/255) && Near(c[2], 155.f/255)) return 1;
    if (Near(c[0], 1) && Near(c[1], 155.f/255) && Near(c[2], 0)) return 2;
    return 0;
}
inline std::array<float, 3> Center(const Quad& q) {
    std::array<float, 3> c{};
    for (const auto& v : q.vertices)
        for (unsigned a = 0; a < 3; ++a) c[a] += v[0][a] * .25f;
    return c;
}
// Captured forest sprites are separate four-vertex squares in a strip, joined
// by two degenerate vertices. Decode submission order, never cache-miss order.
inline std::optional<std::vector<Quad>> Decode(std::span<const Vertex> inputs,
                                              std::span<const std::uint32_t> order) {
    if (inputs.empty() || inputs.size() > 97 || order.empty() || order.size() > 144 || order.size() % 6) return {};
    for (auto i : order) if (i >= inputs.size()) return {};
    std::vector<bool> used(inputs.size());
    std::vector<Quad> out;
    unsigned kind = 0;
    for (unsigned i = 0; i < order.size(); i += 6) {
        if (order[i+4] != order[i+3] || (i+6 < order.size() && order[i+5] != order[i+6])) return {};
        Quad q{};
        for (unsigned k = 0; k < 4; ++k) {
            const auto index = order[i+k];
            if (used[index]) return {};
            used[index] = true;
            q.indices[k] = index;
            q.vertices[k] = inputs[index];
            const auto& v = q.vertices[k];
            for (const auto& row : v) for (float f : row) if (!std::isfinite(f)) return {};
            if (!Kind(v) || (kind && Kind(v) != kind)) return {};
            kind = Kind(v);
            if (!Near(v[0][3], 1) || !Near(v[2][2], 0) || !Near(v[2][3], 1) || v[1][3] < 0 || v[1][3] > 1) return {};
            for (unsigned a = 0; a < 4; ++a) if (!Near(v[1][a], q.vertices[0][1][a])) return {};
        }
        // Captured squares grow from about .83 to 3.2 units; some blue
        // halos are 8 units wide. Reject non-square geometry and larger effects.
        float aa = 0, bb = 0, ab = 0;
        for (unsigned a = 0; a < 3; ++a) {
            const float u = q.vertices[1][0][a] - q.vertices[0][0][a];
            const float v = q.vertices[2][0][a] - q.vertices[0][0][a];
            aa += u*u; bb += v*v; ab += u*v;
            if (std::abs(q.vertices[3][0][a] - q.vertices[0][0][a] - u - v) > .01f) return {};
        }
        if (aa < .5f || aa > 64.1f || std::abs(aa - bb) > .1f || std::abs(ab) > .1f) return {};
        for (unsigned a = 0; a < 2; ++a) {
            if (!Near(q.vertices[0][2][a] + q.vertices[3][2][a], 1) ||
                !Near(q.vertices[1][2][a] + q.vertices[2][2][a], 1)) return {};
        }
        out.push_back(q);
    }
    // The trailing connector is not a visible corner and must remain untouched.
    if (used[order.back()]) return {};
    return out;
}
inline bool Compatible(const Quad& a, const Quad& b) {
    if (Kind(a.vertices[0]) != Kind(b.vertices[0]) ||
        std::abs(a.vertices[0][1][3] - b.vertices[0][1][3]) > .12f) return false;
    const auto ac = Center(a), bc = Center(b);
    float distance = 0;
    for (unsigned k = 0; k < 3; ++k) distance += (ac[k]-bc[k])*(ac[k]-bc[k]);
    if (distance > 9) return false; // captured motion < 2.3 units per guest frame
    for (unsigned v = 0; v < 4; ++v) {
        for (unsigned k = 0; k < 3; ++k)
            if (std::abs(a.vertices[v][0][k] - b.vertices[v][0][k]) > 4) return false;
        for (unsigned k = 0; k < 2; ++k)
            if (std::abs(a.vertices[v][2][k] - b.vertices[v][2][k]) > .3f) return false;
    }
    return true;
}
inline std::optional<unsigned> Match(const Quad& q, const std::vector<Quad>& candidates) {
    std::optional<unsigned> index;
    float best = std::numeric_limits<float>::infinity(), second = best;
    const auto center = Center(q);
    for (unsigned i = 0; i < candidates.size(); ++i) {
        if (!Compatible(q, candidates[i])) continue;
        const auto other = Center(candidates[i]);
        float distance = 0;
        for (unsigned a = 0; a < 3; ++a) distance += (center[a]-other[a])*(center[a]-other[a]);
        if (distance < best) { second = best; best = distance; index = i; }
        else if (distance < second) second = distance;
    }
    // Require a clear nearest neighbor; near ties stay native.
    return second > best * 1.5f + .01f ? index : std::nullopt;
}
class History {
    struct Entry { std::uint64_t frame{}; std::vector<Quad> quads; };
    using Key = std::array<std::uint32_t, 3>; // texture, generation, sprite kind
    std::map<Key, Entry> entries;
public:
    void Reset() { entries.clear(); }
    std::optional<std::vector<Quad>> Apply(std::uint64_t frame, std::uint32_t texture,
                                         std::uint32_t generation, const std::vector<Quad>& quads) {
        if (quads.empty() || quads.size() > 24) return {};
        for (auto it = entries.begin(); it != entries.end();)
            if (it->second.frame > frame || frame-it->second.frame > 1) it = entries.erase(it); else ++it;
        const Key key{texture, generation, Kind(quads[0].vertices[0])};
        auto it = entries.find(key);
        if (it == entries.end()) {
            if (entries.size() < 8) entries.emplace(key, Entry{frame, quads});
            return {};
        }
        auto& e = it->second;
        if (e.frame == frame) { e.quads.clear(); return {}; } // ambiguous repeated batch
        auto out = quads;
        bool changed = false;
        if (e.frame + 1 == frame) {
            for (unsigned i = 0; i < quads.size(); ++i) {
                const auto match = Match(quads[i], e.quads);
                if (!match || Match(e.quads[*match], quads) != i) continue;
                for (unsigned v = 0; v < 4; ++v)
                    for (unsigned a = 0; a < 3; ++a)
                        for (unsigned c = 0; c < 4; ++c)
                            out[i].vertices[v][a][c] = (quads[i].vertices[v][a][c] + e.quads[*match].vertices[v][a][c]) * .5f;
                changed = true;
            }
        }
        e = {frame, quads};
        return changed ? std::optional{std::move(out)} : std::nullopt;
    }
};
}
