// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#include <mutex>
#include <algorithm>

namespace MM3D::MessageInterpolation {
using Values = std::array<std::array<float, 4>, 96>;
using Key = std::array<std::uint32_t, 3>; // texture, framebuffer, scene generation
inline constexpr std::array<unsigned, 5> Rows{8, 11, 12, 13, 93};
using Pose = std::array<std::array<float, 4>, Rows.size()>;

inline bool Candidate(std::uint64_t shader, std::uint64_t swizzle,
                      unsigned vertices, float color_reference, bool indicators = false) {
    return shader == 0x9b343fea686d98e0ULL && swizzle == 0xac22b79e29209c01ULL &&
           vertices == 6 && (color_reference == -93.f || (indicators && color_reference == 1.f));
}
inline bool Near(float a, float b, float tolerance = .001f) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

// Read-only snapshots from the shared native MessageWindow draw path. Match
// actual submitted geometry, not a global "a textbox is open" flag: unrelated
// HUD draws can share this shader and even the same atlas.
using Source = std::array<std::uint32_t, 4>; // layout, pane, geometry, message ID
struct Panel {
    Source source{};
    std::array<std::array<float,4>,3> transform{};
    std::array<std::array<float,4>,2> uv{};
    bool operator==(const Panel&) const = default;
};
struct Snapshot {std::uint64_t frame;std::uint32_t generation;Panel panel;};
inline std::mutex snapshot_mutex;
inline std::vector<Snapshot> snapshots;
inline void Publish(std::uint64_t frame,std::uint32_t generation,const std::vector<Panel>& panels) {
    std::scoped_lock lock(snapshot_mutex);
    std::erase_if(snapshots,[&](const auto& s){return s.generation!=generation||s.frame>frame||frame-s.frame>7;});
    for(const auto& panel:panels) {
        if(!panel.source[0]||!panel.source[1]||!panel.source[2])continue;
        const bool duplicate=std::any_of(snapshots.begin(),snapshots.end(),[&](const auto& s){
            return s.frame==frame&&s.panel==panel;
        });
        if(!duplicate&&snapshots.size()<256)snapshots.push_back({frame,generation,panel});
    }
}
inline bool Active(std::uint64_t frame,std::uint32_t generation) {
    std::scoped_lock lock(snapshot_mutex);
    return std::any_of(snapshots.begin(),snapshots.end(),[&](const auto& s) {
        return s.generation==generation&&(s.frame==frame||s.frame+1==frame);
    });
}
inline std::optional<Source> SourceFor(std::uint64_t frame,std::uint32_t generation,const Values& u) {
    std::scoped_lock lock(snapshot_mutex);
    std::optional<Source> source;
    for(const auto& s:snapshots) {
        if(s.generation!=generation||(s.frame!=frame&&s.frame+1!=frame))continue;
        bool match=true;
        // The GPU receives float24-rounded native matrices. Allow that
        // rounding, but never borrow a different pane's nearby rectangle.
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
            match &= Near(s.panel.transform[r][c],u[11+r][c],.02f);
        for(unsigned r=0;r<2;++r)for(unsigned c=0;c<4;++c)
            match &= Near(s.panel.uv[r][c],u[94+r][c],.00001f);
        if(!match)continue;
        if(source&&*source!=s.panel.source)return {};
        source=s.panel.source;
    }
    return source;
}
inline bool OwnedPlate(const Values& u) {
    const bool indirect=u[10]==std::array<float,4>{0,3,94,-93};
    if(!indirect&&u[10]!=std::array<float,4>{0,3,94,1})return false;
    for(unsigned r:Rows)if(r!=93||indirect)for(float x:u[r])if(!std::isfinite(x))return false;
    for(unsigned r:{8u,93u})if(r!=93||indirect)for(float x:u[r])if(x<0||x>1.1f)return false;
    for(unsigned r:{94u,95u})for(float x:u[r])if(!std::isfinite(x)||std::abs(x)>2)return false;
    return u[13][1]>.001f&&u[13][1]<=4&&u[13][2]==0&&u[13][3]==0&&
        u[11][1]==0&&u[11][2]==0&&u[12][1]==0&&u[12][3]==0&&
        u[11][3]>0&&u[11][3]<=800&&u[12][2]>0&&u[12][2]<=480;
}

// Shared MessageWindow plate encoding observed through repeated sign and regular
// dialogue openings/closings: one axis-aligned quad and indirect tint. The sign
// uses the second quarter-atlas row; regular dialogue uses the first row.
// Identify the layout, never a particular sign, text string or VRAM address.
// Glyph batches and ordinary HUD quads do not have this combination.
inline bool Recognized(const Values& u) {
    const bool sign = u[95] == std::array<float, 4>{0, .25f, .25f, 0};
    const bool dialogue = u[95] == std::array<float, 4>{0, 0, .25f, 0};
    if (u[10] != std::array<float, 4>{0, 3, 94, -93} ||
        u[94] != std::array<float, 4>{0, 0, 0, 1} ||
        (!sign && !dialogue)) return false;
    for (unsigned row : Rows)
        for (float x : u[row]) if (!std::isfinite(x)) return false;
    // Regular dialogue animates RGB together and can become a translucent
    // black plate; its green and alpha channels are not fixed at one.
    for (float x : u[8]) if (x < 0 || x > 1.1f) return false;
    if (sign && (u[8][1] != 1 || u[8][3] != 1)) return false;
    // Another regular-dialogue variant uses the sign's warm fade: green and
    // alpha stay at one while red/blue rise together, including a small
    // overshoot. It still uses the first atlas row, not the sign's second row.
    const bool warm_fade = u[8][1] == 1 && u[8][3] == 1 &&
        Near(u[8][2], u[8][0] * (230.f / 255));
    // Some dialogue plates keep a fixed cream tint throughout their scale
    // animation instead of multiplying red and blue by a shared fade.
    const bool cream = Near(u[8][0], 250.f / 255) && u[8][1] == 1 &&
        Near(u[8][2], 230.f / 255) && u[8][3] == 1;
    if (dialogue && ((!Near(u[8][0], u[8][1]) && !warm_fade && !cream) || u[8][3] > 1)) return false;
    const float scale = u[13][1];
    return scale > .001f && scale <= 2.f && Near(u[13][0], 5.f * scale) &&
        u[13][2] == 0 && u[13][3] == 0 &&
        u[11][1] == 0 && u[11][2] == 0 && u[12][1] == 0 && u[12][3] == 0 &&
        u[11][3] > 0 && u[11][3] <= 800 && u[12][2] > 0 && u[12][2] <= 240;
}

class History {
    struct Entry { Key key; std::uint64_t frame; Values values; std::optional<Source> source; bool ambiguous{}; };
    std::vector<Entry> entries;
public:
    void Reset() { entries.clear(); }
    std::optional<Pose> Apply(std::uint64_t frame, const Key& key, const Values& u,
                              std::optional<Source> source = {}) {
        for (auto it = entries.begin(); it != entries.end();)
            if (it->frame > frame || frame - it->frame > 1) it = entries.erase(it);
            else ++it;
        if (!(source ? OwnedPlate(u) : Recognized(u))) {
            for (auto& e : entries) if (e.key == key) e.ambiguous = true;
            return {};
        }
        for (auto& e : entries) {
            if (e.key != key || e.source != source) continue;
            if (e.frame == frame) { e.ambiguous = true; return {}; }
            const auto& p = e.values;
            bool compatible = !e.ambiguous && e.frame + 1 == frame;
            // Projection, atlas and tint identity must stay fixed. Ignore stale
            // uniforms belonging to earlier, larger layout batches.
            for (unsigned row : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 94u, 95u})
                compatible &= p[row] == u[row];
            if(source) {
                // Same native window/pane/message: interpolate its own pose
                // regardless of tint, atlas row, pivot or entrance direction.
                for(unsigned r:{11u,12u,13u})for(unsigned c=0;c<4;++c)
                    compatible &= std::abs(p[r][c]-u[r][c])<=192;
                for(unsigned r:{8u,93u})if(r!=93||u[10][3]==-93)for(unsigned c=0;c<4;++c)
                    compatible &= std::abs(p[r][c]-u[r][c])<=.4f;
            } else {
                compatible &= p[93]==u[93];
                const float ps = p[13][1], ns = u[13][1];
                compatible &= std::abs(ps - ns) <= .4f;
                for (unsigned c = 0; c < 4; ++c)
                    compatible &= std::abs(p[8][c] - u[8][c]) <= .4f;
                // A popup scales about its center. Reject layout edits, different
                // windows sharing an atlas, and sudden repositioning.
                compatible &= Near(p[11][0] + p[11][3] * .5f, u[11][0] + u[11][3] * .5f, .1f) &&
                    Near(p[12][0] + p[12][2] * .5f, u[12][0] + u[12][2] * .5f, .1f) &&
                    Near(p[11][3] / ps, u[11][3] / ns, .1f) &&
                    Near(p[12][2] / ps, u[12][2] / ns, .1f);
            }
            std::optional<Pose> half;
            if (compatible) {
                half.emplace();
                for (unsigned r = 0; r < Rows.size(); ++r)
                    for (unsigned c = 0; c < 4; ++c)
                        (*half)[r][c] = Rows[r]==93&&u[10][3]==1 ? u[93][c] :
                            (p[Rows[r]][c] + u[Rows[r]][c]) * .5f;
            }
            e = {key, frame, u, source};
            return half;
        }
        if (entries.size() < 8) entries.push_back({key, frame, u, source});
        return {};
    }
};
}
