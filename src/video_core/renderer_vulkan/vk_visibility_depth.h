// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <optional>
#include "mm3d/visibility_depth.h"
#include "video_core/rasterizer_cache/surface_base.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocation)
namespace Vulkan {
class Instance;
class Scheduler;
class TextureRuntime;
class Surface;

// Private, bounded snapshots for the verified MM3D visibility consumer only.
// Dirty guest-memory ownership and ordinary Surface::Download remain exact.
class VisibilityDepthCache {
public:
    VisibilityDepthCache(const Instance& instance, Scheduler& scheduler);
    ~VisibilityDepthCache();
    std::optional<u32> Read(TextureRuntime& runtime, Surface& surface, u32 address,
                            const MM3D::VisibilityDepth::Context& context, u64 now_ns);
    // Only armed by a successful surface lookup for the verified depth reader.
    std::optional<u32> SourceAddress() const {
        return batch ? std::optional<u32>{source_address} : std::nullopt;
    }
    bool Refresh(TextureRuntime& runtime, Surface& surface,
                 const MM3D::VisibilityDepth::Context& context, u64 now_ns);
    void Reset();
    void Invalidate(u32 address, u32 size);
private:
    struct Slot {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        void* mapped{};
        u64 tick{}, surface_id{}, modification{}, captured_ns{};
        MM3D::VisibilityDepth::Context context{};
        VideoCore::SurfaceRegions invalid_regions;
        bool usable{}, completed{};
    };
    bool Allocate(Slot& slot);
    bool Capture(TextureRuntime& runtime, Surface& surface,
                 const MM3D::VisibilityDepth::Context& context, u64 now_ns);
    const Instance& instance;
    Scheduler& scheduler;
    std::array<Slot, 3> slots{};
    std::optional<unsigned> selected;
    std::optional<MM3D::VisibilityDepth::Context> batch;
    u64 batch_surface{}, batch_modification{};
    u32 source_address{}, source_size{};
    std::optional<u64> refreshed_frame;
    bool failed{};
};
} // namespace Vulkan
