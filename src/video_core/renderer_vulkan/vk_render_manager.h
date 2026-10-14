// Copyright 2024-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <mutex>
#include <span>
#include <vector>

#include "common/math_util.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {
enum class PixelFormat : u32;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Framebuffer;

struct RenderPass {
    vk::Framebuffer framebuffer;
    vk::RenderPass render_pass;
    vk::Rect2D render_area;
    vk::ClearValue clear;
    u32 do_clear;

    bool operator==(const RenderPass& other) const noexcept {
        return std::tie(framebuffer, render_pass, render_area, do_clear) ==
                   std::tie(other.framebuffer, other.render_pass, other.render_area,
                            other.do_clear) &&
               std::memcmp(&clear, &other.clear, sizeof(vk::ClearValue)) == 0;
    }
};

class RenderManager {
    static constexpr u32 NumColorFormats = static_cast<u32>(VideoCore::PixelFormat::NumColorFormat);
    static constexpr u32 NumDepthFormats = static_cast<u32>(VideoCore::PixelFormat::NumDepthFormat);

public:
    explicit RenderManager(const Instance& instance, Scheduler& scheduler);
    ~RenderManager();

    /// Begins a new renderpass with the provided framebuffer as render target.
    void BeginRendering(const Framebuffer* framebuffer, Common::Rectangle<u32> draw_rect);

    /// Begins a new renderpass with the provided render state.
    void BeginRendering(const RenderPass& new_pass);

    /// Exits from any currently active renderpass instance
    void EndRendering();

    class ReplayScope {
    public:
        // Nonempty writes certifies this capture only writes those private
        // attachments. Unknown captures retain full ordering at readbacks.
        ReplayScope(RenderManager& manager_, bool eligible,
                    std::span<const vk::Image> writes = {});
        ~ReplayScope();
        ReplayScope(const ReplayScope&) = delete;
        ReplayScope& operator=(const ReplayScope&) = delete;
    private:
        RenderManager* manager{};
    };
    bool MatchesFramebuffer(vk::Framebuffer framebuffer, u32 width, u32 height) const {
        return pass.framebuffer == framebuffer && pass.render_pass &&
               pass.render_area == vk::Rect2D{{0, 0}, {width, height}};
    }
    u64 PassesStarted() const { return passes_started; }
    bool CanReadbackBeforeReplay(vk::Image source) const;

    /// Returns the renderpass associated with the color-depth format pair
    vk::RenderPass GetRenderpass(VideoCore::PixelFormat color, VideoCore::PixelFormat depth,
                                 bool is_clear);

private:
    void SwapReplayState();
    void DrainReplay();
    /// Creates a renderpass configured appropriately and stores it in cached_renderpasses
    vk::UniqueRenderPass CreateRenderPass(vk::Format color, vk::Format depth,
                                          vk::AttachmentLoadOp load_op) const;

private:
    const Instance& instance;
    Scheduler& scheduler;
    vk::UniqueRenderPass cached_renderpasses[NumColorFormats + 1][NumDepthFormats + 1][2];
    std::mutex cache_mutex;
    std::array<vk::Image, 2> images;
    std::array<vk::ImageAspectFlags, 2> aspects;
    bool shadow_rendering{};
    RenderPass pass{};
    u32 num_draws{};
    std::array<vk::Image, 2> replay_images{};
    std::array<vk::ImageAspectFlags, 2> replay_aspects{};
    bool replay_shadow_rendering{};
    RenderPass replay_pass{};
    u32 replay_num_draws{};
    u64 passes_started{};
    std::vector<vk::Image> replay_writes;
    bool replay_writes_known{};
};

} // namespace Vulkan
