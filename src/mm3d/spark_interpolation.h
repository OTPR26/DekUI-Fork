// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/water_particle_interpolation.h"

namespace MM3D::SparkInterpolation {
using ForestParticleInterpolation::Vertex;
using ForestParticleInterpolation::Quad;
using ForestParticleInterpolation::Near;
using ForestParticleInterpolation::Center;
using Key = std::array<std::uint32_t, 2>; // active texture, scene generation

inline std::optional<std::vector<Quad>> Decode(std::span<const Vertex> inputs,
                                              std::span<const std::uint32_t> order) {
    if (inputs.size() > 257 || order.size() > 384) return {};
    const auto decoded = WaterParticleInterpolation::DecodeQuads(inputs, order);
    if (!decoded) return {};
    std::vector<Quad> out;
    for (const auto& q : *decoded) {
        const auto& color = q.vertices[0][1];
        // The fuse uses small yellow/white stars. The same batch may contain
        // larger gray particles; leave those and their input slots untouched.
        if (!Near(color[0], 1) || !Near(color[1], 1) || color[2] < 0 || color[2] > 1 ||
            color[3] > 178.f/255+.001f) continue;
        float aa = 0, bb = 0, ab = 0;
        for (unsigned c = 0; c < 3; ++c) {
            const float a = q.vertices[1][0][c]-q.vertices[0][0][c];
            const float b = q.vertices[2][0][c]-q.vertices[0][0][c];
            aa += a*a;bb += b*b;ab += a*b;
        }
        // Captured edges span 1.4--sqrt(5) units and rotate twenty degrees
        // per native frame. Require the doubled UV square as well as size.
        if (aa < 1.95f || aa > 5.02f || std::abs(aa-bb) > .01f || std::abs(ab) > .01f) continue;
        bool uv = true;
        for (unsigned k = 0; k < 4; ++k)
            uv &= Near(q.vertices[k][2][0], k < 2 ? 0.f : 2.f) &&
                  Near(q.vertices[k][2][1], k%2 ? 0.f : 2.f);
        if (uv) out.push_back(q);
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
    const auto& a = current.vertices[0][1];
    const auto& b = previous.vertices[0][1];
    for (unsigned c = 0; c < 3; ++c) if (!Near(a[c], b[c])) return false;
    // Lifetime fade is either stationary or decreases by at most 20/255.
    // A newly spawned spark must not inherit a fading neighbor's history.
    if (a[3] > b[3]+.001f || b[3]-a[3] > .08f) return false;
    const auto ac = Center(current), bc = Center(previous);
    float distance = 0, dot = 0, aa = 0, bb = 0;
    for (unsigned c = 0; c < 3; ++c) {
        distance += (ac[c]-bc[c])*(ac[c]-bc[c]);
        const float u = current.vertices[1][0][c]-current.vertices[0][0][c];
        const float v = previous.vertices[1][0][c]-previous.vertices[0][0][c];
        dot += u*v;aa += u*u;bb += v*v;
    }
    return distance <= 9 && dot > .85f*std::sqrt(aa*bb);
}
inline std::optional<unsigned> Match(const Quad& q, const std::vector<Quad>& candidates, bool newer) {
    float best = std::numeric_limits<float>::infinity(), second = best;
    std::optional<unsigned> match;
    const auto center = Center(q);
    for (unsigned i = 0; i < candidates.size(); ++i) {
        const auto& other = candidates[i];
        if (!(newer ? SparkInterpolation::Compatible(q, other) : SparkInterpolation::Compatible(other, q))) continue;
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
        if (quads.empty() || quads.size() > 64) return {};
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
        // A spark can occur in two overlapping submissions in one game frame.
        // Both use the same immutable previous-frame pool. Deduplicate exact
        // copies for the next frame, not their draw calls or native rendering.
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
            const auto match = Match(quads[i], e.previous, true);
            if (!match || Match(e.previous[*match], quads, false) != i) continue;
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
