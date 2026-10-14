// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdlib>
#include <limits>
#include <mutex>
#include <string_view>
#ifdef ENABLE_MM3D_PRESENTATION
#include "mm3d/interpolation_performance.h"
#endif
#include <utility>
#include "common/microprofile.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#ifdef HAVE_LIBRETRO
#include "citra_libretro/libretro_vk.h"
#endif

MICROPROFILE_DEFINE(Vulkan_WaitForWorker, "Vulkan", "Wait for worker", MP_RGB(255, 192, 192));
MICROPROFILE_DEFINE(Vulkan_Submit, "Vulkan", "Submit Exectution", MP_RGB(255, 192, 255));

namespace Vulkan {

namespace {

std::unique_ptr<MasterSemaphore> MakeMasterSemaphore(const Instance& instance) {
#ifdef HAVE_LIBRETRO
    return CreateLibRetroMasterSemaphore(instance);
#else
    if (instance.IsTimelineSemaphoreSupported()) {
        return std::make_unique<MasterSemaphoreTimeline>(instance);
    } else {
        return std::make_unique<MasterSemaphoreFence>(instance);
    }
#endif
}

} // Anonymous namespace

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Instance& instance)
    : device{instance.GetDevice()}, graphics_queue{instance.GetGraphicsQueue()},
      master_semaphore{MakeMasterSemaphore(instance)},
      command_pool{instance, master_semaphore.get()}, use_worker_thread{true} {
#ifdef ENABLE_MM3D_PRESENTATION
    const char* replay_option = std::getenv("DEKUI_BATCH_INTERPOLATION");
    // Batch interpolation by default; retain an explicit diagnostic opt-out.
    replay_batching_enabled = !replay_option || std::string_view{replay_option} != "0";
    LOG_INFO(Render_Vulkan, "Guarded interpolation batching enabled={}", replay_batching_enabled);
#ifndef HAVE_LIBRETRO
    const char* readback_option = std::getenv("DEKUI_DEPTH_READBACK_PRIORITY");
    readback_priority_enabled = !readback_option || std::string_view{readback_option} != "0";
#endif
    LOG_INFO(Render_Vulkan, "Native depth readback priority enabled={}", readback_priority_enabled);
    timing_device=instance.GetDevice();
    const auto physical=instance.GetPhysicalDevice();
    timing_bits=physical.getQueueFamilyProperties()[instance.GetGraphicsQueueFamilyIndex()].timestampValidBits;
    timing_period=physical.getProperties().limits.timestampPeriod;
    if(timing_bits && timing_period>0) {
        try {
            timing_pool=timing_device.createQueryPoolUnique(vk::QueryPoolCreateInfo{
                .queryType=vk::QueryType::eTimestamp,.queryCount=128});
        } catch(const vk::SystemError& error) {
            LOG_WARNING(Render_Vulkan,"GPU performance timestamps unavailable: {}",error.what());
        }
    }
    LOG_INFO(Render_Vulkan,"Interpolation GPU timing support: valid_bits={} period_ns={}",timing_bits,timing_period);
#endif
    AllocateWorkerCommandBuffers();
    if (use_worker_thread) {
        AcquireNewChunk();
        worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
    }
}

Scheduler::~Scheduler() = default;

Scheduler::NativeReadbackScope::NativeReadbackScope(Scheduler& owner, bool independent) {
    if (independent && owner.readback_priority_enabled && owner.HasReplay() &&
        !owner.capture_replay && !owner.drain_replay && !owner.native_draw_depth &&
        !owner.native_readback) {
        scheduler = &owner;
        scheduler->native_readback = true;
    }
#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::InterpolationPerformance::RecordReadbackPriority(
        owner.HasReplay(), scheduler != nullptr, owner.queued_replay_scopes);
#endif
}

Scheduler::NativeReadbackScope::~NativeReadbackScope() {
    if (scheduler) scheduler->native_readback = false;
}

void Scheduler::SetReplayBatchingEnabled(bool enabled) {
    FlushReplay();
    replay_batching_enabled = enabled;
}

void Scheduler::BeginReplayCapture() {
    ASSERT(!capture_replay);
    capture_replay = true;
    if (!drain_replay) ++queued_replay_scopes;
    state = StateFlags::AllDirty;
}

void Scheduler::EndReplayCapture() {
    ASSERT(capture_replay);
    capture_replay = false;
    state = StateFlags::AllDirty;
}

void Scheduler::FlushReplay() {
    if (drain_replay || !HasReplay()) return;
    ASSERT_MSG(!native_readback, "Resource mutation/submission inside native readback");
    // Capture scopes contain only immutable draw state, not resource allocation,
    // stream-buffer mapping, readbacks or submission. Those happen beforehand.
    ASSERT_MSG(!capture_replay, "Resource operation inside deferred replay capture");
    drain_replay = true;
    ASSERT(on_replay_drain);
    on_replay_drain();
    if (replay_chunk) replay_chunks.push_back(std::move(replay_chunk));
    auto commands = std::move(replay_chunks);
    replay_chunks.clear();
    Record([commands = std::move(commands)](vk::CommandBuffer cmdbuf) {
        for (const auto& part : commands) part->ExecuteAll(cmdbuf);
    });
#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::InterpolationPerformance::RecordReplayBatch(queued_replay_scopes);
#endif
    queued_replay_scopes = 0;
    state = StateFlags::AllDirty;
    drain_replay = false;
}

void Scheduler::Flush(vk::Semaphore signal, vk::Semaphore wait) {
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    SubmitExecution(signal, wait);
}

void Scheduler::FlushSkippedFrame() {
    // Limit run-ahead if there are no presentations to provide backpressure.
    // Wait only for the oldest skipped submission, not the just-rendered scene.
    auto& oldest = skipped_frame_ticks[skipped_frame_slot];
    if (oldest) Wait(oldest);
    oldest = CurrentTick();
    Flush();
    skipped_frame_slot = (skipped_frame_slot + 1) % skipped_frame_ticks.size();
}

void Scheduler::Finish(vk::Semaphore signal, vk::Semaphore wait, std::source_location caller) {
    if (native_readback) {
        ASSERT(!signal && !wait);
        FinishNativeReadback(caller);
        return;
    }
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
#ifdef ENABLE_MM3D_PRESENTATION
    const auto trace = MM3D::InterpolationPerformance::BeginFinish(this, presubmit_tick, caller);
    SubmitExecution(signal, wait, trace);
    Wait(presubmit_tick, caller);
    if (trace) trace->completed = MM3D::InterpolationPerformance::Now();
#else
    SubmitExecution(signal, wait);
    Wait(presubmit_tick, caller);
#endif
}

void Scheduler::FinishNativeReadback([[maybe_unused]] std::source_location caller) {
    ASSERT(native_readback && !capture_replay && !drain_replay);
    if (!readback_fence) readback_fence = device.createFenceUnique({});
    device.resetFences(*readback_fence);
    state = StateFlags::AllDirty;
    // Close only the native pass. Replay commands (including their open pass)
    // remain captured, in the SAME unretired resource tick.
    on_submit();
#ifdef ENABLE_MM3D_PRESENTATION
    const auto trace = MM3D::InterpolationPerformance::BeginFinish(this, CurrentTick(), caller);
    MM3D::InterpolationPerformance::WaitSiteScope wait_site{caller};
    MM3D::InterpolationPerformance::Scope wait_timing{MM3D::InterpolationPerformance::Part::GpuWait};
    const auto tick = CurrentTick();
    const auto phase = timing_phase;
#endif
    Record([this
#ifdef ENABLE_MM3D_PRESENTATION
            , trace, tick, phase
#endif
           ](vk::CommandBuffer cmdbuf) {
#ifdef ENABLE_MM3D_PRESENTATION
        if (trace) trace->worker = MM3D::InterpolationPerformance::Now();
#endif
        std::scoped_lock lock{submit_mutex};
#ifdef ENABLE_MM3D_PRESENTATION
        if (trace) trace->locked = MM3D::InterpolationPerformance::Now();
        if (timing_slot >= 0) {
            cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *timing_pool,
                                  static_cast<u32>(timing_slot) * 2 + 1);
            timing_slots[timing_slot].phase = phase;
            timing_slots[timing_slot].tick = tick;
            timing_slots[timing_slot].trace = trace;
        }
        MM3D::InterpolationPerformance::Scope timing{MM3D::InterpolationPerformance::Part::Submit};
#endif
        cmdbuf.end();
        // No master-semaphore signal: it would prematurely release resources
        // referenced by replay. A dedicated fence covers just this readback.
        graphics_queue.submit(vk::SubmitInfo{.commandBufferCount = 1,
                                            .pCommandBuffers = &cmdbuf}, *readback_fence);
#ifdef ENABLE_MM3D_PRESENTATION
        if (trace) trace->submitted = MM3D::InterpolationPerformance::Now();
#endif
    });
    if (use_worker_thread) {
        chunk->MarkSubmit();
        WaitWorkerCommands();
    } else {
        AllocateWorkerCommandBuffers();
    }
    while (device.waitForFences(*readback_fence, true, std::numeric_limits<u64>::max()) !=
           vk::Result::eSuccess) {}
#ifdef ENABLE_MM3D_PRESENTATION
    if (trace) trace->completed = MM3D::InterpolationPerformance::Now();
#endif
}

void Scheduler::WaitWorker() {
    FlushReplay();
    WaitWorkerCommands();
}

void Scheduler::WaitWorkerCommands() {
    if (!use_worker_thread) {
        return;
    }

#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::InterpolationPerformance::Scope timing{MM3D::InterpolationPerformance::Part::WorkerWait};
#endif
    MICROPROFILE_SCOPE(Vulkan_WaitForWorker);
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    // This needs to be done in the same order as WorkerThread.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::Wait(u64 tick, [[maybe_unused]] std::source_location caller) {
    if (IsFree(tick))
        return;
#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::InterpolationPerformance::WaitSiteScope source_timing{caller};
    MM3D::InterpolationPerformance::Scope timing{MM3D::InterpolationPerformance::Part::GpuWait};
#endif
    if (tick >= master_semaphore->CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        Flush();
    }
    master_semaphore->Wait(tick);
}

void Scheduler::DispatchWork() {
    if (!use_worker_thread || chunk->Empty()) {
        return;
    }

    on_dispatch();

    {
        std::scoped_lock ql{queue_mutex};
        work_queue.push(std::move(chunk));
    }

    event_cv.notify_all();
    AcquireNewChunk();
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            Common::CondvarWait(event_cv, lk, stop_token, [&] { return TryPopQueue(work); });

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            std::exchange(lk, std::unique_lock{execution_mutex});

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffers();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    current_cmdbuf.begin(begin_info);
#ifdef ENABLE_MM3D_PRESENTATION
    timing_slot=-1;
    if(timing_pool) {
        // Worker-owned slots. Never wait or reuse a query still referenced by the GPU.
        for(u32 i=0;i<timing_slots.size();++i) {
            auto& slot=timing_slots[i];
            if(!slot.tick || !IsFree(slot.tick))continue;
            std::array<u64,4> result{};
            const auto status=timing_device.getQueryPoolResults(*timing_pool,i*2,2,
                sizeof(result),result.data(),2*sizeof(u64),
                vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWithAvailability);
            if(status!=vk::Result::eSuccess || !result[1] || !result[3])continue;
            const u64 mask=timing_bits==64?~u64{0}:(u64{1}<<timing_bits)-1;
            const auto ns = static_cast<u64>(double((result[2]-result[0])&mask)*timing_period);
            if (slot.trace) {
                slot.trace->gpu_ns = ns;
                slot.trace->gpu_available = true;
            }
            MM3D::InterpolationPerformance::AddGpuPart(slot.session,slot.phase,ns);
            slot={};
        }
        const auto session=MM3D::InterpolationPerformance::ActiveSession();
        if(session)for(u32 i=0;i<timing_slots.size();++i) {
            if(timing_slots[i].tick)continue;
            timing_slot=static_cast<int>(i);
            timing_slots[i].session=session;
            current_cmdbuf.resetQueryPool(*timing_pool,i*2,2);
            current_cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,*timing_pool,i*2);
            break;
        }
    }
#endif
}

void Scheduler::SubmitExecution(vk::Semaphore signal_semaphore, vk::Semaphore wait_semaphore,
    [[maybe_unused]] std::shared_ptr<MM3D::InterpolationTimeline::Submission> trace) {
    FlushReplay();
    state = StateFlags::AllDirty;
    const u64 signal_value = master_semaphore->NextTick();
    [[maybe_unused]] const unsigned performance_phase =
#ifdef ENABLE_MM3D_PRESENTATION
        timing_phase;
#else
        0;
#endif

    on_submit();

    Record([signal_semaphore, wait_semaphore, signal_value,
#ifdef ENABLE_MM3D_PRESENTATION
            performance_phase, trace,
#endif
            this](vk::CommandBuffer cmdbuf) {
        MICROPROFILE_SCOPE(Vulkan_Submit);
#ifdef ENABLE_MM3D_PRESENTATION
        if (trace) trace->worker = MM3D::InterpolationPerformance::Now();
#endif
        std::scoped_lock lock{submit_mutex};
#ifdef ENABLE_MM3D_PRESENTATION
        if (trace) trace->locked = MM3D::InterpolationPerformance::Now();
        if(timing_slot>=0) {
            cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,*timing_pool,
                static_cast<u32>(timing_slot)*2+1);
            timing_slots[timing_slot].phase=performance_phase;
            timing_slots[timing_slot].tick=signal_value;
            timing_slots[timing_slot].trace=trace;
        }
        MM3D::InterpolationPerformance::Scope timing{MM3D::InterpolationPerformance::Part::Submit};
#endif
        master_semaphore->SubmitWork(cmdbuf, wait_semaphore, signal_semaphore, signal_value);
#ifdef ENABLE_MM3D_PRESENTATION
        if (trace) trace->submitted = MM3D::InterpolationPerformance::Now();
#endif
    });

    master_semaphore->Refresh();

    if (!use_worker_thread) {
        AllocateWorkerCommandBuffers();
    } else {
        chunk->MarkSubmit();
        DispatchWork();
    }
}

void Scheduler::AcquireNewChunk() {
    std::scoped_lock lock{reserve_mutex};
    if (chunk_reserve.empty()) {
        chunk = std::make_unique<CommandChunk>();
        return;
    }

    chunk = std::move(chunk_reserve.back());
    chunk_reserve.pop_back();
}

} // namespace Vulkan
