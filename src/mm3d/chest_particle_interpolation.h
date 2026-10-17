// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include "mm3d/water_particle_interpolation.h"

namespace MM3D::ChestParticleInterpolation {
using ForestParticleInterpolation::Vertex;
using ForestParticleInterpolation::Quad;
using ForestParticleInterpolation::Near;
using ForestParticleInterpolation::Center;
using Key = std::array<std::uint32_t, 2>; // active texture, scene generation

// Blue squares surrounding a chest while it materializes. Match the full
// material, not a texture address, which can change when the room reloads.
inline bool Material(const std::array<std::array<std::uint32_t, 5>, 6>& t) {
    constexpr std::array<std::array<std::uint32_t, 5>, 6> expected{{
        {249564899, 0, 0, 0, 2}, {250482430, 0, 1, 38655, 0},
        {249564976, 0, 8, 0, 0}, {268374015, 0, 0, 0xffffffff, 0},
        {268374015, 0, 0, 0xffffffff, 0}, {268374015, 0, 0, 0xffffffff, 0},
    }};
    return t == expected;
}
inline std::optional<std::vector<Quad>> Decode(std::span<const Vertex> inputs,
                                              std::span<const std::uint32_t> order) {
    if (order.size() > 32 * 6) return {};
    const auto decoded = WaterParticleInterpolation::DecodeQuads(inputs, order);
    if (!decoded) return {};
    std::vector<Quad> out;
    for (const auto& q : *decoded) {
        const auto& color = q.vertices[0][1];
        if (!Near(color[0], 0) || !Near(color[1], 105.f/255) ||
            !Near(color[2], 1) || !Near(color[3], 1)) return {};
        float aa = 0, bb = 0, ab = 0;
        for (unsigned c = 0; c < 3; ++c) {
            const float a = q.vertices[1][0][c] - q.vertices[0][0][c];
            const float b = q.vertices[2][0][c] - q.vertices[0][0][c];
            aa += a*a; bb += b*b; ab += a*b;
        }
        if (std::abs(aa-bb) > .01f*std::max(1.f, aa) ||
            std::abs(ab) > .002f*std::max(1.f, aa) || aa > 23*23) return {};
        for (unsigned k = 0; k < 4; ++k)
            if (!Near(q.vertices[k][2][0], float(k/2)) ||
                !Near(q.vertices[k][2][1], float(1-k%2))) return {};
        // Collapsed particles are births/deaths, not reliable history anchors.
        if (aa > .01f) out.push_back(q);
    }
    return out.empty() ? std::nullopt : std::optional{std::move(out)};
}
inline bool Same(const Quad& a, const Quad& b) {
    for (unsigned v = 0; v < 4; ++v)
        for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 4; ++c)
                if (!Near(a.vertices[v][r][c], b.vertices[v][r][c])) return false;
    return true;
}
inline bool Compatible(const Quad& current, const Quad& previous) {
    const auto ac = Center(current), bc = Center(previous);
    float distance = 0, dot = 0, aa = 0, bb = 0;
    for (unsigned c = 0; c < 3; ++c) {
        distance += (ac[c]-bc[c])*(ac[c]-bc[c]);
        const float u = current.vertices[1][0][c]-current.vertices[0][0][c];
        const float v = previous.vertices[1][0][c]-previous.vertices[0][0][c];
        dot += u*v;aa += u*u;bb += v*v;
    }
    // Native sizes intentionally jump every tick. Match motion and orientation,
    // allowing that size change to interpolate instead of rejecting it.
    return distance <= 4*4 &&
        dot > .8f*std::sqrt(aa*bb);
}
inline std::optional<unsigned> Match(const Quad& q, const std::vector<Quad>& candidates) {
    float best = std::numeric_limits<float>::infinity(), second = best;
    std::optional<unsigned> match;
    const auto center = Center(q);
    for (unsigned i = 0; i < candidates.size(); ++i) {
        const auto& other = candidates[i];
        if (!ChestParticleInterpolation::Compatible(q, other)) continue;
        const auto oc = Center(other);
        float distance = 0;
        for (unsigned c = 0; c < 3; ++c) distance += (center[c]-oc[c])*(center[c]-oc[c]);
        if (distance < best) { second = best;best = distance;match = i; }
        else if (distance < second) second = distance;
    }
    return second > best*1.5f+.01f ? match : std::nullopt;
}
class History {
    struct Entry {
        std::uint64_t frame;
        std::vector<Quad> previous, current;
        bool overflow{};
    };
    std::map<Key, Entry> entries;
public:
    void Reset() { entries.clear(); }
    std::optional<std::vector<Quad>> Apply(std::uint64_t frame, const Key& key,
                                         const std::vector<Quad>& quads) {
        if (quads.empty() || quads.size() > 32) return {};
        for (auto it = entries.begin(); it != entries.end();)
            if (it->second.frame > frame || frame-it->second.frame > 1) it = entries.erase(it); else ++it;
        auto it = entries.find(key);
        if (it == entries.end()) {
            if (entries.size() >= 8) return {};
            it = entries.emplace(key, Entry{frame, {}, {}}).first;
        }
        auto& e = it->second;
        if (e.frame != frame) {
            e.previous = std::move(e.current);e.current.clear();e.frame = frame;e.overflow = false;
        }
        if (e.overflow) return {};
        // Repeated submissions use the same immutable previous-frame pool.
        // Deduplicate copies for the next frame, preserving native draw calls.
        for (const auto& q : quads) {
            if (std::any_of(e.current.begin(), e.current.end(), [&](const auto& p) { return Same(q,p); })) continue;
            if (e.current.size() >= 128) {
                e.previous.clear();e.current.clear();e.overflow = true;return {};
            }
            e.current.push_back(q);
        }
        auto out = quads;
        bool changed = false;
        for (unsigned i = 0; i < quads.size(); ++i) {
            const auto match = ChestParticleInterpolation::Match(quads[i], e.previous);
            if (!match || ChestParticleInterpolation::Match(e.previous[*match], quads) != i) continue;
            for (unsigned v = 0; v < 4; ++v)
                for (unsigned a = 0; a < 2; ++a)
                    for (unsigned c = 0; c < 4; ++c)
                        out[i].vertices[v][a][c] = (quads[i].vertices[v][a][c]+e.previous[*match].vertices[v][a][c])*.5f;
            ForestParticleInterpolation::PreserveSpinSize(out[i], quads[i], e.previous[*match]);
            changed = true;
        }
        return changed ? std::optional{std::move(out)} : std::nullopt;
    }
};
}
