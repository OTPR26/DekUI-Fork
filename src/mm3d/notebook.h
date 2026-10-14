// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include "mm3d/profiles.h"

namespace MM3D::Notebook {

// Manual, session-only composed-frame preview; no claim of menu detection.
inline std::atomic<bool> detail_preview{false};
inline std::atomic<float> detail_scale{0.8f}, detail_x{0.1f}, detail_y{0.52f};
inline std::atomic<int> detail_top{86}, detail_bottom{204};
inline std::atomic<bool> transparent_copy{false};
inline std::atomic<unsigned> excluded_draws{0};
inline std::mutex exclusions_mutex;
inline std::vector<Profiles::Rule> exclusions;
inline bool Excluded(const Profiles::Rule& rule) {
    if (!transparent_copy.load()) return false;
    std::lock_guard lock(exclusions_mutex);
    for (const auto& r : exclusions)
        if (r == rule) return true;
    return false;
}
inline bool ExclusionCandidate(unsigned w, unsigned h, unsigned f) {
    if (!transparent_copy.load()) return false;
    std::lock_guard lock(exclusions_mutex);
    for (const auto& r : exclusions)
        if (r.width == w && r.height == h && r.format == f) return true;
    return false;
}

// This is deliberately separate from texture/profile state. The current emulator
// has no guest UI-resource callback, so this first proof of concept patches a
// copied GAR for an external override test.
struct Settings {
    bool enabled = true;
    float events_scale = .70f;
    float events_x = 0.f; // Native MFL layout units; the exact scale is title-specific.
    float events_y = 0.f;
    std::string source_archive;
    std::string output_archive;
    float fade_width = 426.666667f;
    std::string fade_source;
    float telop_x = 40.f;
    float telop_scale = 1.f;
    float telop_text_scale = 1.f;
    float telop_y = 0.f;
    float telop_text_y = 0.f;
    float telop_brightness = 1.f;
};

struct PatchResult {
    bool ok = false;
    std::string message;
};

PatchResult PatchEventsArchive(const std::string& source, const std::string& output,
                               float x, float y, float scale);
PatchResult InstallEventsOverride(const std::string& source, float x, float y, float scale);
bool RemoveEventsOverride();
std::string EventsOverridePath();
std::string DefaultSourceArchive();
bool Load(Settings& settings, const std::string& path);
bool Save(const Settings& settings, const std::string& path);
PatchResult PatchFadeArchive(const std::string& source,const std::string& output,float width);
PatchResult InstallFadeOverride(const std::string& source,float width);
PatchResult RemoveFadeOverride();
std::string FadeOverridePath();
std::string DefaultFadeArchive();
PatchResult PatchTelopArchive(const std::string& source,const std::string& output,float x,float scale=1.f,float y=0.f,float text_scale=1.f,float text_y=0.f,float brightness=1.f);
PatchResult InstallTelopOverride(float x,float scale=1.f,float y=0.f,float text_scale=1.f,float text_y=0.f,float brightness=1.f);
PatchResult RemoveTelopOverride();

} // namespace MM3D::Notebook
