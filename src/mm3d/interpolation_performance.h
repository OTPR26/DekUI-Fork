// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <memory>
#include <source_location>
#include <string_view>
#include "common/logging/log.h"
#include "mm3d/interpolation_timeline.h"

namespace MM3D::InterpolationPerformance {
using U64=std::uint64_t;
enum class Part { Draw, Replay, Skeleton, WorkerWait, GpuWait, Submit, AcquireFrame, Compose, Present,
                  NativePass, MidpointPass, SlowFrame, UniformReuse, TexelReuse, VertexReuse, UploadReuse, DownloadReuse, CpuVertexReplay, Count };
inline U64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Sample { U64 calls{},ns{}; };
inline std::mutex mutex;
inline std::atomic<bool> pending{};
inline U64 session{},begin{},end{},previous{},intervals{},interval_sum{},interval_max{};
inline bool enabled{},present{};
inline std::array<Sample,static_cast<unsigned>(Part::Count)> samples{};
inline std::array<Sample,static_cast<unsigned>(Part::Count)> gpu_parts{};
inline std::array<U64,static_cast<unsigned>(Part::Count)> gpu_part_max{};
inline Sample gpu_sample{};
inline U64 gpu_max{};
inline U64 slow_frames{}, slow_frame_ns_max{};
inline U64 ActiveSession();
struct GuestReadInfo { bool active{}; U64 address{}, pc{}, lr{}; };
inline thread_local GuestReadInfo guest_read;
class GuestReadScope {
    GuestReadInfo previous;
public:
    GuestReadScope(U64 address,U64 pc,U64 lr) : previous{guest_read} {
        guest_read={true,address,pc,lr};
    }
    ~GuestReadScope(){guest_read=previous;}
};
inline void TraceDownload(U64 address,unsigned width,unsigned height,unsigned format) {
    const auto id=ActiveSession();
    if(!id)return;
    static U64 last_session{};
    static unsigned count{};
    if(last_session!=id){last_session=id;count=0;}
    if(count++>=8)return;
    LOG_INFO(Render_Vulkan,"Interpolation readback origin session={} jit_read={} guest_address={:#x} register_pc={:#x} register_lr={:#x} surface_address={:#x} width={} height={} format={}; JIT register PC may identify block rather than exact instruction",id,guest_read.active,guest_read.address,guest_read.pc,guest_read.lr,address,width,height,format);
}
struct ReadbackInfo {
    const char* reason="unclassified/internal cache operation";
    U64 address{}, size{};
};
inline thread_local ReadbackInfo readback_info;
class ReadbackScope {
    ReadbackInfo previous;
public:
    ReadbackScope(const char* reason,U64 address=0,U64 size=0) : previous{readback_info} {
        readback_info={reason,address,size};
    }
    ~ReadbackScope(){readback_info=previous;}
};
struct WaitSite {
    std::source_location location{};
    Sample sample{};
    ReadbackInfo readback{};
    U64 max_ns{};
};
inline std::array<WaitSite,128> wait_sites{};
inline unsigned wait_site_count{};
inline U64 wait_sites_dropped{};
inline U64 ActiveSession() {
    if(!pending.load(std::memory_order_relaxed))return 0;
    std::scoped_lock lock{mutex};
    const auto now=Now();
    return pending && now>=begin && now<end ? session : 0;
}

using SubmissionTrace = InterpolationTimeline::Submission;
using PresentTrace = InterpolationTimeline::Present;
inline std::array<std::shared_ptr<SubmissionTrace>, 2048> submission_traces{};
inline std::array<PresentTrace, 4096> present_traces{};
inline unsigned submission_trace_count{}, present_trace_count{};
inline U64 submission_traces_dropped{}, present_traces_dropped{};
inline U64 replay_batch_count{}, replay_batch_scopes{}, replay_batch_max{};

inline void RecordReplayBatch(unsigned scopes) {
    if (!pending.load(std::memory_order_relaxed)) return;
    const auto now = Now();
    std::scoped_lock lock{mutex};
    if (!pending || now < begin || now >= end) return;
    ++replay_batch_count;
    replay_batch_scopes += scopes;
    replay_batch_max = std::max(replay_batch_max, U64(scopes));
}

inline std::shared_ptr<SubmissionTrace> BeginFinish(const void* owner, U64 tick,
                                                   std::source_location caller) {
    if (!pending.load(std::memory_order_relaxed)) return {};
    const auto now = Now();
    std::scoped_lock lock{mutex};
    if (!pending || now < begin || now >= end) return {};
    if (submission_trace_count == submission_traces.size()) {
        ++submission_traces_dropped;
        return {};
    }
    auto trace = std::make_shared<SubmissionTrace>();
    trace->session = session;
    trace->tick = tick;
    trace->requested = now;
    trace->owner = reinterpret_cast<std::uintptr_t>(owner);
    trace->address = readback_info.address;
    trace->bytes = readback_info.size;
    trace->reason = readback_info.reason;
    trace->pc = guest_read.pc;
    trace->file = caller.file_name();
    trace->line = caller.line();
    submission_traces[submission_trace_count++] = trace;
    return trace;
}

inline void RecordPresent(const PresentTrace& trace) {
    if (!trace.session) return;
    std::scoped_lock lock{mutex};
    if (!pending || trace.session != session || trace.unlocked > end) return;
    if (present_trace_count == present_traces.size()) {
        ++present_traces_dropped;
        return;
    }
    present_traces[present_trace_count++] = trace;
}

// Called with the capture mutex held. Worker timestamps are atomic; an unfinished
// tail is explicitly excluded, never waited for. No logging inside queue locks.
inline void ReportTimeline() {
    LOG_INFO(Render_Vulkan, "Interpolation replay batching session={} batches={} scopes={} mean_scopes={} max_scopes={}; scopes are captured world-draw groups, not display frames",
             session, replay_batch_count, replay_batch_scopes,
             replay_batch_count ? double(replay_batch_scopes) / replay_batch_count : 0., replay_batch_max);
    constexpr std::array phase_names{"finish_total", "before_worker_submission",
        "graphics_queue_lock", "submit_call_and_timestamp_recording",
        "after_submit_return_to_completion_observed", "submit_completion_overlap"};
    std::array<Sample, phase_names.size()> totals{};
    std::array<U64, phase_names.size()> maxima{};
    std::array<const SubmissionTrace*, 2048> complete{};
    unsigned count = 0;
    U64 present_overlap = 0;
    for (unsigned i = 0; i < submission_trace_count; ++i) {
        const auto& trace = *submission_traces[i];
        const auto d = InterpolationTimeline::Measure(trace);
        if (!d.valid || trace.completed.load() > end || trace.submitted.load() > end) continue;
        complete[count++] = &trace;
        const std::array values{d.total, d.before_worker, d.queue_lock, d.submit,
                                d.after_submit, d.overlap};
        for (unsigned j = 0; j < values.size(); ++j) {
            ++totals[j].calls;
            totals[j].ns += values[j];
            maxima[j] = std::max(maxima[j], values[j]);
        }
        for (unsigned j = 0; j < present_trace_count; ++j)
            present_overlap += InterpolationTimeline::PresentLockOverlap(trace, present_traces[j]);
    }
    LOG_INFO(Render_Vulkan, "Interpolation submission timeline session={} completed={} incomplete={} dropped={} presentation_lock_overlap_ms={}; CPU durations, completion is host observation, after-submit includes GPU queue/dependency waits and wakeup delay, not pure GPU execution",
             session, count, submission_trace_count - count, submission_traces_dropped,
             double(present_overlap) / 1e6);
    for (unsigned i = 0; i < totals.size(); ++i) {
        const auto& s = totals[i];
        LOG_INFO(Render_Vulkan, "Interpolation submission phase session={} phase={} calls={} total_ms={} mean_ms={} max_ms={}",
                 session, phase_names[i], s.calls, double(s.ns) / 1e6,
                 s.calls ? double(s.ns) / s.calls / 1e6 : 0., double(maxima[i]) / 1e6);
    }
    // Include the first four completed samples and twelve slowest. Both carry
    // capture-relative CPU times so presentation events can be correlated.
    const auto log_submission = [&](const SubmissionTrace& s, const char* selection) {
        U64 overlap = 0;
        for (unsigned i = 0; i < present_trace_count; ++i)
            overlap += InterpolationTimeline::PresentLockOverlap(s, present_traces[i]);
        LOG_INFO(Render_Vulkan, "Interpolation submission detail session={} selection={} tick={} file={} line={} reason={} address={:#x} bytes={} pc={:#x} request_ms={} worker_ms={} locked_ms={} submit_return_ms={} completion_observed_ms={} present_lock_overlap_ms={} gpu_span_available={} gpu_submission_span_ms={}",
                 session, selection, s.tick, s.file, s.line, s.reason, s.address, s.bytes, s.pc,
                 double(s.requested - begin) / 1e6, double(s.worker.load() - begin) / 1e6,
                 double(s.locked.load() - begin) / 1e6, double(s.submitted.load() - begin) / 1e6,
                 double(s.completed.load() - begin) / 1e6, double(overlap) / 1e6,
                 s.gpu_available.load(), double(s.gpu_ns.load()) / 1e6);
    };
    for (unsigned i = 0; i < std::min(count, 4u); ++i) log_submission(*complete[i], "first");
    std::sort(complete.begin(), complete.begin() + count, [](const auto* a, const auto* b) {
        return a->completed.load() - a->requested > b->completed.load() - b->requested;
    });
    for (unsigned i = 0; i < std::min(count, 12u); ++i) log_submission(*complete[i], "slowest");

    constexpr std::array present_names{"pacing", "acquire_and_recreate", "record_commands",
        "queue_and_surface_lock", "queue_submit", "actual_present", "queue_lock_hold"};
    std::array<Sample, present_names.size()> present_totals{};
    std::array<U64, present_names.size()> present_max{};
    std::array<const PresentTrace*, 4096> ordered_present{};
    for (unsigned i = 0; i < present_trace_count; ++i) {
        const auto& p = present_traces[i];
        ordered_present[i] = &p;
        const std::array values{p.paced - p.started, p.acquired - p.paced,
            p.lock_requested - p.acquired, p.locked - p.lock_requested,
            p.submitted - p.locked, p.returned - p.submitted, p.unlocked - p.locked};
        for (unsigned j = 0; j < values.size(); ++j) {
            ++present_totals[j].calls;
            present_totals[j].ns += values[j];
            present_max[j] = std::max(present_max[j], values[j]);
        }
    }
    for (unsigned i = 0; i < present_totals.size(); ++i) {
        const auto& s = present_totals[i];
        LOG_INFO(Render_Vulkan, "Interpolation presentation phase session={} phase={} calls={} total_ms={} mean_ms={} max_ms={} dropped={}",
                 session, present_names[i], s.calls, double(s.ns) / 1e6,
                 s.calls ? double(s.ns) / s.calls / 1e6 : 0., double(present_max[i]) / 1e6,
                 present_traces_dropped);
    }
    std::sort(ordered_present.begin(), ordered_present.begin() + present_trace_count,
        [](const auto* a, const auto* b) { return a->unlocked - a->locked > b->unlocked - b->locked; });
    for (unsigned i = 0; i < std::min(present_trace_count, 12u); ++i) {
        const auto& p = *ordered_present[i];
        LOG_INFO(Render_Vulkan, "Interpolation presentation detail session={} shared_queue={} lock_request_ms={} locked_ms={} submit_return_ms={} present_return_ms={} unlocked_ms={}",
                 session, p.shared_queue, double(p.lock_requested - begin) / 1e6,
                 double(p.locked - begin) / 1e6, double(p.submitted - begin) / 1e6,
                 double(p.returned - begin) / 1e6, double(p.unlocked - begin) / 1e6);
    }
}
inline void AddGpu(U64 id,U64 ns) {
    std::scoped_lock lock{mutex};
    if(!pending || id!=session)return;
    ++gpu_sample.calls;gpu_sample.ns+=ns;gpu_max=std::max(gpu_max,ns);
}
inline void AddGpuPart(U64 id,unsigned part,U64 ns) {
    std::scoped_lock lock{mutex};
    if(!pending || id!=session)return;
    ++gpu_sample.calls;gpu_sample.ns+=ns;gpu_max=std::max(gpu_max,ns);
    if(part<static_cast<unsigned>(Part::Count)) {
        auto& sample=gpu_parts[part];++sample.calls;sample.ns+=ns;
        gpu_part_max[part]=std::max(gpu_part_max[part],ns);
    }
}
// The source location is captured at the caller, including callers of Finish.
// Accumulate in memory; avoid logging on the render thread for each stall.
class WaitSiteScope {
    std::source_location location;
    ReadbackInfo readback;
    U64 id{}, start{};
public:
    explicit WaitSiteScope(std::source_location caller) : location{caller}, readback{readback_info} {
        id=ActiveSession();
        if(id)start=Now();
    }
    ~WaitSiteScope() {
        if(!start)return;
        const auto finish=Now();
        std::scoped_lock lock{mutex};
        if(!pending || session!=id || finish>end)return;
        unsigned index=0;
        for(;index<wait_site_count;++index) {
            const auto& site=wait_sites[index].location;
            if(site.line()==location.line() &&
               std::string_view{site.file_name()}==location.file_name() &&
               wait_sites[index].readback.address==readback.address &&
               wait_sites[index].readback.size==readback.size &&
               std::string_view{wait_sites[index].readback.reason}==readback.reason)break;
        }
        if(index==wait_site_count) {
            if(index==wait_sites.size()){++wait_sites_dropped;return;}
            wait_sites[index].location=location;
            wait_sites[index].readback=readback;
            ++wait_site_count;
        }
        auto& site=wait_sites[index];
        const auto elapsed=finish-start;
        ++site.sample.calls;site.sample.ns+=elapsed;
        site.max_ns=std::max(site.max_ns,elapsed);
    }
};
inline void Start(bool replay_enabled,bool presentation_enabled) {
    std::scoped_lock lock{mutex};
    ++session;begin=Now()+3000000000ULL;end=begin+30000000000ULL;
    enabled=replay_enabled;present=presentation_enabled;
    wait_sites={};wait_site_count=0;wait_sites_dropped=0;
    submission_traces={};submission_trace_count=present_trace_count=0;
    submission_traces_dropped=present_traces_dropped=0;
    replay_batch_count=replay_batch_scopes=replay_batch_max=0;
    samples={};gpu_parts={};gpu_part_max={};gpu_sample={};gpu_max=0;slow_frames=slow_frame_ns_max=0;
    previous=intervals=interval_sum=interval_max=0;pending=true;
    LOG_INFO(Render_Vulkan,"Interpolation performance start session={} replay={} present60={} delay_s=3 duration_s=30; CPU nested scopes overlap. GPU timestamps measure completed submission spans, including GPU waits, not utilization; unfinished tail submissions are excluded. Keep scene, resolution and draw distance unchanged for off/on comparison.",session,enabled,present);
}
class Scope {
    Part part;
    U64 id{},start{};
public:
    explicit Scope(Part p,bool include=true):part(p) {
        if(!include || !pending.load(std::memory_order_relaxed))return;
        std::scoped_lock lock{mutex};
        const auto now=Now();
        if(pending && now>=begin && now<end){id=session;start=now;}
    }
    ~Scope() {
        if(!start)return;
        const auto now=Now();
        std::scoped_lock lock{mutex};
        if(id!=session || !pending || now>end)return;
        auto& s=samples[static_cast<unsigned>(part)];++s.calls;s.ns+=now-start;
    }
};
class FrameScope {
    U64 id{}, start{};
    const char* kind{};
public:
    explicit FrameScope(const char* kind_) : kind{kind_} {
        if(!pending.load(std::memory_order_relaxed))return;
        std::scoped_lock lock{mutex};
        const auto now=Now();
        if(pending && now>=begin && now<end){id=session;start=now;}
    }
    ~FrameScope() {
        if(!start)return;
        const auto finish=Now();
        std::scoped_lock lock{mutex};
        if(id!=session || !pending || finish>end)return;
        const auto elapsed=finish-start;
        auto& sample=samples[static_cast<unsigned>(
            kind[0]=='m' ? Part::MidpointPass : Part::NativePass)];
        ++sample.calls;sample.ns+=elapsed;
        if(elapsed>16666667ULL) {
            ++slow_frames;slow_frame_ns_max=std::max(slow_frame_ns_max,elapsed);
            auto& slow=samples[static_cast<unsigned>(Part::SlowFrame)];
            ++slow.calls;slow.ns+=elapsed;
            if(slow.calls<=24)
                LOG_INFO(Render_Vulkan,"Interpolation slow frame session={} kind={} elapsed_ms={} over_budget_ms={}",
                    id,kind,double(elapsed)/1e6,double(elapsed-16666667ULL)/1e6);
        }
    }
};
inline void Tick(bool replay_enabled,bool presentation_enabled) {
    if(!pending.load(std::memory_order_relaxed))return;
    std::scoped_lock lock{mutex};
    if(!pending)return;
    if(enabled!=replay_enabled || present!=presentation_enabled) {
        pending=false;
        LOG_INFO(Render_Vulkan,"Interpolation performance cancelled session={}: interpolation mode changed; restart capture.",session);
        return;
    }
    const auto now=Now();if(now<begin)return;
    if(now<end) {
        if(previous){const auto dt=now-previous;++intervals;interval_sum+=dt;interval_max=std::max(interval_max,dt);}
        previous=now;return;
    }
    pending=false;
    ReportTimeline();
    LOG_INFO(Render_Vulkan,"Interpolation performance complete session={} replay={} present60={} window_calls={} mean_window_interval_ms={} max_window_interval_ms={}; window callbacks are not display scanout",session,enabled,present,intervals+bool(previous),intervals?double(interval_sum)/intervals/1e6:0.,double(interval_max)/1e6);
    LOG_INFO(Render_Vulkan,"Interpolation performance GPU session={} completed_submissions={} total_ms={} mean_ms={} max_ms={}; submission spans may overlap; not GPU utilization or scanout",session,gpu_sample.calls,double(gpu_sample.ns)/1e6,gpu_sample.calls?double(gpu_sample.ns)/gpu_sample.calls/1e6:0.,double(gpu_max)/1e6);
    for(const auto part : {Part::NativePass,Part::MidpointPass}) {
        const auto& s=gpu_parts[static_cast<unsigned>(part)];
        LOG_INFO(Render_Vulkan,"Interpolation performance GPU phase session={} phase={} submissions={} total_ms={} mean_ms={} max_ms={}",session,part==Part::NativePass?"native":"midpoint",s.calls,double(s.ns)/1e6,s.calls?double(s.ns)/s.calls/1e6:0.,double(gpu_part_max[static_cast<unsigned>(part)])/1e6);
    }
    LOG_INFO(Render_Vulkan,"Interpolation frame budget session={} slow_frames={} max_slow_frame_ms={}; budget_ms=16.666667; only frames over budget are logged individually",session,slow_frames,double(slow_frame_ns_max)/1e6);
    for(unsigned i=0;i<wait_site_count;++i) {
        const auto& site=wait_sites[i];
        LOG_INFO(Render_Vulkan,"Interpolation GPU wait source session={} file={} line={} function={} reason={} address={:#x} bytes={} calls={} total_ms={} mean_ms={} max_ms={}",
            session,site.location.file_name(),site.location.line(),site.location.function_name(),
            site.readback.reason,site.readback.address,site.readback.size,
            site.sample.calls,double(site.sample.ns)/1e6,
            double(site.sample.ns)/site.sample.calls/1e6,double(site.max_ns)/1e6);
    }
    if(wait_sites_dropped)
        LOG_INFO(Render_Vulkan,"Interpolation GPU wait source overflow session={} dropped={}",session,wait_sites_dropped);
    constexpr std::array names{"draw_total_including_replay","extra_frame_replay_including_skeleton","skeleton_matching_and_interpolation","scheduler_worker_wait","scheduler_gpu_wait","driver_queue_submit","acquire_present_frame","screen_composition","queue_present","native_pass","midpoint_pass","slow_frame","uniform_buffer_reuse_wait","texel_buffer_reuse_wait","vertex_buffer_reuse_wait","upload_buffer_reuse_wait","download_buffer_reuse_wait","cpu_vertex_replay_and_upload"};
    static_assert(names.size() == samples.size());
    for(unsigned i=0;i<samples.size();++i) {
        const auto& s=samples[i];
        LOG_INFO(Render_Vulkan,"Interpolation performance CPU session={} scope={} calls={} total_ms={} mean_us={}",session,names[i],s.calls,double(s.ns)/1e6,s.calls?double(s.ns)/s.calls/1e3:0.);
    }
}
}
