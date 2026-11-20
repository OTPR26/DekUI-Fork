#pragma once

// Diffuse screen-space GI, milestone M5: the runtime pass object
// (docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M5"). It runs the module's GPU pipeline
// (diffuse_gi_pass_plan.h) with the precompiled kernels of builtin_pass.spvpack
// on the renderer's device: pass-owned buffers (adapter inputs, frame
// transients, per-view histories) and two caller-owned output images,
//   DiffuseIllumination  R16G16B16A16_SFLOAT  J.rgb (before the receiver albedo), receiver flag
//   accessibility        R32_SFLOAT           A clamped to [0, 1] (optional)
// at the output extent. Title-neutral raw Vulkan (no NRI, PICA or title
// vocabulary); the renderer's provider maps its graph contract onto it.
//
// Kernels (DiffuseGiKernels) are shared by every pass instance of a device and
// can be created off the frame: resolve the SPIR-V on the thread that owns the
// shader compiler, create the pipelines on any thread, publish when complete.
//
// Lifecycle of an instance (one per view and composed surface: the history
// belongs to the instance):
//   Initialize   bind the shared kernels (or create private ones: tests)
//   Configure    extent, settings, optional inputs, output images; replaces
//                resources only when their sizes change; output images are
//                cached by view, so alternating outputs reuse their sets
//   Execute      records one frame; adapter inputs must be written (compute
//                shader or transfer writes recorded earlier on the same queue)
//   RepublishOutputs  writes the last frame's outputs again (another
//                composition of the same frame), without advancing history
//   ClearOutputs records neutral outputs (J = 0, A = 1) for a frame without GI
//                and invalidates the history (the next frame resets)
//   ResetHistory the next frame resets (explicit)
//   InvalidateScreenResources / Shutdown release resources
// Configure (when ReplacesResources), InvalidateScreenResources and Shutdown
// destroy Vulkan objects: the caller first completes every command buffer
// that recorded this pass (the renderer's recorded-work retirement boundary).
// A Configure that changes only parameters, or selects cached outputs,
// destroys nothing.

#include "fast/renderer/diffuse_gi_pass_plan.h"
#include "fast/renderer/shaderc_compiler.h"

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

struct PassDevice {
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkPipelineCache PipelineCache = VK_NULL_HANDLE; // optional; must be internally synchronized when threaded
};

// SPIR-V of a kernel from its exact source text (tests compare resolvers).
using PassShaderResolver = std::function<std::vector<uint32_t>(const PassKernelSource& kernel)>;
using PassKernelBinaries = std::array<std::vector<uint32_t>, kPassKernelCount>;

// Whether a device can run the pass, with the reason when not (storage
// buffers per stage, storage formats of the outputs).
[[nodiscard]] bool DiffuseGiPassSupported(VkPhysicalDevice physical, std::string& reason);

// Set layouts, pipeline layouts and pipelines of the nine kernels, shared by
// the pass instances of one device.
class DiffuseGiKernels final {
  public:
    DiffuseGiKernels() = default;
    ~DiffuseGiKernels();
    DiffuseGiKernels(const DiffuseGiKernels&) = delete;
    DiffuseGiKernels& operator=(const DiffuseGiKernels&) = delete;

    // Resolves every kernel on the calling thread; empty modules when a kernel
    // cannot be resolved (the reason names it).
    [[nodiscard]] static PassKernelBinaries Resolve(const PassShaderResolver& resolver, std::string& reason);
    [[nodiscard]] static PassKernelBinaries Resolve(CachedPassShaderCompiler& shaders, std::string& reason);
    // Creates layouts and pipelines; safe on a worker thread (it touches only
    // this object and the device). Complete() turns true when it succeeded.
    bool Create(const PassDevice& device, const PassKernelBinaries& binaries);
    void Destroy() noexcept;
    [[nodiscard]] bool Complete() const noexcept { return mComplete.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& Reason() const noexcept { return mReason; }
    [[nodiscard]] double CreationMilliseconds() const noexcept { return mCreationMilliseconds; }
    [[nodiscard]] const std::array<double, kPassKernelCount>& KernelMilliseconds() const noexcept {
        return mKernelMilliseconds;
    }

    enum Layout : uint32_t { Gather, Pack, Temporal, Filter, Output, Upsample, LayoutCount };
    [[nodiscard]] VkDescriptorSetLayout SetLayout(Layout layout) const noexcept { return mSetLayouts[layout]; }
    [[nodiscard]] VkPipelineLayout PipelineLayout(Layout layout) const noexcept { return mPipelineLayouts[layout]; }
    [[nodiscard]] VkPipeline Pipeline(PassKernel kernel) const noexcept {
        return mPipelines[static_cast<size_t>(kernel)];
    }
    [[nodiscard]] static Layout KernelLayout(PassKernel kernel) noexcept;

  private:
    VkDevice mDevice = VK_NULL_HANDLE;
    std::array<VkDescriptorSetLayout, LayoutCount> mSetLayouts{};
    std::array<VkPipelineLayout, LayoutCount> mPipelineLayouts{};
    std::array<VkPipeline, kPassKernelCount> mPipelines{};
    std::atomic<bool> mComplete{false};
    std::string mReason = "not created";
    double mCreationMilliseconds = 0.0;
    std::array<double, kPassKernelCount> mKernelMilliseconds{};
};

struct PassImageBinding {
    VkImage Image = VK_NULL_HANDLE;
    VkImageView View = VK_NULL_HANDLE; // storage-capable view of mip 0, layer 0
    VkFormat Format = VK_FORMAT_UNDEFINED;
    uint32_t Width = 0, Height = 0;
};

struct PassOutputs {
    PassImageBinding Illumination;  // R16G16B16A16_SFLOAT, STORAGE usage, output extent
    // R32_SFLOAT, STORAGE usage, output extent; a null image: A is not
    // written (the pass binds a private 1 x 1 placeholder).
    PassImageBinding Accessibility;
};

// Vulkan state of an output image. The caller owns it (its graph state
// tracker): the pass transitions from it and stores the completion state.
struct PassImageState {
    VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags Stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags Access = 0;
    bool operator==(const PassImageState&) const = default;
};

// Completion states the caller's graph decides: readable by later passes, or
// left in the dispatch state (GENERAL, compute shader write) when nothing
// reads the output, which elides the completion transition. With
// ExternalTransitions the caller transitions the outputs itself (to the
// dispatch state before the call, from it after) and the pass emits no image
// barrier on them.
struct PassOutputAccess {
    PassImageState Illumination{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_SHADER_READ_BIT};
    PassImageState Accessibility{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_SHADER_READ_BIT};
    bool ExternalTransitions = false;
};
// The dispatch state of both outputs.
inline constexpr PassImageState kPassOutputWriteState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                      VK_ACCESS_SHADER_WRITE_BIT};

// Barrier execution record of one Execute, RepublishOutputs or ClearOutputs.
struct PassBarrierRecord {
    uint32_t PlannedOutputTransitions = 0; // dispatch + completion per output image
    uint32_t EmittedOutputTransitions = 0;
    uint32_t ElidedOutputTransitions = 0;  // completion already equal to the dispatch state
    uint32_t PrivateBarriers = 0;          // memory barriers on pass-owned buffers and images
    uint32_t Dispatches = 0;
};

struct PassStats {
    uint64_t Configurations = 0;   // successful Configure calls
    uint64_t Reallocations = 0;    // Configure calls that replaced resources
    uint64_t Executions = 0;
    uint64_t Republications = 0;
    uint64_t Clears = 0;
    uint64_t Failures = 0;         // refused calls (nothing recorded)
    uint64_t ResetFrames = 0;      // executed frames that reset the history
    std::array<uint64_t, 8> ResetsByReason{}; // index = bit of kReset* (first frame ... disabled)
    uint64_t HistoryResets = 0;    // ResetHistory calls
    uint64_t Invalidations = 0;    // InvalidateScreenResources calls
    uint32_t LastResetReasons = 0;
    uint64_t BufferBytes = 0;      // sum of the pass-owned VkBuffer sizes (= PassResourcePlan::PassBytes)
    uint64_t MemoryBytes = 0;      // device memory allocated for them (alignment included)
    uint32_t KernelModules = 0;    // pipelines of the bound kernels
    uint32_t OutputSets = 0;       // output image pairs with descriptor sets
};

// Developer timing (off by default): GPU time of each kernel of one executed
// frame, from timestamps written after every dispatch (the pass's barriers
// serialize the kernels, so consecutive timestamps bound one kernel; levels of
// the hierarchy and the filter are summed). Results arrive a few frames late
// (a ring of query pools read without waiting).
struct PassKernelTimings {
    std::array<double, kPassKernelCount> Milliseconds{}; // by PassKernel
    double Total = 0.0;                                   // first to last timestamp of the frame
    uint64_t Frames = 0;                                  // frames read back so far
    bool Valid = false;
};

class DiffuseGiPass final {
  public:
    DiffuseGiPass();
    ~DiffuseGiPass();
    DiffuseGiPass(const DiffuseGiPass&) = delete;
    DiffuseGiPass& operator=(const DiffuseGiPass&) = delete;

    // Shared kernels (complete, or completing on another thread: Execute and
    // the output calls refuse until Complete()).
    bool Initialize(const PassDevice& device, std::shared_ptr<const DiffuseGiKernels> kernels);
    // Private kernels created at once, from the precompiled pack or a resolver.
    bool Initialize(const PassDevice& device, CachedPassShaderCompiler& shaders);
    bool Initialize(const PassDevice& device, const PassShaderResolver& resolver);

    // True when Configure with these arguments would destroy or replace
    // Vulkan objects (the caller then completes its recorded work first).
    [[nodiscard]] bool ReplacesResources(const PassExtent& extent, const PassSettings& settings,
                                         const PassInputLayout& inputs, const PassOutputs& outputs) const;
    bool Configure(const PassExtent& extent, const PassSettings& settings, const PassInputLayout& inputs,
                   const PassOutputs& outputs);

    struct BufferView {
        VkBuffer Buffer = VK_NULL_HANDLE;
        VkDeviceSize Size = 0; // 0 when not allocated
    };
    // Adapter inputs (Depth ... OutputGuide) and, for diagnostics and tests,
    // any other pass buffer. Contents are valid after the commands that wrote them.
    [[nodiscard]] BufferView Buffer(PassResourceId id) const noexcept;
    // Index (0 or 1) of the history buffer the last frame wrote.
    [[nodiscard]] uint32_t LatestHistory() const noexcept;

    bool Execute(VkCommandBuffer command, const PassFrame& frame, PassImageState& illumination,
                 PassImageState& accessibility, const PassOutputAccess& completion, PassBarrierRecord& record);
    bool RepublishOutputs(VkCommandBuffer command, PassImageState& illumination, PassImageState& accessibility,
                          const PassOutputAccess& completion, PassBarrierRecord& record);
    bool ClearOutputs(VkCommandBuffer command, PassImageState& illumination, PassImageState& accessibility,
                      const PassOutputAccess& completion, PassBarrierRecord& record);
    void ResetHistory() noexcept;
    void InvalidateScreenResources() noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] bool Available() const noexcept;           // kernels complete and configured
    [[nodiscard]] const std::string& UnavailableReason() const noexcept;
    [[nodiscard]] const PassStats& Stats() const noexcept;
    [[nodiscard]] const PassResourcePlan& Resources() const noexcept;
    [[nodiscard]] const PassParameterLayout& ParameterLayout() const noexcept;
    [[nodiscard]] const PassFramePlan& LastFrame() const noexcept; // constants of the last executed frame
    [[nodiscard]] const DiffuseGiKernels* Kernels() const noexcept;

    // Developer per-kernel timing (see PassKernelTimings); needs a queue with
    // timestamp support. Off releases the query pools.
    void SetKernelTiming(bool enabled) noexcept;
    [[nodiscard]] const PassKernelTimings& LastKernelTimings() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Fast::Renderer::DiffuseGi
