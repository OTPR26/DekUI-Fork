// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "mm3d/visibility_depth.h"
#include "video_core/rasterizer_cache/utils.h"
#include "video_core/rasterizer_cache/pixel_format.h"
#include "video_core/renderer_vulkan/vk_descriptor_update_queue.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"
#include "video_core/renderer_vulkan/vk_visibility_depth.h"

namespace MM3D {
static void RequireDepth(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void RunVisibilityDepthFixture(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    Vulkan::RenderManager manager(instance, scheduler);
    Vulkan::DescriptorUpdateQueue updates(instance);
    Vulkan::TextureRuntime runtime(instance, scheduler, manager, updates, 3);
    scheduler.RegisterOnSubmit([&] { manager.EndRendering(); });
    scheduler.RegisterOnDispatch([&] { updates.Flush(); });
    for (const u32 scale : {1u, 2u, 4u}) {
        VideoCore::SurfaceParams params;
        params.addr = 0x18537fc0;
        params.width = params.stride = 512;
        params.height = 400;
        params.res_scale = scale;
        params.is_tiled = true;
        params.pixel_format = VideoCore::PixelFormat::D24S8;
        params.UpdateParams();
        Vulkan::Surface surface(runtime, params);
        Vulkan::VisibilityDepthCache cache(instance, scheduler);
        const auto paint = [&](bool version) {
            VideoCore::TextureClear clear{};
            clear.texture_rect = surface.GetScaledRect();
            clear.value.depth = version ? 1.f : 0.f;
            clear.value.stencil = version ? 17 : 93;
            runtime.ClearTexture(surface, clear);
            clear.texture_rect = Common::Rectangle<u32>{16, 32, 24, 24} * scale;
            clear.value.depth = version ? 0.f : 1.f;
            clear.value.stencil = version ? 37 : 173;
            runtime.ClearTexture(surface, clear);
            surface.MarkValid(surface.GetInterval());
        };
        const auto exact = [&] {
            const auto staging = runtime.FindStaging(surface.width * surface.height * 4, false);
            surface.Download({.buffer_offset = staging.offset, .buffer_size = staging.size,
                .texture_rect = surface.GetRect(), .texture_level = 0}, staging);
            std::vector<u8> bytes(surface.size);
            VideoCore::EncodeTexture(surface, surface.addr, surface.end, staging.mapped, bytes,
                                     runtime.NeedsConversion(surface));
            return bytes;
        };
        const auto word = [](const std::vector<u8>& data, u32 offset) {
            u32 value{}; std::memcpy(&value, data.data() + offset, 4); return value;
        };
        VisibilityDepth::Context context{10, 1, 1, 0x123000, 5};
        constexpr u64 first_time = 1'000'000'000;
        paint(false);
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time), "first use falls back exactly");
        scheduler.Finish();
        ++context.batch;
        const auto original = exact();
        for (u32 offset = 0; offset < surface.size; offset += 4)
            RequireDepth(cache.Read(runtime, surface, surface.addr + offset, context, first_time + 1) == word(original, offset),
                         "every cached pixel must match exact tiled D24S8, including nonuniform depth/stencil");

        // Prove cached reads return while the actual GPU is blocked. The gate
        // also proves we never read the mapped pending snapshot or reuse its buffer.
        RequireDepth(instance.IsTimelineSemaphoreSupported(), "visibility fixture needs timeline semaphore");
        const auto device = instance.GetDevice();
        const vk::SemaphoreTypeCreateInfo type{.semaphoreType = vk::SemaphoreType::eTimeline};
        auto gate = device.createSemaphoreUnique({.pNext = &type});
        std::atomic<bool> released{};
        const auto release = [&] {
            if (!released.exchange(true)) device.signalSemaphoreKHR({.semaphore = *gate, .value = 1});
        };
        std::jthread watchdog([&] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); release(); });
        scheduler.Flush(nullptr, *gate);
        paint(true);
        ++context.frame; ++context.batch;
        RequireDepth(cache.Refresh(runtime, surface, context, first_time + 57'000'000),
                     "background refresh queues while GPU is blocked");
        const auto cached = cache.Read(runtime, surface, surface.addr, context, first_time + 58'000'000);
        const bool nonblocking = !released.load();
        RequireDepth(cached == word(original, 0) && nonblocking, "cached depth must return before blocked GPU is released");
        RequireDepth(cache.Read(runtime, surface, surface.end - 4, context, first_time + 59'000'000) == word(original, surface.size - 4),
                     "all samples in one query use the same completed snapshot");
        // Frame lag and wall-clock lag both cause an exact fallback, never an
        // unbounded wait inside this cache. Normal caller performs the exact read.
        ++context.frame; ++context.batch;
        RequireDepth(!cache.Refresh(runtime, surface, context, first_time + 60'000'000),
                     "unchanged pending contents are not refreshed or relabelled");
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 60'000'000), "two-frame-old cache rejected");
        ++context.frame; ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 65'000'000), "full pending ring falls back without recycling GPU-owned buffers");
        ++context.frame; ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 65'500'000), "all three buffers remain GPU-owned");
        RequireDepth(!released.load(), "even missed/overdue cache lookup must not wait for GPU");
        release();
        scheduler.Finish();
        ++context.batch;
        const auto current = exact();
        RequireDepth(word(current, 0) != word(original, 0), "ordinary downloads still read current data");
        RequireDepth(cache.Read(runtime, surface, surface.addr, context, first_time + 66'000'000) == word(current, 0), "newest completed capture becomes available");
        ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 200'000'000), "paused stale snapshot rejected");
        scheduler.Finish();
        ++context.batch;
        RequireDepth(cache.Read(runtime, surface, surface.addr, context, first_time + 200'000'001) == word(current, 0),
                     "expired same-frame capture is replaced and recovers after pause");
        ++context.scene; ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 201'000'000), "new scene never inherits completed old snapshot");
        scheduler.Finish();
        ++context.batch;
        RequireDepth(cache.Read(runtime, surface, surface.addr, context, first_time + 202'000'000).has_value(), "new scene warms up");
        cache.Invalidate(surface.addr, 4);
        ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 203'000'000), "CPU writes invalidate historical depth");
        scheduler.Finish();
        Vulkan::Surface replacement(runtime, params);
        const VideoCore::TextureClear clear{.texture_rect = replacement.GetScaledRect()};
        runtime.ClearTexture(replacement, clear);
        ++context.batch;
        RequireDepth(!cache.Read(runtime, replacement, replacement.addr, context, first_time + 204'000'000), "recreated surface never inherits old snapshots");
        scheduler.Finish();
        cache.Reset();
        surface.MarkInvalid({surface.addr, surface.addr + 256});
        ++context.frame; ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr + 256, context, first_time + 205'000'000), "partial snapshot starts cold");
        scheduler.Finish();
        surface.MarkValid({surface.addr, surface.addr + 256});
        ++context.frame; ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 206'000'000), "previously invalid pixels never serve cached values");
        scheduler.Finish();

        // Reproduce the Deck's 57-58ms frame spacing with a deterministic clock.
        // Complete each previous submission, then change depth before the next
        // request: a hit must return the PREVIOUS frame, not the new contents.
        for (const u64 cadence : {57'000'000ULL, 58'000'000ULL, 60'000'000ULL}) {
            cache.Reset();
            context.frame += 10;
            ++context.batch;
            u64 now = first_time + 1'000'000'000;
            paint(false);
            RequireDepth(!cache.Read(runtime, surface, surface.addr, context, now),
                         "slow cadence starts with one exact fallback");
            scheduler.Finish();
            for (unsigned frame = 1; frame <= 12; ++frame) {
                now += cadence;
                ++context.frame;
                ++context.batch;
                paint(frame % 2 != 0);
                const auto previous_word = word(frame % 2 ? original : current, 0);
                RequireDepth(cache.Read(runtime, surface, surface.addr, context, now) == previous_word,
                             "completed previous-frame depth must remain usable at 57-60ms cadence");
                scheduler.Finish();
            }
        }
        // A modification within the same guest frame must still reject history.
        cache.Reset();
        context.frame += 10;
        ++context.batch;
        paint(false);
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 3'000'000'000),
                     "same-frame modification test starts cold");
        scheduler.Finish();
        paint(true);
        ++context.batch;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, first_time + 3'000'000'001),
                     "same-frame modification rejects earlier same-frame capture");
        scheduler.Finish();

        // Reproduce long gaps between depth queries while the scene keeps drawing.
        // A separate on-demand cache demonstrates the old cold miss at each gap;
        // the refreshed cache must return the previous frame's changing contents.
        cache.Reset();
        Vulkan::VisibilityDepthCache on_demand(instance, scheduler);
        context.frame += 10;
        ++context.batch;
        u64 refresh_time = first_time + 5'000'000'000;
        paint(false);
        RequireDepth(!cache.Refresh(runtime, surface, context, refresh_time),
                     "background refresh cannot arm an unqueried source");
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, refresh_time), "refresh test cold start");
        RequireDepth(!on_demand.Read(runtime, surface, surface.addr, context, refresh_time), "control cache cold start");
        scheduler.Finish();
        unsigned refreshed = 0;
        for (unsigned frame = 1; frame <= 90; ++frame) {
            refresh_time += 33'333'333;
            ++context.frame;
            ++context.batch;
            paint(frame % 2 != 0);
            if (frame == 45 || frame == 90) {
                RequireDepth(!on_demand.Read(runtime, surface, surface.addr, context, refresh_time),
                             "on-demand control misses after a long gap without queries");
                RequireDepth(cache.Read(runtime, surface, surface.addr, context, refresh_time) ==
                                 word(frame % 2 ? original : current, 0),
                             "refresh supplies previous-frame depth after a long query gap");
            }
            refreshed += cache.Refresh(runtime, surface, context, refresh_time);
            RequireDepth(!cache.Refresh(runtime, surface, context, refresh_time + 1),
                         "refresh is bounded to once per guest frame");
            scheduler.Finish();
        }
        RequireDepth(refreshed == 88, "queried frames do not pay for a duplicate background snapshot");
        ++context.frame; ++context.batch;
        refresh_time += 33'333'333;
        RequireDepth(!cache.Refresh(runtime, surface, context, refresh_time),
                     "unchanged surface is not relabelled as a new frame");
        ++context.scene;
        paint(true);
        RequireDepth(!cache.Refresh(runtime, surface, context, refresh_time + 1) && !cache.SourceAddress(),
                     "scene changes disarm background refresh until a verified query");
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, refresh_time + 2), "new scene starts cold");
        scheduler.Finish();
        cache.Invalidate(surface.addr, 4);
        RequireDepth(!cache.SourceAddress() && !cache.Refresh(runtime, surface, context, refresh_time + 3),
                     "CPU invalidation disarms refresh");

        // A small local cost comparison, not a gameplay FPS estimate. Both paths
        // perform the same changing depth clears and wait outside Refresh.
        paint(false);
        ++context.frame; ++context.batch;
        refresh_time += 33'333'333;
        RequireDepth(!cache.Read(runtime, surface, surface.addr, context, refresh_time), "cost test arms source");
        scheduler.Finish();
        std::array<double, 2> totals{};
        for (unsigned sample = 0; sample < 32; ++sample) {
            for (unsigned mode : {sample % 2, 1 - sample % 2}) {
                ++context.frame; ++context.batch;
                refresh_time += 33'333'333;
                const auto start = std::chrono::steady_clock::now();
                paint(sample % 2 != 0);
                if (mode) RequireDepth(cache.Refresh(runtime, surface, context, refresh_time), "changed depth is refreshed");
                scheduler.Finish();
                totals[mode] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
        }
        std::cout << "Visibility refresh cost: scale=" << scale
                  << " baseline_mean_ms=" << totals[0] / 32
                  << " refresh_mean_ms=" << totals[1] / 32
                  << "; synthetic depth clears plus completion, not gameplay FPS\n";
        std::cout << "PASS visibility depth: scale=" << scale
                  << "; all pixels, blocked GPU, bounded age/ring, scene/surface/CPU invalidation, exact download, 57-60ms cadence, sparse-query refresh\n";
    }
    scheduler.Finish(); scheduler.WaitWorker();
    scheduler.RegisterOnSubmit([] {});
    scheduler.RegisterOnDispatch([] {});
}
} // namespace MM3D
