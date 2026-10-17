// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/forest_particle_interpolation.h"
#include "mm3d/ripple_interpolation.h"

namespace MM3D::WaterParticleInterpolation {
using ForestParticleInterpolation::Vertex;
using ForestParticleInterpolation::Quad;
using ForestParticleInterpolation::Center;
using ForestParticleInterpolation::Near;
using Key = std::array<std::uint32_t, 4>; // textures and scene generation

// Resolve the strip's visible corners separately from its degenerate connectors.
// The capture cache may reorder inputs; only submission order identifies corners.
inline std::optional<std::vector<Quad>> DecodeQuads(std::span<const Vertex> inputs,
                                                  std::span<const std::uint32_t> order,
                                                  bool uniform_color = true, float max_alpha = 1.f) {
    if (inputs.empty() || inputs.size() > 513 || order.empty() || order.size() > 768 ||
        order.size() % 6) return {};
    for (auto i : order) if (i >= inputs.size()) return {};
    std::vector<bool> used(inputs.size());
    std::vector<Quad> out;
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
            if (!Near(v[0][3], 1) || !Near(v[2][2], 0) || !Near(v[2][3], 1) ||
                v[1][3] < 0 || v[1][3] > max_alpha) return {};
            if (uniform_color) for (unsigned c = 0; c < 4; ++c)
                if (!Near(v[1][c], q.vertices[0][1][c])) return {};
        }
        for (unsigned c = 0; c < 3; ++c)
            if (std::abs(q.vertices[3][0][c] - q.vertices[1][0][c] -
                         q.vertices[2][0][c] + q.vertices[0][0][c]) > .01f) return {};
        out.push_back(q);
    }
    if (used[order.back()]) return {};
    return out;
}

inline std::optional<std::vector<Quad>> DecodeDrops(std::span<const Vertex> inputs,
                                                  std::span<const std::uint32_t> order) {
    if (order.size() > 384) return {};
    auto quads = DecodeQuads(inputs, order);
    if (!quads) return {};
    for (const auto& q : *quads) {
        for (unsigned v = 0; v < 4; ++v) {
            const auto& color = q.vertices[v][1];
            if (!Near(color[0], 1) || !Near(color[1], 1) || !Near(color[2], 1) ||
                !Near(color[3], 150.f/255)) return {};
            if (!Near(q.vertices[v][2][0], float(v/2)) ||
                !Near(q.vertices[v][2][1], float(1-v%2))) return {};
        }
        // Captured drops and their small impact fragments have 1.5--40 unit
        // edges. Exclude large water sheets and unrelated white screen quads.
        for (unsigned v : {1u, 2u}) {
            float length2 = 0;
            for (unsigned c = 0; c < 3; ++c) {
                const float d = q.vertices[v][0][c] - q.vertices[0][0][c];
                length2 += d*d;
            }
            if (length2 < .25f || length2 > 4096) return {};
        }
    }
    return quads;
}

struct Rings {
    RippleInterpolation::Mesh mesh;
    std::vector<std::uint32_t> indices;
};
inline std::optional<Rings> DecodeRipples(std::span<const Vertex> inputs,
                                        std::span<const std::uint32_t> order) {
    if (order.size() % 12) return {};
    const auto quads = DecodeQuads(inputs, order);
    if (!quads) return {};
    Rings rings;
    // CPU strip corners run down each half of the ring. The existing ring
    // matcher uses corners across it; canonicalize before matching histories.
    constexpr unsigned canonical[]{0, 2, 1, 3, 4, 6, 5, 7};
    for (unsigned i = 0; i < quads->size(); i += 2) {
        for (unsigned k = 0; k < 8; ++k) {
            const auto& q = (*quads)[i+k/4];
            const auto& v = q.vertices[k%4];
            if (!Near(v[2][0], k%4 < 2 ? 1.f : 0.f) ||
                !Near(v[2][1], float(k%2))) return {};
        }
        const auto start = static_cast<unsigned>(rings.mesh.points.size());
        for (auto k : canonical) {
            const auto& q = (*quads)[i+k/4];
            const auto& v = q.vertices[k%4];
            rings.mesh.points.push_back({v[0][0], v[0][1], v[0][2]});
            rings.mesh.colors.push_back(v[1]);
            rings.indices.push_back(q.indices[k%4]);
        }
        if (!RippleInterpolation::Describe(rings.mesh, start)) return {};
    }
    // The ring history expects a trailing connector. It is never written back.
    rings.mesh.points.push_back({});
    rings.mesh.colors.push_back({});
    return rings;
}

inline bool CompatibleDrop(const Quad& a, const Quad& b) {
    const auto ac = Center(a), bc = Center(b);
    float distance = 0;
    for (unsigned c = 0; c < 3; ++c) distance += (ac[c]-bc[c])*(ac[c]-bc[c]);
    // Fast falling drops move 13 1/3 units per guest frame. New distant drops
    // must not inherit an old drop's history when the particle array compacts.
    if (distance > 16*16) return false;
    for (unsigned v = 0; v < 4; ++v)
        for (unsigned c = 0; c < 3; ++c)
            if (std::abs(a.vertices[v][0][c] - b.vertices[v][0][c]) > 24) return false;
    return true;
}
inline std::optional<unsigned> MatchDrop(const Quad& q, const std::vector<Quad>& candidates) {
    std::optional<unsigned> index;
    float best = std::numeric_limits<float>::infinity(), second = best;
    const auto center = Center(q);
    for (unsigned i = 0; i < candidates.size(); ++i) {
        if (!CompatibleDrop(q, candidates[i])) continue;
        const auto other = Center(candidates[i]);
        float distance = 0;
        for (unsigned c = 0; c < 3; ++c) distance += (center[c]-other[c])*(center[c]-other[c]);
        if (distance < best) { second = best; best = distance; index = i; }
        else if (distance < second) second = distance;
    }
    return second > best*1.5f + .01f ? index : std::nullopt;
}

class History {
    struct Drops { std::uint64_t frame; std::vector<Quad> quads; };
    struct Ripples { std::uint64_t frame; RippleInterpolation::History history; };
    std::map<Key, Drops> drops;
    std::map<Key, Ripples> ripples;
    template <typename Entries>
    static void Prune(Entries& entries, std::uint64_t frame) {
        for (auto it = entries.begin(); it != entries.end();)
            if (it->second.frame > frame || frame-it->second.frame > 1) it = entries.erase(it); else ++it;
    }
public:
    void Reset() { drops.clear(); ripples.clear(); }
    std::optional<std::vector<Quad>> ApplyDrops(std::uint64_t frame, const Key& key,
                                               const std::vector<Quad>& quads) {
        if (quads.empty() || quads.size() > 64) return {};
        Prune(drops, frame);
        auto it = drops.find(key);
        if (it == drops.end()) {
            if (drops.size() < 8) drops.emplace(key, Drops{frame, quads});
            return {};
        }
        auto& e = it->second;
        if (e.frame == frame) { e.quads.clear(); return {}; }
        auto out = quads;
        bool changed = false;
        if (e.frame + 1 == frame) {
            for (unsigned i = 0; i < quads.size(); ++i) {
                const auto match = MatchDrop(quads[i], e.quads);
                if (!match || MatchDrop(e.quads[*match], quads) != i) continue;
                for (unsigned v = 0; v < 4; ++v)
                    for (unsigned c = 0; c < 3; ++c)
                        out[i].vertices[v][0][c] =
                            (quads[i].vertices[v][0][c] + e.quads[*match].vertices[v][0][c]) * .5f;
                changed = true;
            }
        }
        e = {frame, quads};
        return changed ? std::optional{std::move(out)} : std::nullopt;
    }
    std::optional<RippleInterpolation::Mesh> ApplyRipples(std::uint64_t frame, const Key& key,
                                                         const Rings& rings) {
        Prune(ripples, frame);
        auto it = ripples.find(key);
        if (it == ripples.end()) {
            if (ripples.size() >= 8) return {};
            it = ripples.emplace(key, Ripples{frame, {}}).first;
        } else if (it->second.frame == frame) {
            it->second.history = {};
            return {}; // ambiguous repeated batch cannot seed the next frame
        }
        it->second.frame = frame;
        return it->second.history.Apply(frame, key[0], key[3], rings.mesh);
    }
};
}
