// Copyright 2019 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <memory>
#include <source_location>
#include <utility>
#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace MM3D::InterpolationTimeline {
struct Submission;
}

namespace Vulkan {

enum class StateFlags {
    AllDirty = 0,
    Pipeline = 1 << 0,
    DescriptorSets = 1 << 1,
};
DECLARE_ENUM_FLAG_OPERATORS(StateFlags)

class Instance;

/// The scheduler abstracts command buffer and fence management with an interface that's able to do
/// OpenGL-like operations on Vulkan command buffers.
class Scheduler {
public:
    explicit Scheduler(const Instance& instance);
    ~Scheduler();

    /// Sends the current execution context to the GPU.
    void Flush(vk::Semaphore signal = nullptr, vk::Semaphore wait = nullptr);

    /// Submit a skipped display frame without draining all prior GPU work.
    /// At most two skipped-frame submissions may remain outstanding.
    void FlushSkippedFrame();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(vk::Semaphore signal = nullptr, vk::Semaphore wait = nullptr,
                std::source_location caller = std::source_location::current());

    /// Waits for the worker thread to finish executing everything. After this function returns it's
    /// safe to touch worker resources.
    void WaitWorker();

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick, std::source_location caller = std::source_location::current());

    /// Sends currently recorded work to the worker thread.
    void DispatchWork();

    // Experimental replay commands stay in the current resource tick. Ordinary
    // commands and submissions drain them before resources can change or recycle.
    bool ReplayBatchingEnabled() const { return replay_batching_enabled; }
    bool CapturingReplay() const { return capture_replay; }
    bool DrainingReplay() const { return drain_replay; }
    bool NativeDrawActive() const { return native_draw_depth != 0 && replay_batching_enabled; }
    bool HasReplay() const { return replay_chunk || !replay_chunks.empty(); }
    void SetReplayBatchingEnabled(bool enabled);
    void RegisterReplayDrain(std::function<void()> callback) { on_replay_drain = std::move(callback); }
    void BeginReplayCapture();
    void EndReplayCapture();
    void FlushReplay();
    bool ReplayLimitReached() const { return replay_chunks.size() >= 16; }

    class NativeDrawScope {
    public:
        explicit NativeDrawScope(Scheduler& scheduler_, bool eligible = true) {
            if (eligible && scheduler_.ReplayBatchingEnabled()) {
                scheduler = &scheduler_;
                ++scheduler->native_draw_depth;
            }
        }
        ~NativeDrawScope() { End(); }
        NativeDrawScope(const NativeDrawScope&) = delete;
        NativeDrawScope& operator=(const NativeDrawScope&) = delete;
        void End() { if (scheduler) { --scheduler->native_draw_depth; scheduler = nullptr; } }
    private:
        Scheduler* scheduler{};
    };

#ifdef ENABLE_MM3D_PRESENTATION
    /// Labels the next command-buffer submission for interpolation profiling.
    void SetPerformancePhase(unsigned phase) noexcept { timing_phase = phase; }
#endif

    /// Records the command to the current chunk.
    template <typename T>
    void Record(T&& command) {
        if (capture_replay) {
            if (!replay_chunk) replay_chunk = std::make_unique<CommandChunk>();
            if (!replay_chunk->Record(command)) {
                replay_chunks.push_back(std::move(replay_chunk));
                replay_chunk = std::make_unique<CommandChunk>();
                (void)replay_chunk->Record(command);
            }
            return;
        }
        if (!native_draw_depth && !drain_replay && HasReplay()) FlushReplay();
        if (chunk->Record(command)) {
            return;
        }
        DispatchWork();
        (void)chunk->Record(command);
    }

    /// Marks the provided state as non dirty
    void MarkStateNonDirty(StateFlags flag) noexcept {
        state |= flag;
    }

    /// Marks the provided state as dirty
    void MakeDirty(StateFlags flag) noexcept {
        state &= ~flag;
    }

    /// Returns true if the state is dirty
    [[nodiscard]] bool IsStateDirty(StateFlags flag) const noexcept {
        return False(state & flag);
    }

    /// Registers a callback to perform on queue submission.
    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit = std::move(func);
    }

    /// Registers a callback to perform on queue submission.
    void RegisterOnDispatch(std::function<void()>&& func) {
        on_dispatch = std::move(func);
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore->CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return master_semaphore->IsFree(tick);
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore* GetMasterSemaphore() noexcept {
        return master_semaphore.get();
    }

    std::mutex submit_mutex;

private:
    class Command {
    public:
        virtual ~Command() = default;

        virtual void Execute(vk::CommandBuffer cmdbuf) const = 0;

        Command* GetNext() const {
            return next;
        }

        void SetNext(Command* next_) {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(vk::CommandBuffer cmdbuf) const override {
            command(cmdbuf);
        }

    private:
        T command;
    };

    class CommandChunk final {
    public:
        ~CommandChunk() {
            // Also release captures if the renderer is destroyed before dispatch.
            while (first) {
                auto* next = first->GetNext();
                first->~Command();
                first = next;
            }
        }
        void ExecuteAll(vk::CommandBuffer cmdbuf);

        template <typename T>
        bool Record(T& command) {
            using FuncType = TypedCommand<T>;
            static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

            recorded_counts++;
            command_offset = Common::AlignUp(command_offset, alignof(FuncType));
            if (command_offset > sizeof(data) - sizeof(FuncType)) {
                return false;
            }
            Command* const current_last = last;
            last = new (data.data() + command_offset) FuncType(std::move(command));

            if (current_last) {
                current_last->SetNext(last);
            } else {
                first = last;
            }
            command_offset += sizeof(FuncType);
            return true;
        }

        void MarkSubmit() {
            submit = true;
        }

        bool Empty() const {
            return recorded_counts == 0;
        }

        bool HasSubmit() const {
            return submit;
        }

    private:
        Command* first = nullptr;
        Command* last = nullptr;

        std::size_t recorded_counts = 0;
        std::size_t command_offset = 0;
        bool submit = false;
        alignas(std::max_align_t) std::array<u8, 0x8000> data{};
    };

private:
    void WorkerThread(std::stop_token stop_token);

    void AllocateWorkerCommandBuffers();

    void SubmitExecution(vk::Semaphore signal_semaphore, vk::Semaphore wait_semaphore,
                         std::shared_ptr<MM3D::InterpolationTimeline::Submission> trace = {});

    void AcquireNewChunk();

private:
#ifdef ENABLE_MM3D_PRESENTATION
    struct TimingSlot {
        u64 tick{}, session{};
        unsigned phase{};
        std::shared_ptr<MM3D::InterpolationTimeline::Submission> trace;
    };
    vk::Device timing_device;
    vk::UniqueQueryPool timing_pool;
    std::array<TimingSlot,64> timing_slots{};
    int timing_slot{-1};
    unsigned timing_phase{};
    u32 timing_bits{};
    double timing_period{};
#endif
    std::unique_ptr<MasterSemaphore> master_semaphore;
    std::array<u64, 2> skipped_frame_ticks{};
    unsigned skipped_frame_slot{};
    CommandPool command_pool;
    std::unique_ptr<CommandChunk> chunk;
    std::unique_ptr<CommandChunk> replay_chunk;
    std::vector<std::unique_ptr<CommandChunk>> replay_chunks;
    std::function<void()> on_replay_drain;
    bool replay_batching_enabled{}, capture_replay{}, drain_replay{};
    unsigned native_draw_depth{};
    unsigned queued_replay_scopes{};
    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    vk::CommandBuffer current_cmdbuf;
    StateFlags state{};
    std::function<void()> on_submit;
    std::function<void()> on_dispatch;
    std::mutex execution_mutex;
    std::mutex reserve_mutex;
    std::mutex queue_mutex;
    std::condition_variable_any event_cv;
    std::jthread worker_thread;
    bool use_worker_thread;
};

} // namespace Vulkan
