// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_visibility_depth.h"
#include <bit>
#include <cstring>
#include <vk_mem_alloc.h>
#include "mm3d/interpolation_performance.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

namespace Vulkan {
namespace {
constexpr u32 Capacity = 512 * 512 * 4;
using MM3D::InterpolationPerformance::RecordVisibilityEvent;
using MM3D::InterpolationPerformance::VisibilityEvent;
}
VisibilityDepthCache::VisibilityDepthCache(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}
VisibilityDepthCache::~VisibilityDepthCache() {
    // Snapshots have been submitted. Never destroy their buffers while a worker
    // or GPU still owns them, including shutdown with an unfinished snapshot.
    for (const auto& slot : slots) if (slot.tick) scheduler.Wait(slot.tick);
    scheduler.WaitWorker();
    for (const auto& slot : slots) if (slot.buffer)
        vmaDestroyBuffer(instance.GetAllocator(), slot.buffer, slot.allocation);
}
void VisibilityDepthCache::Reset() {
    if (batch) RecordVisibilityEvent(VisibilityEvent::HistoryReset);
    selected.reset();
    batch.reset();
    refreshed_frame.reset();
    for (auto& slot : slots) slot.usable = false;
    // Pending buffers remain reserved until completion; resetting is nonblocking.
}
void VisibilityDepthCache::Invalidate(u32 address, u32 size) {
    if (u64(address) < u64(source_address) + source_size && u64(source_address) < u64(address) + size) {
        RecordVisibilityEvent(VisibilityEvent::CpuInvalidation);
        Reset();
    }
}
bool VisibilityDepthCache::Allocate(Slot& slot) {
    if (slot.buffer) return true;
    const vk::BufferCreateInfo info{.size = Capacity, .usage = vk::BufferUsageFlagBits::eStorageBuffer};
    const auto raw_info = static_cast<VkBufferCreateInfo>(info);
    const VmaAllocationCreateInfo allocation_info{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer buffer{};
    VmaAllocationInfo mapped_info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &raw_info, &allocation_info, &buffer,
                        &slot.allocation, &mapped_info) != VK_SUCCESS) {
        failed = true;
        LOG_WARNING(Render_Vulkan, "Visibility depth snapshots unavailable; retaining exact reads");
        return false;
    }
    slot.buffer = buffer;
    slot.mapped = mapped_info.pMappedData;
    return true;
}
bool VisibilityDepthCache::Capture(TextureRuntime& runtime, Surface& surface,
    const MM3D::VisibilityDepth::Context& context, u64 now_ns) {
    const auto id = surface.mm3d_visibility_id;
    const auto modification = surface.ModificationTick();
    for (const auto& slot : slots) {
        if (slot.usable && slot.surface_id == id && slot.modification == modification &&
            slot.context.frame == context.frame &&
            MM3D::VisibilityDepth::Fresh(slot.context, context, slot.captured_ns, now_ns)) return false;
    }
    // Never replace a selected batch's image, or a GPU-owned pending buffer.
    for (unsigned i = 0; i < slots.size(); ++i) {
        auto& slot = slots[i];
        if (selected == i || (slot.tick && !scheduler.IsFree(slot.tick))) continue;
        if (!Allocate(slot)) {
            RecordVisibilityEvent(VisibilityEvent::AllocationFailure);
            return false;
        }
        slot.usable = true;
        slot.completed = false;
        slot.surface_id = id;
        slot.modification = modification;
        slot.captured_ns = now_ns;
        slot.context = context;
        slot.invalid_regions = surface.invalid_regions;
        runtime.QueueVisibilityDepth(surface, slot.buffer);
        slot.tick = scheduler.CurrentTick();
        scheduler.Flush(); // Submit without waiting for GPU completion.
        MM3D::InterpolationPerformance::RecordVisibilityDepth(false, false, true);
        return true;
    }
    RecordVisibilityEvent(VisibilityEvent::RingFull);
    return false;
}

bool VisibilityDepthCache::Refresh(TextureRuntime& runtime, Surface& surface,
    const MM3D::VisibilityDepth::Context& context, u64 now_ns) {
    if (!batch || failed) return false;
    if (!MM3D::VisibilityDepth::SameScene(*batch, context) || context.frame < batch->frame ||
        (refreshed_frame && context.frame < *refreshed_frame) ||
        surface.mm3d_visibility_id != batch_surface || surface.addr != source_address ||
        surface.size != source_size) {
        RecordVisibilityEvent(VisibilityEvent::RefreshInvalidated);
        Reset();
        return false;
    }
    if (refreshed_frame == context.frame) return false;
    refreshed_frame = context.frame;
    // A paused/unchanged surface must not be relabelled as a newer game frame.
    // A foreground capture of these same contents also makes a refresh redundant.
    for (const auto& slot : slots) {
        if (slot.usable && slot.surface_id == batch_surface &&
            slot.modification == surface.ModificationTick()) return false;
    }
    scheduler.GetMasterSemaphore()->Refresh();
    if (!Capture(runtime, surface, context, now_ns)) return false;
    RecordVisibilityEvent(VisibilityEvent::RefreshQueued);
    return true;
}

std::optional<u32> VisibilityDepthCache::Read(TextureRuntime& runtime, Surface& surface,
    u32 address, const MM3D::VisibilityDepth::Context& context, u64 now_ns) {
    if (failed || surface.pixel_format != VideoCore::PixelFormat::D24S8 || !surface.is_tiled ||
        surface.levels != 1 || surface.width != surface.stride || surface.size > Capacity ||
        address < surface.addr || address >= surface.end || surface.IsCustom()) {
        RecordVisibilityEvent(VisibilityEvent::UnsupportedSurface);
        MM3D::InterpolationPerformance::RecordVisibilityDepth(true, false, false);
        Reset();
        return {};
    }
    if (!surface.IsRegionValid({address, address + 4})) {
        RecordVisibilityEvent(VisibilityEvent::InvalidCurrentPixel);
        MM3D::InterpolationPerformance::RecordVisibilityDepth(true, false, false);
        Reset();
        return {};
    }
    const auto index = MM3D::VisibilityDepth::LinearIndex(address - surface.addr, surface.width, surface.height);
    if (!index) {
        RecordVisibilityEvent(VisibilityEvent::UnsupportedSurface);
        MM3D::InterpolationPerformance::RecordVisibilityDepth(true, false, false);
        Reset();
        return {};
    }
    const auto id = surface.mm3d_visibility_id;
    const auto modification = surface.ModificationTick();
    if (!batch || *batch != context || batch_surface != id || batch_modification != modification) {
        if (batch) {
            const bool scene_changed = !MM3D::VisibilityDepth::SameScene(*batch, context);
            const bool surface_changed = batch_surface != id;
            const bool frame_rollback = context.frame < batch->frame;
            if (scene_changed) RecordVisibilityEvent(VisibilityEvent::SceneReset);
            if (surface_changed) RecordVisibilityEvent(VisibilityEvent::SurfaceReset);
            if (frame_rollback) RecordVisibilityEvent(VisibilityEvent::FrameRollbackReset);
            if (scene_changed || surface_changed || frame_rollback) Reset();
        }
        batch = context;
        source_address = surface.addr;
        source_size = surface.size;
        batch_surface = id;
        batch_modification = modification;
        selected.reset();
        scheduler.GetMasterSemaphore()->Refresh();
        for (unsigned i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (!slot.tick) continue;
            if (!scheduler.IsFree(slot.tick)) {
                RecordVisibilityEvent(VisibilityEvent::PendingCandidate);
                continue;
            }
            if (!slot.completed) {
                if (vmaInvalidateAllocation(instance.GetAllocator(), slot.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
                    slot.usable = false;
                    RecordVisibilityEvent(VisibilityEvent::HostInvalidateFailure);
                }
                slot.completed = true;
            }
            if (!slot.usable) {
                RecordVisibilityEvent(VisibilityEvent::DiscardedCandidate);
                continue;
            }
            if (slot.surface_id != id) {
                RecordVisibilityEvent(VisibilityEvent::SurfaceCandidate);
                continue;
            }
            const auto freshness = MM3D::VisibilityDepth::CheckFreshness(slot.context, context, slot.captured_ns, now_ns);
            if (freshness != MM3D::VisibilityDepth::Freshness::Fresh) {
                using MM3D::VisibilityDepth::Freshness;
                const auto reason = freshness == Freshness::Scene ? VisibilityEvent::SceneCandidate :
                    freshness == Freshness::FrameAge ? VisibilityEvent::FrameAgeCandidate : VisibilityEvent::WallAgeCandidate;
                RecordVisibilityEvent(reason, now_ns >= slot.captured_ns ? now_ns - slot.captured_ns : 0,
                                      context.frame >= slot.context.frame ? context.frame - slot.context.frame : 0);
                continue;
            }
            // A same-frame write after capture is a resource boundary, not a
            // license to return arbitrarily older data from that same frame.
            if (slot.context.frame == context.frame && slot.modification != modification) {
                RecordVisibilityEvent(VisibilityEvent::SameFrameWriteCandidate);
                continue;
            }
            if (!selected || slot.tick > slots[*selected].tick) selected = i;
        }
        Capture(runtime, surface, context, now_ns);
    }
    // Keep all 3x3/eight-entry reads within this batch on the same snapshot.
    const bool invalid_pixel = selected && slots[*selected].invalid_regions.find(VideoCore::SurfaceInterval{address, address + 4}) != slots[*selected].invalid_regions.end();
    if (!selected || invalid_pixel || !MM3D::VisibilityDepth::Fresh(slots[*selected].context, context,
                                                                 slots[*selected].captured_ns, now_ns)) {
        RecordVisibilityEvent(!selected ? VisibilityEvent::NoSnapshot : invalid_pixel ?
                              VisibilityEvent::InvalidCachedPixel : VisibilityEvent::SelectedExpired);
        MM3D::InterpolationPerformance::RecordVisibilityDepth(true, false, false);
        return {};
    }
    u32 word{};
    std::memcpy(&word, static_cast<const u8*>(slots[*selected].mapped) + *index * 4, 4);
    MM3D::InterpolationPerformance::RecordVisibilityDepth(true, true, false);
    RecordVisibilityEvent(VisibilityEvent::CachedAge, now_ns - slots[*selected].captured_ns,
                          context.frame - slots[*selected].context.frame);
    return std::rotr(word, 8); // Same D24S8 packing as EncodeTexture.
}
} // namespace Vulkan
