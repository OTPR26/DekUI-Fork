// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/forest_particle_interpolation.h"
#include "mm3d/link_pose_math.h"

namespace MM3D::SwordTrailInterpolation {
using ForestParticleInterpolation::Vertex;
using ForestParticleInterpolation::Near;
using LinkPoseMath::Matrix;
using Key = std::array<std::uint32_t, 2>; // active texture, scene generation
struct Ribbon {
    std::vector<Vertex> vertices; // paired outer/inner edges, oldest to newest
    std::vector<std::uint32_t> indices;
};
inline std::optional<Ribbon> Decode(std::span<const Vertex> inputs,
                                    std::span<const std::uint32_t> order) {
    // Captured sword blur: one quad, or four subdivisions per history segment.
    if (order.size() < 4 || order.size() > 118 || (order.size()+2)%6) return {};
    const unsigned sections = unsigned((order.size()+2)/6)+1;
    if (sections != 2 && (sections < 5 || (sections-1)%4)) return {};
    Ribbon out;
    for (unsigned i = 0; i < sections; ++i) {
        const unsigned offset = i == 0 ? 0 : (i-1)*6+2;
        for (unsigned side = 0; side < 2; ++side) {
            const auto index = order[offset+side];
            if (index >= inputs.size() ||
                std::find(out.indices.begin(), out.indices.end(), index) != out.indices.end()) return {};
            const auto& v = inputs[index];
            for (const auto& a : v) for (float x : a) if (!std::isfinite(x)) return {};
            if (!Near(v[0][3], 1) || !Near(v[1][0], 1) || !Near(v[1][1], 1) ||
                !Near(v[1][2], 1) || v[1][3] < 0 || v[1][3] > 213.f/255 ||
                !Near(v[2][0], float(i)) || !Near(v[2][1], float(1-side)) ||
                !Near(v[2][2], 0) || !Near(v[2][3], 1)) return {};
            out.vertices.push_back(v);out.indices.push_back(index);
        }
        if (i && (out.vertices[2*i][1][3] < out.vertices[2*i-2][1][3] ||
                  out.vertices[2*i+1][1][3] > .21f)) return {};
    }
    // Validate every connector too; other particle strips must not enter here.
    for (unsigned i = 0; i+1 < sections; ++i) {
        const std::array<unsigned, 6> expected{2*i, 2*i+1, 2*i+2, 2*i+3, 2*i+3, 2*i+2};
        for (unsigned k = 0; k < (i+2 == sections ? 4u : 6u); ++k)
            if (order[6*i+k] != out.indices[expected[k]]) return {};
    }
    return out;
}
inline void Transform(Vertex& v, const Matrix& m) {
    const auto p = v[0];
    for (unsigned r = 0; r < 3; ++r) {
        double x = m[r][3];
        for (unsigned c = 0; c < 3; ++c) x += m[r][c]*p[c];
        v[0][r] = float(x);
    }
}
class History {
    struct Entry {
        std::uint64_t frame{};
        std::vector<Vertex> current, previous;
        bool ambiguous{};
    };
    std::map<Key, Entry> entries;
public:
    void Reset() { entries.clear(); }
    std::optional<Ribbon> Apply(std::uint64_t frame, const Key& key,
                                const Ribbon& ribbon, const Matrix& world) {
        if (ribbon.vertices.size() < 4 || ribbon.vertices.size() > 42 ||
            !LinkPoseMath::Rigid(world)) return {};
        for (auto it = entries.begin(); it != entries.end();)
            if (it->second.frame > frame || frame-it->second.frame > 1) it = entries.erase(it); else ++it;
        auto found = entries.find(key);
        if (found == entries.end()) {
            if (entries.size() >= 8) return {};
            found = entries.emplace(key, Entry{frame, {}, {}, false}).first;
        }
        auto current = ribbon.vertices;
        for (auto& v : current) Transform(v, world);
        auto& e = found->second;
        if (e.frame != frame) {
            e.previous = e.ambiguous ? std::vector<Vertex>{} : std::move(e.current);
            e.current.clear();e.frame = frame;e.ambiguous = false;
        }
        if (!e.current.empty() && e.current != current) {
            e.ambiguous = true;e.previous.clear();return {};
        }
        e.current = current;
        if (e.ambiguous || e.previous.empty()) return {};
        const auto& previous = e.previous;
        // Aging lowers the oldest edge's opacity. An increase is a new swing.
        if (current.front()[1][3] > previous.front()[1][3]+.004f) return {};
        // Consecutive ribbons retain at least one old cross-section. Reject
        // separate effects/teleports rather than morphing between distant trails.
        double closest = std::numeric_limits<double>::infinity();
        for (unsigned i = 0; i < current.size(); i += 2)
            for (unsigned j = 0; j < previous.size(); j += 2) {
                double distance = 0;
                for (unsigned s = 0; s < 2; ++s) for (unsigned c = 0; c < 3; ++c)
                    distance += std::pow(current[i+s][0][c]-previous[j+s][0][c], 2);
                closest = std::min(closest, distance);
            }
        if (closest > 64) return {};
        const auto inverse = LinkPoseMath::InverseRigid(world);
        auto out = ribbon;
        const unsigned count = unsigned(current.size()/2), old_count = unsigned(previous.size()/2);
        for (unsigned i = 0; i < count; ++i) {
            // Resample the old ribbon onto the current topology. Buffer slot
            // numbers alone do not identify points when the trail grows/shrinks.
            const double t = double(i)*(old_count-1)/(count-1);
            const unsigned a = unsigned(t), b = std::min(a+1, old_count-1);
            const float f = float(t-a);
            for (unsigned s = 0; s < 2; ++s) {
                auto& v = out.vertices[2*i+s];
                for (unsigned c = 0; c < 3; ++c)
                    v[0][c] = (current[2*i+s][0][c] +
                        std::lerp(previous[2*a+s][0][c], previous[2*b+s][0][c], f))*.5f;
                v[1][3] = (current[2*i+s][1][3] +
                    std::lerp(previous[2*a+s][1][3], previous[2*b+s][1][3], f))*.5f;
                Transform(v, inverse);
            }
        }
        return out;
    }
};
}
