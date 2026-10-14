// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
namespace MM3D::Profiles {
// Diagnostic only: -1 missing/disabled signal, 0 absent, 1 observed last frame.
inline std::atomic<int> notebook_detection{-1};
void CompleteDetectionFrame();
bool NotebookDetectedThisFrame();
inline std::atomic<unsigned> guest_conflicts{0};
struct Rule {
    unsigned width{}, height{}, format{};
    std::string hash, context;
    bool operator==(const Rule&) const = default;
};
struct Profile {
    std::string id, name;
    bool enabled = true, presentation = false;
    bool source_top = false;
    bool panel_copy = false;
    std::vector<Rule> exclusions;
    std::string required_context;
    // Raw draw alpha channel, explicitly selected after checking the live probe.
    std::string fade_source;
    int fade_channel = 0;
    bool follow_fade = false;
    bool signal_only = false;
    float scale = 1.f, x = 0.f, y = 0.f;
    std::array<float,4> crop{0,0,1,1};
    std::vector<Rule> rules;
    struct AtlasGrid {
        std::string hash;
        unsigned width{}, height{};
        int columns = 8, rows = 8;
        std::vector<int> cells; // Row-major, origin at the visually upright top left.
        std::vector<std::array<float,4>> rectangles; // Normalized left, top, right, bottom.
        bool operator==(const AtlasGrid&) const = default;
    };
    bool atlas_selection = false; // Metadata only until region-aware rendering is implemented.
    std::vector<AtlasGrid> atlas_grids;
    std::vector<std::uint64_t> keys; // Capture provenance only; never runtime matching.
    bool operator==(const Profile&) const = default;
};
void Load(const std::string& directory);
std::vector<Profile> List();
std::vector<Profile> Panels();
bool ContextDetectedThisFrame(const std::string& id);
struct FadeProbe {
    unsigned draws = 0;
    std::array<float,7> minimum{}, maximum{}; // Six TEV constants, then blend constant.
};
void ObserveFade(const Rule& rule, const std::array<float,7>& alpha);
FadeProbe LastFadeProbe(const std::string& id);
float FadeOpacity(const Profile& profile);
void Update(const Profile& profile);
std::string Save(Profile profile);
bool Candidate(unsigned width, unsigned height, unsigned format);
void Observe(std::uint64_t key, const Rule& rule);
void SetDrawTexcoordBounds(float min_x, float min_y, float max_x, float max_y);
void ClearDrawTexcoordBounds();
std::vector<Profile> Match(const Rule& rule);
Profile Selection(const std::vector<std::uint64_t>& keys);
std::vector<std::uint64_t> CapturedKeys(const Profile& profile);
void Preview(Profile profile);
void StopPreview();
void HideSelection(const std::vector<std::uint64_t>& keys);
bool Hidden(const Rule& rule);
void ClearCaptured();
std::string Status();
bool Active(const std::string& id);
}
