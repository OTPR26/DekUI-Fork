// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <span>
#include "mm3d/overlay.h"
namespace Vulkan { class Scheduler; }
namespace MM3D::Textures {
constexpr unsigned Limit = 256;
struct Thumbnail {
    vk::Device device;
    vk::DeviceMemory memory{};
    vk::Image image{};
    vk::ImageView view{};
    std::uint64_t key{};
    unsigned width{}, height{};
    std::vector<std::uint8_t> alpha; // Detail-only, upright rows; never saved to disk.
    ~Thumbnail();
};
bool TrimVisible(std::span<const std::uint8_t> alpha, unsigned width, unsigned height,
                 std::array<float,4>& rectangle);
inline std::atomic<std::int64_t> capture_until{0}, hide_until{0};
void ToggleSelection(std::uint64_t key);
void SetSelection(const std::vector<std::uint64_t>& keys);
bool IsSelected(std::uint64_t key);
unsigned SelectionCount();
std::vector<std::uint64_t> SelectedKeys();
bool LoadProfile(const std::string& path);
void DisableProfile();
bool LoadProfileDefinition(const std::string& path);
void ObserveProfileKey(std::uint64_t key);
bool SavedScaleMatches(std::uint64_t key);
bool ContentCandidate(unsigned width, unsigned height, unsigned format);
std::string ContentHash(std::span<const std::uint8_t> bytes);
bool ContentMatches(unsigned width, unsigned height, unsigned format,
                    const std::string& hash, const std::string& context);
bool StartRecording(const std::string& path, const std::string& phase);
bool RecordingKey(std::uint64_t key);
bool NeedsRecordingHash(std::uint64_t key);
void RecordBinding(std::uint64_t key, std::span<const std::uint8_t> bytes,
                   const std::string& context);
std::string RecordingStatus();
std::string ProfileReport(const std::string& path);
inline std::atomic<unsigned> hidden{0};
// Session-only developer diagnostic for changing render-target contents.
inline std::atomic<bool> hide_by_address{false};
inline thread_local bool hide_current_draw = false;
inline thread_local bool scale_current_draw = false;
inline thread_local bool saved_scale_current_draw = false;
inline std::atomic<std::int64_t> scale_until{0};
inline std::atomic<bool> profile_active{false};
inline std::atomic<bool> profile_enabled{true};
inline std::atomic<unsigned> scaled_draws{0};
inline std::atomic<float> group_scale{0.5f}, group_x{0.f}, group_y{0.f};
// Saved guest-profile transforms must never read the picker's temporary controls.
inline std::atomic<float> saved_scale{1.f}, saved_x{0.f}, saved_y{0.f};
struct DrawTransform { float scale, x, y; };
inline DrawTransform CurrentDrawTransform() {
    // An explicit preview overrides the saved transform only on matching selected draws.
    if (scale_current_draw) return {group_scale.load(), group_x.load(), group_y.load()};
    return {saved_scale.load(), saved_x.load(), saved_y.load()};
}
std::uint64_t Key(unsigned address, unsigned width, unsigned height, unsigned format);
void Start();
bool Wants(std::uint64_t key);
void RequestDetail(std::uint64_t key, const std::string& hash);
bool WantsDetail(std::uint64_t key, const std::string& hash = {});
std::shared_ptr<Thumbnail> Detail();
void Capture(const Vulkan::Instance&, Vulkan::Scheduler&, vk::Image source, vk::Format format,
             unsigned width, unsigned height, std::uint64_t key,
             vk::ImageLayout source_layout = vk::ImageLayout::eGeneral, bool detail = false);
std::vector<std::shared_ptr<Thumbnail>> Snapshot();
void Clear();
void HideSelected(bool by_address = false);
void ScaleSelected();
void ApplyProfile();
bool ScaleMatches(std::uint64_t key);
void Restore();
bool Matches(std::uint64_t key);
}
