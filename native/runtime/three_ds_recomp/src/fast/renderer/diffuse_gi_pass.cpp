#include "fast/renderer/diffuse_gi_pass.h"

// Vulkan execution of the M5 runtime pass. Plumbing follows the repository's
// pass objects (compute pipelines resolved from the precompiled pack through
// the pass shader compiler, explicit producer -> consumer barriers, caller-
// owned output state, Configure / Execute / invalidation lifecycle); the
// algorithm is the module's kernels unchanged plus the own output and
// upsample kernels.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace Fast::Renderer::DiffuseGi {
namespace {

constexpr uint32_t kGroup = 8;
constexpr VkDeviceSize kUpdateLimit = 65536; // vkCmdUpdateBuffer
constexpr uint32_t kOutputVariants = 4;     // cached output image pairs per instance
constexpr uint32_t kColourSources = 3;      // temporal colour, filter ping, filter pong

constexpr VkDescriptorType kUbo = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
constexpr VkDescriptorType kDyn = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
constexpr VkDescriptorType kSsbo = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
constexpr VkDescriptorType kImg = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
// Gather family (normal, hierarchy, M4 gather): 0 gather block, 1-6 M2
// buffers, 7 M4 block (dynamic: the level), 8 cells, 9 SH, 10 guide table,
// 11 guide records, 12 noise (9-12 unused by the pass: placeholder), 13 M8
// shading normals (placeholder without them).
constexpr std::array<VkDescriptorType, 14> kGatherTypes{kUbo,  kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo,
                                                        kDyn,  kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
// Pack: 0-7 as the gather family's first eight, 8-12 placeholders, 13 M8
// shading normals in, 14 M8 receiver shading normals out.
constexpr std::array<VkDescriptorType, 15> kPackTypes{kUbo,  kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo,
                                                      kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
// Temporal 0-11 (M3) and 12 the M8 receiver shading normals; filter 0-5 and 6.
constexpr std::array<VkDescriptorType, 13> kTemporalTypes{kUbo,  kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo,
                                                          kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
constexpr std::array<VkDescriptorType, 7> kFilterTypes{kDyn, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo};
constexpr std::array<VkDescriptorType, 5> kOutputTypes{kDyn, kSsbo, kSsbo, kImg, kImg};
// Upsample: 0 block, 1 colour, 2 geometry, 3 output guide, 4 M8 receiver
// shading normals, 5 M8 output shading codes, 6-7 images.
constexpr std::array<VkDescriptorType, 8> kUpsampleTypes{kDyn, kSsbo, kSsbo, kSsbo, kSsbo, kSsbo, kImg, kImg};
constexpr uint32_t kMinStorageBuffersPerStage = 14; // the pack set

uint32_t Groups(uint32_t n) noexcept { return (n + kGroup - 1U) / kGroup; }

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(result) + ")");
}

struct OwnedBuffer {
    VkBuffer Handle = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkDeviceSize Size = 0;
    VkDeviceSize MemorySize = 0;
};

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits) {
    for (const VkMemoryPropertyFlags wanted :
         {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), VkMemoryPropertyFlags(0)})
        for (uint32_t t = 0; t < memory.memoryTypeCount; ++t)
            if ((bits & (1U << t)) != 0U && (memory.memoryTypes[t].propertyFlags & wanted) == wanted) return t;
    throw std::runtime_error("no memory type for a diffuse GI resource");
}

template <size_t N>
VkDescriptorSetLayout MakeSetLayout(VkDevice device, const std::array<VkDescriptorType, N>& types) {
    std::array<VkDescriptorSetLayoutBinding, N> bindings{};
    for (uint32_t i = 0; i < N; ++i) bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(N);
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    Check(vkCreateDescriptorSetLayout(device, &info, nullptr, &layout), "diffuse GI set layout");
    return layout;
}

} // namespace

// ---------------------------------------------------------------------------
// Support and kernels.

bool DiffuseGiPassSupported(VkPhysicalDevice physical, std::string& reason) {
    if (physical == VK_NULL_HANDLE) {
        reason = "no Vulkan device";
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.limits.maxPerStageDescriptorStorageBuffers < kMinStorageBuffersPerStage) {
        reason = "the device binds fewer than 14 storage buffers per stage";
        return false;
    }
    for (const VkFormat format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT}) {
        VkFormatProperties support{};
        vkGetPhysicalDeviceFormatProperties(physical, format, &support);
        if ((support.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0U) {
            reason = "the output formats are not storage images on this device";
            return false;
        }
    }
    reason.clear();
    return true;
}

DiffuseGiKernels::~DiffuseGiKernels() { Destroy(); }

DiffuseGiKernels::Layout DiffuseGiKernels::KernelLayout(PassKernel kernel) noexcept {
    switch (kernel) {
    case PassKernel::Pack: return Pack;
    case PassKernel::Temporal: return Temporal;
    case PassKernel::Filter: return Filter;
    case PassKernel::Output: return Output;
    case PassKernel::Upsample: return Upsample;
    default: return Gather;
    }
}

PassKernelBinaries DiffuseGiKernels::Resolve(const PassShaderResolver& resolver, std::string& reason) {
    PassKernelBinaries binaries;
    reason.clear();
    try {
        for (const PassKernelSource& kernel : BuildDiffuseGiPassKernels()) {
            auto words = resolver(kernel);
            if (words.size() < 5U || words[0] != 0x07230203U) {
                reason = std::string("no SPIR-V for ") + kernel.Name;
                return {};
            }
            binaries[static_cast<size_t>(kernel.Kernel)] = std::move(words);
        }
    } catch (const std::exception& error) {
        reason = error.what();
        return {};
    }
    return binaries;
}

PassKernelBinaries DiffuseGiKernels::Resolve(CachedPassShaderCompiler& shaders, std::string& reason) {
    return Resolve([&shaders](const PassKernelSource& kernel) {
        return shaders.Resolve(kernel.Source, SpirvStage::Compute, kernel.Name);
    }, reason);
}

bool DiffuseGiKernels::Create(const PassDevice& device, const PassKernelBinaries& binaries) {
    Destroy();
    mDevice = device.Device;
    if (mDevice == VK_NULL_HANDLE) {
        mReason = "no Vulkan device";
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    try {
        mSetLayouts[Gather] = MakeSetLayout(mDevice, kGatherTypes);
        mSetLayouts[Pack] = MakeSetLayout(mDevice, kPackTypes);
        mSetLayouts[Temporal] = MakeSetLayout(mDevice, kTemporalTypes);
        mSetLayouts[Filter] = MakeSetLayout(mDevice, kFilterTypes);
        mSetLayouts[Output] = MakeSetLayout(mDevice, kOutputTypes);
        mSetLayouts[Upsample] = MakeSetLayout(mDevice, kUpsampleTypes);
        for (uint32_t l = 0; l < LayoutCount; ++l) {
            VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            info.setLayoutCount = 1;
            info.pSetLayouts = &mSetLayouts[l];
            Check(vkCreatePipelineLayout(mDevice, &info, nullptr, &mPipelineLayouts[l]), "diffuse GI pipeline layout");
        }
        for (const PassKernelSource& kernel : BuildDiffuseGiPassKernels()) {
            const auto index = static_cast<size_t>(kernel.Kernel);
            const std::vector<uint32_t>& words = binaries[index];
            if (words.size() < 5U || words[0] != 0x07230203U)
                throw std::runtime_error(std::string("no SPIR-V for ") + kernel.Name);
            const auto kernelStart = std::chrono::steady_clock::now();
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            moduleInfo.codeSize = words.size() * sizeof(uint32_t);
            moduleInfo.pCode = words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            Check(vkCreateShaderModule(mDevice, &moduleInfo, nullptr, &module), "diffuse GI shader module");
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                          module, "main", nullptr};
            info.layout = mPipelineLayouts[KernelLayout(kernel.Kernel)];
            const VkResult result =
                vkCreateComputePipelines(mDevice, device.PipelineCache, 1, &info, nullptr, &mPipelines[index]);
            vkDestroyShaderModule(mDevice, module, nullptr);
            Check(result, kernel.Name);
            mKernelMilliseconds[index] =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - kernelStart).count();
        }
    } catch (const std::exception& error) {
        Destroy();
        mReason = error.what();
        return false;
    }
    mCreationMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    mReason.clear();
    mComplete.store(true, std::memory_order_release);
    return true;
}

void DiffuseGiKernels::Destroy() noexcept {
    mComplete.store(false, std::memory_order_release);
    if (mDevice == VK_NULL_HANDLE) return;
    for (VkPipeline& p : mPipelines) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(mDevice, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    for (VkPipelineLayout& l : mPipelineLayouts) {
        if (l != VK_NULL_HANDLE) vkDestroyPipelineLayout(mDevice, l, nullptr);
        l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout& l : mSetLayouts) {
        if (l != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(mDevice, l, nullptr);
        l = VK_NULL_HANDLE;
    }
    mReason = "not created";
}

// ---------------------------------------------------------------------------
// Pass instance.

struct DiffuseGiPass::Impl {
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkPhysicalDeviceLimits Limits{};
    VkPhysicalDeviceMemoryProperties Memory{};
    std::shared_ptr<const DiffuseGiKernels> Kernels;
    std::array<OwnedBuffer, kPassResourceCount> Buffers{};
    VkDescriptorPool Pool = VK_NULL_HANDLE;
    VkDescriptorSet SetGather = VK_NULL_HANDLE, SetPack = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> SetTemporal{}; // [k]: history k in, 1 - k out
    std::array<VkDescriptorSet, 3> SetFilter{};   // level 0; odd levels; even levels >= 2
    struct OutputVariant {
        PassOutputs Outputs;
        std::array<VkDescriptorSet, kColourSources> Output{};   // colour source: Colour, Filter0, Filter1
        std::array<VkDescriptorSet, kColourSources> Upsample{}; // work scale only
    };
    std::vector<OutputVariant> Variants;
    uint32_t CurrentVariant = 0;
    // 1 x 1 R32F image bound when the caller wants no A.
    VkImage PlaceholderImage = VK_NULL_HANDLE;
    VkDeviceMemory PlaceholderMemory = VK_NULL_HANDLE;
    VkImageView PlaceholderView = VK_NULL_HANDLE;
    bool PlaceholderReady = false;
    bool Initialized = false, Configured = false, HasFrame = false;
    PassExtent Extent;
    PassSettings Settings;
    PassInputLayout Inputs;
    PassResourcePlan Plan;
    PassParameterLayout Params;
    TemporalHistoryState History;
    uint32_t Current = 0; // history buffer written by the last frame
    PassFramePlan Frame;
    PassStats Stats;
    std::string Reason = "not initialized";

    // Developer kernel timing: one query pool per ring entry; query 0 is the
    // frame's start, query q > 0 follows the dispatch of TimingKernels[q].
    static constexpr uint32_t kTimingRing = 4;
    static constexpr uint32_t kTimingQueries = 32;
    bool Timing = false;
    mutable bool TimingActive = false;
    mutable uint32_t TimingWritten = 0;
    uint32_t TimingSlot = 0;
    std::array<VkQueryPool, kTimingRing> TimingPools{};
    mutable std::array<std::array<uint8_t, kTimingQueries>, kTimingRing> TimingKernels{};
    std::array<uint32_t, kTimingRing> TimingCounts{};
    std::array<bool, kTimingRing> TimingPending{};
    PassKernelTimings Timings;

    void DestroyTiming() noexcept {
        for (VkQueryPool& pool : TimingPools) {
            if (pool != VK_NULL_HANDLE && Device != VK_NULL_HANDLE) vkDestroyQueryPool(Device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
        TimingCounts = {};
        TimingPending = {};
        TimingActive = false;
    }
    // Reads the ring entry's results when available (never waits), then
    // starts this frame's timestamps in it.
    void BeginTiming(VkCommandBuffer command) {
        TimingActive = false;
        if (!Timing || Device == VK_NULL_HANDLE || Limits.timestampPeriod <= 0.0F) return;
        VkQueryPool& pool = TimingPools[TimingSlot];
        if (pool == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kTimingQueries;
            if (vkCreateQueryPool(Device, &info, nullptr, &pool) != VK_SUCCESS) {
                pool = VK_NULL_HANDLE;
                return;
            }
        } else if (TimingPending[TimingSlot]) {
            const uint32_t count = TimingCounts[TimingSlot];
            std::array<uint64_t, kTimingQueries> ticks{};
            if (count >= 2U && vkGetQueryPoolResults(Device, pool, 0, count, sizeof(ticks), ticks.data(),
                                                     sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                PassKernelTimings t;
                const double scale = static_cast<double>(Limits.timestampPeriod) * 1.0e-6;
                for (uint32_t q = 1; q < count; ++q)
                    t.Milliseconds[TimingKernels[TimingSlot][q]] +=
                        static_cast<double>(ticks[q] - ticks[q - 1U]) * scale;
                t.Total = static_cast<double>(ticks[count - 1U] - ticks[0]) * scale;
                t.Frames = Timings.Frames + 1U;
                t.Valid = true;
                Timings = t;
            }
            TimingPending[TimingSlot] = false;
        }
        vkCmdResetQueryPool(command, pool, 0, kTimingQueries);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
        TimingWritten = 1;
        TimingActive = true;
    }
    void StampAfter(VkCommandBuffer command, PassKernel kernel) const {
        if (!TimingActive || TimingWritten >= kTimingQueries) return;
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, TimingPools[TimingSlot], TimingWritten);
        TimingKernels[TimingSlot][TimingWritten] = static_cast<uint8_t>(kernel);
        ++TimingWritten;
    }
    void EndTiming() {
        if (!TimingActive) return;
        TimingCounts[TimingSlot] = TimingWritten;
        TimingPending[TimingSlot] = true;
        TimingSlot = (TimingSlot + 1U) % kTimingRing;
        TimingActive = false;
    }

    [[nodiscard]] uint32_t Alignment() const noexcept {
        return static_cast<uint32_t>(std::max<VkDeviceSize>(Limits.minUniformBufferOffsetAlignment, 16));
    }
    [[nodiscard]] bool KernelsReady() const noexcept { return Kernels != nullptr && Kernels->Complete(); }

    void DestroyBuffer(PassResourceId id) noexcept {
        OwnedBuffer& b = Buffers[static_cast<size_t>(id)];
        if (b.Handle != VK_NULL_HANDLE) vkDestroyBuffer(Device, b.Handle, nullptr);
        if (b.Memory != VK_NULL_HANDLE) vkFreeMemory(Device, b.Memory, nullptr);
        b = {};
    }

    void CreateBuffer(PassResourceId id, VkDeviceSize size) {
        OwnedBuffer& b = Buffers[static_cast<size_t>(id)];
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     (id == PassResourceId::Parameters ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(Device, &info, nullptr, &b.Handle), "diffuse GI buffer creation");
        b.Size = size;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(Device, b.Handle, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = FindMemoryType(Memory, requirements.memoryTypeBits);
        Check(vkAllocateMemory(Device, &allocation, nullptr, &b.Memory), "diffuse GI memory allocation");
        b.MemorySize = requirements.size;
        Check(vkBindBufferMemory(Device, b.Handle, b.Memory, 0), "diffuse GI memory binding");
    }

    void CreatePlaceholderImage() {
        if (PlaceholderImage != VK_NULL_HANDLE) return;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R32_SFLOAT;
        info.extent = {1, 1, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(vkCreateImage(Device, &info, nullptr, &PlaceholderImage), "diffuse GI placeholder image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(Device, PlaceholderImage, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = FindMemoryType(Memory, requirements.memoryTypeBits);
        Check(vkAllocateMemory(Device, &allocation, nullptr, &PlaceholderMemory), "diffuse GI placeholder memory");
        Check(vkBindImageMemory(Device, PlaceholderImage, PlaceholderMemory, 0), "diffuse GI placeholder binding");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = PlaceholderImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R32_SFLOAT;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(Device, &view, nullptr, &PlaceholderView), "diffuse GI placeholder view");
        PlaceholderReady = false;
    }

    void DestroyPlaceholderImage() noexcept {
        if (PlaceholderView != VK_NULL_HANDLE) vkDestroyImageView(Device, PlaceholderView, nullptr);
        if (PlaceholderImage != VK_NULL_HANDLE) vkDestroyImage(Device, PlaceholderImage, nullptr);
        if (PlaceholderMemory != VK_NULL_HANDLE) vkFreeMemory(Device, PlaceholderMemory, nullptr);
        PlaceholderView = VK_NULL_HANDLE;
        PlaceholderImage = VK_NULL_HANDLE;
        PlaceholderMemory = VK_NULL_HANDLE;
        PlaceholderReady = false;
    }

    void DestroySets() noexcept {
        if (Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(Device, Pool, nullptr);
        Pool = VK_NULL_HANDLE;
        SetGather = SetPack = VK_NULL_HANDLE;
        SetTemporal = {};
        SetFilter = {};
        Variants.clear();
        CurrentVariant = 0;
    }

    void ReleaseResources() noexcept {
        DestroySets();
        for (uint32_t id = 0; id < kPassResourceCount; ++id) DestroyBuffer(static_cast<PassResourceId>(id));
        DestroyPlaceholderImage();
        Configured = false;
        HasFrame = false;
        Stats.BufferBytes = Stats.MemoryBytes = 0;
        Stats.OutputSets = 0;
    }

    [[nodiscard]] bool Allocated(PassResourceId id) const noexcept {
        return Buffers[static_cast<size_t>(id)].Handle != VK_NULL_HANDLE;
    }

    // Storage binding of a buffer, or the placeholder where the pass has none.
    [[nodiscard]] VkDescriptorBufferInfo Storage(PassResourceId id) const noexcept {
        const PassResourceId used = Allocated(id) ? id : PassResourceId::Placeholder;
        return {Buffers[static_cast<size_t>(used)].Handle, 0, VK_WHOLE_SIZE};
    }
    [[nodiscard]] VkDescriptorBufferInfo Block(uint32_t offset, VkDeviceSize range) const noexcept {
        return {Buffers[static_cast<size_t>(PassResourceId::Parameters)].Handle, offset, range};
    }

    void WriteSet(VkDescriptorSet set, const VkDescriptorType* types, const std::vector<VkDescriptorBufferInfo>& buffers,
                  const std::vector<VkDescriptorImageInfo>& images) {
        std::vector<VkWriteDescriptorSet> writes;
        uint32_t binding = 0;
        for (const VkDescriptorBufferInfo& info : buffers) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = types[binding];
            w.pBufferInfo = &info;
            writes.push_back(w);
            ++binding;
        }
        for (const VkDescriptorImageInfo& info : images) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = types[binding];
            w.pImageInfo = &info;
            writes.push_back(w);
            ++binding;
        }
        vkUpdateDescriptorSets(Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    VkDescriptorSet AllocateSet(DiffuseGiKernels::Layout layout) {
        const VkDescriptorSetLayout setLayout = Kernels->SetLayout(layout);
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = Pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Check(vkAllocateDescriptorSets(Device, &info, &set), "diffuse GI descriptor set");
        return set;
    }

    void BuildBufferSets() {
        DestroySets();
        using enum PassResourceId;
        // Room for the buffer sets and kOutputVariants output variants of
        // three output and three upsample sets each.
        const uint32_t variantSets = kOutputVariants * 2U * kColourSources;
        const uint32_t sets = 2U + 2U + 3U + variantSets;
        const std::array<VkDescriptorPoolSize, 4> sizes{{
            {kUbo, 4},
            {kDyn, 1U + 3U + variantSets},
            {kSsbo, 12U + 14U + 2U * 12U + 6U * 3U + kOutputVariants * kColourSources * (2U + 5U)},
            {kImg, variantSets * 2U},
        }};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = sets;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(Device, &pool, nullptr, &Pool), "diffuse GI descriptor pool");
        const VkDescriptorBufferInfo gatherBlock = Block(Params.Gather, sizeof(GatherShaderParams));
        SetGather = AllocateSet(DiffuseGiKernels::Gather);
        WriteSet(SetGather, kGatherTypes.data(),
                 {gatherBlock, Storage(Depth), Storage(Class), Storage(Radiance), Storage(Normal), Storage(Results),
                  Storage(Aux), Block(0, sizeof(M4ShaderParams)), Storage(Cells), Storage(Placeholder),
                  Storage(Placeholder), Storage(Placeholder), Storage(Placeholder), Storage(ShadingNormal)},
                 {});
        SetPack = AllocateSet(DiffuseGiKernels::Pack);
        WriteSet(SetPack, kPackTypes.data(),
                 {gatherBlock, Storage(Depth), Storage(Class), Storage(Radiance), Storage(Normal), Storage(Results),
                  Storage(Aux), Storage(Geometry), Storage(Placeholder), Storage(Placeholder), Storage(Placeholder),
                  Storage(Placeholder), Storage(Placeholder), Storage(ShadingNormal), Storage(Shading)},
                 {});
        for (uint32_t k = 0; k < 2; ++k) {
            SetTemporal[k] = AllocateSet(DiffuseGiKernels::Temporal);
            WriteSet(SetTemporal[k], kTemporalTypes.data(),
                     {Block(Params.Temporal, sizeof(TemporalShaderParams)), Storage(Results), Storage(Geometry),
                      Storage(Motion), Storage(PreviousPosition), Storage(SurfaceId),
                      Storage(k == 0 ? History0 : History1), Storage(k == 0 ? History1 : History0), Storage(Colour),
                      Storage(Variance), Storage(Diag), Storage(Diag2), Storage(Shading)},
                     {});
        }
        if (Allocated(Filter0)) {
            const std::array<std::array<PassResourceId, 4>, 3> wiring{{
                {Colour, Variance, Filter0, FilterVariance0},
                {Filter0, FilterVariance0, Filter1, FilterVariance1},
                {Filter1, FilterVariance1, Filter0, FilterVariance0},
            }};
            for (uint32_t k = 0; k < 3; ++k) {
                SetFilter[k] = AllocateSet(DiffuseGiKernels::Filter);
                WriteSet(SetFilter[k], kFilterTypes.data(),
                         {Block(0, sizeof(FilterShaderParams)), Storage(Geometry), Storage(wiring[k][0]),
                          Storage(wiring[k][1]), Storage(wiring[k][2]), Storage(wiring[k][3]), Storage(Shading)},
                         {});
            }
        }
    }

    // Index of the cached output variant of these images, or the count when absent.
    [[nodiscard]] uint32_t FindVariant(const PassOutputs& outputs) const noexcept {
        for (uint32_t i = 0; i < Variants.size(); ++i)
            if (Variants[i].Outputs.Illumination.View == outputs.Illumination.View &&
                Variants[i].Outputs.Accessibility.View == outputs.Accessibility.View &&
                Variants[i].Outputs.Illumination.Image == outputs.Illumination.Image &&
                Variants[i].Outputs.Accessibility.Image == outputs.Accessibility.Image)
                return i;
        return static_cast<uint32_t>(Variants.size());
    }

    uint32_t AddVariant(const PassOutputs& outputs) {
        using enum PassResourceId;
        OutputVariant variant;
        variant.Outputs = outputs;
        const VkImageView accessibility =
            outputs.Accessibility.View != VK_NULL_HANDLE ? outputs.Accessibility.View : PlaceholderView;
        const VkDescriptorImageInfo j{VK_NULL_HANDLE, outputs.Illumination.View, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo a{VK_NULL_HANDLE, accessibility, VK_IMAGE_LAYOUT_GENERAL};
        const std::array<PassResourceId, kColourSources> sources{
            Colour, Allocated(Filter0) ? Filter0 : Colour, Allocated(Filter1) ? Filter1 : Colour};
        for (uint32_t k = 0; k < kColourSources; ++k) {
            variant.Output[k] = AllocateSet(DiffuseGiKernels::Output);
            WriteSet(variant.Output[k], kOutputTypes.data(),
                     {Block(0, sizeof(OutputShaderParams)), Storage(sources[k]), Storage(Geometry)}, {j, a});
            if (Extent.WorkFactor > 1U) {
                variant.Upsample[k] = AllocateSet(DiffuseGiKernels::Upsample);
                WriteSet(variant.Upsample[k], kUpsampleTypes.data(),
                         {Block(0, sizeof(UpsampleShaderParams)), Storage(sources[k]), Storage(Geometry),
                          Storage(OutputGuide), Storage(Shading), Storage(OutputShading)},
                         {j, a});
            }
        }
        Variants.push_back(variant);
        Stats.OutputSets = static_cast<uint32_t>(Variants.size());
        return static_cast<uint32_t>(Variants.size() - 1U);
    }

    // --- Recording helpers.
    void Barrier(VkCommandBuffer command, VkPipelineStageFlags srcStages, VkAccessFlags src,
                 VkPipelineStageFlags dstStages, VkAccessFlags dst, PassBarrierRecord& record) const {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = src;
        barrier.dstAccessMask = dst;
        vkCmdPipelineBarrier(command, srcStages, dstStages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        ++record.PrivateBarriers;
    }
    void ComputeBarrier(VkCommandBuffer command, PassBarrierRecord& record) const {
        Barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, record);
    }
    // Producers of this frame (adapter compute or transfer writes, the previous
    // frame's history writes) -> reads; earlier reads of the parameters -> update.
    void UpdateParameters(VkCommandBuffer command, uint32_t offset, const void* data, uint32_t size,
                          PassBarrierRecord& record) const {
        Barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                    VK_ACCESS_UNIFORM_READ_BIT,
                record);
        vkCmdUpdateBuffer(command, Buffers[static_cast<size_t>(PassResourceId::Parameters)].Handle, offset, size, data);
        Barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_UNIFORM_READ_BIT, record);
    }
    void Dispatch(VkCommandBuffer command, PassKernel kernel, VkDescriptorSet set, const uint32_t* dynamicOffset,
                  uint32_t width, uint32_t height, PassBarrierRecord& record) const {
        const auto layout = DiffuseGiKernels::KernelLayout(kernel);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, Kernels->Pipeline(kernel));
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, Kernels->PipelineLayout(layout), 0, 1, &set,
                                dynamicOffset != nullptr ? 1U : 0U, dynamicOffset);
        vkCmdDispatch(command, std::max(Groups(width), 1U), std::max(Groups(height), 1U), 1);
        ++record.Dispatches;
        StampAfter(command, kernel);
    }
    static void Transition(VkCommandBuffer command, VkImage image, const PassImageState& from,
                           const PassImageState& to) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = from.Access;
        barrier.dstAccessMask = to.Access;
        barrier.oldLayout = from.Layout;
        barrier.newLayout = to.Layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, from.Stages != 0U ? from.Stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             to.Stages != 0U ? to.Stages : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
    }
    // The output (or upsample) kernel between its graph-visible transitions.
    void RecordOutput(VkCommandBuffer command, PassKernel kernel, VkDescriptorSet set, uint32_t offset,
                      PassImageState& illumination, PassImageState& accessibility,
                      const PassOutputAccess& completion, PassBarrierRecord& record) {
        const OutputVariant& variant = Variants[CurrentVariant];
        const bool ownA = variant.Outputs.Accessibility.Image != VK_NULL_HANDLE;
        if (!ownA && !PlaceholderReady) {
            Transition(command, PlaceholderImage, PassImageState{}, kPassOutputWriteState);
            ++record.PrivateBarriers;
            PlaceholderReady = true;
        }
        if (!completion.ExternalTransitions) {
            record.PlannedOutputTransitions += ownA ? 4U : 2U;
            Transition(command, variant.Outputs.Illumination.Image, illumination, kPassOutputWriteState);
            ++record.EmittedOutputTransitions;
            if (ownA) {
                Transition(command, variant.Outputs.Accessibility.Image, accessibility, kPassOutputWriteState);
                ++record.EmittedOutputTransitions;
            }
        }
        const auto extent = PassOutputExtent(Extent);
        Dispatch(command, kernel, set, &offset, extent[0], extent[1], record);
        if (completion.ExternalTransitions) return;
        const auto complete = [&](VkImage image, PassImageState& state, const PassImageState& target) {
            if (target == kPassOutputWriteState) {
                ++record.ElidedOutputTransitions;
            } else {
                Transition(command, image, kPassOutputWriteState, target);
                ++record.EmittedOutputTransitions;
            }
            state = target;
        };
        complete(variant.Outputs.Illumination.Image, illumination, completion.Illumination);
        if (ownA) complete(variant.Outputs.Accessibility.Image, accessibility, completion.Accessibility);
    }

    [[nodiscard]] uint32_t ColourSource(uint32_t levels) const noexcept {
        return levels == 0U ? 0U : ((levels - 1U) % 2U == 0U ? 1U : 2U);
    }

    bool Fail(const std::string& reason) {
        Reason = reason;
        ++Stats.Failures;
        return false;
    }
};

DiffuseGiPass::DiffuseGiPass() : mImpl(std::make_unique<Impl>()) {}
DiffuseGiPass::~DiffuseGiPass() { Shutdown(); }

bool DiffuseGiPass::Initialize(const PassDevice& device, std::shared_ptr<const DiffuseGiKernels> kernels) {
    Shutdown();
    Impl& m = *mImpl;
    m.Physical = device.Physical;
    m.Device = device.Device;
    std::string reason;
    if (m.Device == VK_NULL_HANDLE || !DiffuseGiPassSupported(m.Physical, reason)) {
        m.Reason = m.Device == VK_NULL_HANDLE ? "no Vulkan device" : reason;
        return false;
    }
    if (kernels == nullptr) {
        m.Reason = "no kernels";
        return false;
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(m.Physical, &properties);
    m.Limits = properties.limits;
    vkGetPhysicalDeviceMemoryProperties(m.Physical, &m.Memory);
    m.Kernels = std::move(kernels);
    m.Stats.KernelModules = m.Kernels->Complete() ? kPassKernelCount : 0U;
    m.Initialized = true;
    m.Reason = "not configured";
    return true;
}

bool DiffuseGiPass::Initialize(const PassDevice& device, CachedPassShaderCompiler& shaders) {
    return Initialize(device, [&shaders](const PassKernelSource& kernel) {
        return shaders.Resolve(kernel.Source, SpirvStage::Compute, kernel.Name);
    });
}

bool DiffuseGiPass::Initialize(const PassDevice& device, const PassShaderResolver& resolver) {
    Shutdown();
    std::string reason;
    if (device.Device == VK_NULL_HANDLE || !DiffuseGiPassSupported(device.Physical, reason)) {
        mImpl->Reason = device.Device == VK_NULL_HANDLE ? "no Vulkan device" : reason;
        return false;
    }
    const PassKernelBinaries binaries = DiffuseGiKernels::Resolve(resolver, reason);
    if (!reason.empty()) {
        mImpl->Reason = reason;
        return false;
    }
    auto kernels = std::make_shared<DiffuseGiKernels>();
    if (!kernels->Create(device, binaries)) {
        mImpl->Reason = kernels->Reason();
        return false;
    }
    return Initialize(device, std::shared_ptr<const DiffuseGiKernels>(std::move(kernels)));
}

bool DiffuseGiPass::ReplacesResources(const PassExtent& extent, const PassSettings& settings,
                                      const PassInputLayout& inputs, const PassOutputs& outputs) const {
    const Impl& m = *mImpl;
    if (!m.Configured) return false;
    const PassResourcePlan plan = PlanPassResources(extent, settings, inputs, m.Alignment());
    for (uint32_t id = 0; id < kPassResourceCount; ++id)
        if (plan.Buffers[id].Bytes != m.Buffers[id].Size) return true;
    if (extent.WorkFactor != m.Extent.WorkFactor) return true;
    return m.FindVariant(outputs) == m.Variants.size() && m.Variants.size() >= kOutputVariants;
}

bool DiffuseGiPass::Configure(const PassExtent& extent, const PassSettings& settings, const PassInputLayout& inputs,
                              const PassOutputs& outputs) {
    Impl& m = *mImpl;
    if (!m.Initialized) return m.Fail("not initialized");
    if (!m.Kernels->Complete()) return m.Fail(std::string("kernels not ready: ") + m.Kernels->Reason());
    if (const char* e = ValidatePassExtent(extent); *e != '\0') return m.Fail(e);
    if (const char* e = ValidatePassSettings(settings); *e != '\0') return m.Fail(e);
    const auto outputExtent = PassOutputExtent(extent);
    const auto checkOutput = [&](const PassImageBinding& image, VkFormat format) {
        return image.Image != VK_NULL_HANDLE && image.View != VK_NULL_HANDLE && image.Format == format &&
               image.Width == outputExtent[0] && image.Height == outputExtent[1];
    };
    if (!checkOutput(outputs.Illumination, VK_FORMAT_R16G16B16A16_SFLOAT) ||
        (outputs.Accessibility.Image != VK_NULL_HANDLE && !checkOutput(outputs.Accessibility, VK_FORMAT_R32_SFLOAT)))
        return m.Fail("output images must be R16G16B16A16_SFLOAT (and R32_SFLOAT) at the output extent");
    const PassResourcePlan plan = PlanPassResources(extent, settings, inputs, m.Alignment());
    for (const PassResource& r : plan.Buffers)
        if (r.Allocated && r.Id != PassResourceId::Parameters && r.Bytes > m.Limits.maxStorageBufferRange)
            return m.Fail(std::string("buffer '") + r.Name + "' exceeds the device's storage buffer range");
    if (Groups(std::max(extent.BufferWidth, outputExtent[0])) > m.Limits.maxComputeWorkGroupCount[0] ||
        Groups(std::max(extent.BufferHeight, outputExtent[1])) > m.Limits.maxComputeWorkGroupCount[1])
        return m.Fail("extent exceeds the device's dispatch limits");
    const PassParameterLayout params = MakePassParameterLayout(m.Alignment());
    if (params.Size > kUpdateLimit) return m.Fail("parameter blocks exceed one command-stream update");

    const bool rebuild = !m.Configured || extent.WorkFactor != m.Extent.WorkFactor ||
                         (m.FindVariant(outputs) == m.Variants.size() && m.Variants.size() >= kOutputVariants);
    bool replaced = false, historyReplaced = false;
    try {
        for (uint32_t index = 0; index < kPassResourceCount; ++index) {
            const PassResource& r = plan.Buffers[index];
            const auto id = static_cast<PassResourceId>(index);
            if (r.Bytes == m.Buffers[index].Size) continue;
            m.DestroyBuffer(id);
            if (r.Allocated) m.CreateBuffer(id, r.Bytes);
            replaced = true;
            historyReplaced = historyReplaced || r.Lifetime == PassLifetime::Persistent;
        }
        if (outputs.Accessibility.Image == VK_NULL_HANDLE) m.CreatePlaceholderImage();
        m.Extent = extent;
        m.Settings = settings;
        m.Inputs = inputs;
        m.Plan = plan;
        m.Params = params;
        if (replaced || rebuild) {
            m.BuildBufferSets();
            m.HasFrame = false;
        }
        uint32_t variant = m.FindVariant(outputs);
        if (variant == m.Variants.size()) variant = m.AddVariant(outputs);
        m.CurrentVariant = variant;
    } catch (const std::exception& error) {
        m.ReleaseResources();
        if (m.History.HasHistory()) m.History.Invalidate();
        return m.Fail(error.what());
    }
    // Replaced history buffers hold no history: the next frame resets
    // (kResetExplicit, resource invalidation), whatever the extents. Before
    // the first frame there is nothing to invalidate (kResetFirstFrame).
    if (historyReplaced && m.History.HasHistory()) m.History.Invalidate();
    m.Stats.BufferBytes = m.Stats.MemoryBytes = 0;
    for (const OwnedBuffer& b : m.Buffers) {
        m.Stats.BufferBytes += b.Size;
        m.Stats.MemoryBytes += b.MemorySize;
    }
    ++m.Stats.Configurations;
    if (replaced) ++m.Stats.Reallocations;
    m.Stats.KernelModules = kPassKernelCount;
    m.Configured = true;
    m.Reason.clear();
    return true;
}

DiffuseGiPass::BufferView DiffuseGiPass::Buffer(PassResourceId id) const noexcept {
    if (id == PassResourceId::Count) return {};
    const OwnedBuffer& b = mImpl->Buffers[static_cast<size_t>(id)];
    return {b.Handle, b.Size};
}

uint32_t DiffuseGiPass::LatestHistory() const noexcept { return mImpl->Current; }

bool DiffuseGiPass::Execute(VkCommandBuffer command, const PassFrame& frame, PassImageState& illumination,
                            PassImageState& accessibility, const PassOutputAccess& completion,
                            PassBarrierRecord& record) {
    Impl& m = *mImpl;
    record = {};
    if (!m.Configured) return m.Fail(m.Initialized ? "not configured" : "not initialized");
    if (!m.KernelsReady()) return m.Fail("kernels not ready");
    if (command == VK_NULL_HANDLE) return m.Fail("no command buffer");
    const bool writeA = m.Variants[m.CurrentVariant].Outputs.Accessibility.Image != VK_NULL_HANDLE;
    PassFramePlan plan;
    if (const char* e = PlanPassFrame(m.Extent, m.Settings, m.Inputs, m.Params, m.History, frame, plan, writeA);
        *e != '\0')
        return m.Fail(e);

    using enum PassKernel;
    const ViewportRect& v = m.Extent.Viewport;
    m.BeginTiming(command);
    m.UpdateParameters(command, 0, plan.Parameters.data(), m.Params.Size, record);
    m.Dispatch(command, Normal, m.SetGather, &m.Params.M4[0], v.Width, v.Height, record);
    for (uint32_t l = 0; l < plan.HierarchyLevels; ++l) {
        m.ComputeBarrier(command, record);
        m.Dispatch(command, l == 0 ? HierarchyLevel0 : HierarchyReduce, m.SetGather, &m.Params.M4[l],
                   plan.LevelExtent[l][0], plan.LevelExtent[l][1], record);
    }
    m.ComputeBarrier(command, record);
    m.Dispatch(command, Gather, m.SetGather, &m.Params.M4[0], v.Width, v.Height, record);
    m.ComputeBarrier(command, record);
    m.Dispatch(command, Pack, m.SetPack, nullptr, v.Width, v.Height, record);
    m.ComputeBarrier(command, record);
    m.Dispatch(command, Temporal, m.SetTemporal[m.Current], nullptr, v.Width, v.Height, record);
    for (uint32_t l = 0; l < plan.FilterLevels; ++l) {
        m.ComputeBarrier(command, record);
        const VkDescriptorSet set = m.SetFilter[l == 0 ? 0 : (l % 2 == 1 ? 1 : 2)];
        m.Dispatch(command, Filter, set, &m.Params.Filter[l], v.Width, v.Height, record);
    }
    m.ComputeBarrier(command, record);
    const uint32_t source = m.ColourSource(plan.FilterLevels);
    const auto& variant = m.Variants[m.CurrentVariant];
    if (m.Extent.WorkFactor > 1U)
        m.RecordOutput(command, Upsample, variant.Upsample[source], m.Params.Upsample, illumination, accessibility,
                       completion, record);
    else
        m.RecordOutput(command, Output, variant.Output[source], m.Params.Output, illumination, accessibility,
                       completion, record);
    m.EndTiming();

    m.History.Commit(plan.Temporal);
    m.Current = 1U - m.Current;
    m.HasFrame = true;
    ++m.Stats.Executions;
    m.Stats.LastResetReasons = plan.ResetReasons;
    if (plan.ResetReasons != 0U) ++m.Stats.ResetFrames;
    for (uint32_t bit = 0; bit < 8; ++bit)
        if ((plan.ResetReasons & (1U << bit)) != 0U) ++m.Stats.ResetsByReason[bit];
    m.Frame = std::move(plan);
    return true;
}

bool DiffuseGiPass::RepublishOutputs(VkCommandBuffer command, PassImageState& illumination,
                                     PassImageState& accessibility, const PassOutputAccess& completion,
                                     PassBarrierRecord& record) {
    Impl& m = *mImpl;
    record = {};
    if (!m.Configured) return m.Fail(m.Initialized ? "not configured" : "not initialized");
    if (!m.KernelsReady()) return m.Fail("kernels not ready");
    if (!m.HasFrame) return m.Fail("no frame to republish");
    if (command == VK_NULL_HANDLE) return m.Fail("no command buffer");
    // The last frame's colour, geometry and output guide are still in place;
    // their writes (and this instance's earlier output reads) precede this.
    m.ComputeBarrier(command, record);
    const uint32_t source = m.ColourSource(m.Frame.FilterLevels);
    const auto& variant = m.Variants[m.CurrentVariant];
    if (m.Extent.WorkFactor > 1U)
        m.RecordOutput(command, PassKernel::Upsample, variant.Upsample[source], m.Params.Upsample, illumination,
                       accessibility, completion, record);
    else
        m.RecordOutput(command, PassKernel::Output, variant.Output[source], m.Params.Output, illumination,
                       accessibility, completion, record);
    ++m.Stats.Republications;
    return true;
}

bool DiffuseGiPass::ClearOutputs(VkCommandBuffer command, PassImageState& illumination, PassImageState& accessibility,
                                 const PassOutputAccess& completion, PassBarrierRecord& record) {
    Impl& m = *mImpl;
    record = {};
    if (!m.Configured) return m.Fail(m.Initialized ? "not configured" : "not initialized");
    if (!m.KernelsReady()) return m.Fail("kernels not ready");
    if (command == VK_NULL_HANDLE) return m.Fail("no command buffer");
    const bool writeA = m.Variants[m.CurrentVariant].Outputs.Accessibility.Image != VK_NULL_HANDLE;
    const OutputShaderParams neutral = MakeOutputShaderParams(m.Extent, kOutputModeNeutral, writeA);
    m.UpdateParameters(command, m.Params.OutputNeutral, &neutral, sizeof(neutral), record);
    m.RecordOutput(command, PassKernel::Output, m.Variants[m.CurrentVariant].Output[0], m.Params.OutputNeutral,
                   illumination, accessibility, completion, record);
    // A frame without GI breaks the one-frame motion chain: the next frame resets.
    m.History.Invalidate();
    m.HasFrame = false;
    ++m.Stats.Clears;
    return true;
}

void DiffuseGiPass::ResetHistory() noexcept {
    mImpl->History.Invalidate();
    ++mImpl->Stats.HistoryResets;
}

void DiffuseGiPass::InvalidateScreenResources() noexcept {
    Impl& m = *mImpl;
    if (m.Device != VK_NULL_HANDLE) m.ReleaseResources();
    m.History.Invalidate();
    ++m.Stats.Invalidations;
    if (m.Initialized) m.Reason = "screen resources invalidated";
}

void DiffuseGiPass::Shutdown() noexcept {
    Impl& m = *mImpl;
    if (m.Device != VK_NULL_HANDLE) m.ReleaseResources();
    m.DestroyTiming();
    m.Kernels.reset();
    m.Initialized = false;
    m.Configured = false;
    m.Stats.KernelModules = 0;
    m.Reason = "not initialized";
}

bool DiffuseGiPass::Available() const noexcept { return mImpl->Configured && mImpl->KernelsReady(); }
const std::string& DiffuseGiPass::UnavailableReason() const noexcept { return mImpl->Reason; }
const PassStats& DiffuseGiPass::Stats() const noexcept { return mImpl->Stats; }
const PassResourcePlan& DiffuseGiPass::Resources() const noexcept { return mImpl->Plan; }
const PassParameterLayout& DiffuseGiPass::ParameterLayout() const noexcept { return mImpl->Params; }
const PassFramePlan& DiffuseGiPass::LastFrame() const noexcept { return mImpl->Frame; }
const DiffuseGiKernels* DiffuseGiPass::Kernels() const noexcept { return mImpl->Kernels.get(); }

void DiffuseGiPass::SetKernelTiming(bool enabled) noexcept {
    Impl& m = *mImpl;
    if (m.Timing == enabled) return;
    m.Timing = enabled;
    if (!enabled) m.DestroyTiming();
}
const PassKernelTimings& DiffuseGiPass::LastKernelTimings() const noexcept { return mImpl->Timings; }

} // namespace Fast::Renderer::DiffuseGi
