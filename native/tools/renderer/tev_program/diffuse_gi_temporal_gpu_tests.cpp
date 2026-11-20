// GPU twin of the diffuse GI temporal accumulation and a-trous filter
// (milestone M3, docs/TRIAEVUM_SCREEN_SPACE_GI.md): the GLSL kernels compiled at
// run time run over frame sequences and are compared with the FP32 CPU twin,
// which reads the same constant blocks. T16: odd extents and partial 8 x 8
// workgroups, extreme counts, poisoned history at reset and non-finite inputs,
// guard sentinels after every output, the Khronos validation layer with
// synchronization validation (a deliberately missing barrier must be
// reported), and a full chain normal -> gather -> pack -> temporal -> filter.
// Lines starting with DGI_GPU_M3 are quoted in the document.
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_gather_shader.h"
#include "fast/renderer/diffuse_gi_scene.h"
#include "fast/renderer/diffuse_gi_sequence.h"
#include "fast/renderer/diffuse_gi_temporal.h"
#include "fast/renderer/diffuse_gi_temporal_shader.h"

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}

constexpr float kSentinel = 12345.0F;
constexpr uint32_t kGuard = 64; // vec4 elements after every output

uint32_t gErrors = 0, gSyncHazards = 0;
std::string gFirstMessage;

uint32_t gLoaderMessages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    // Loader messages (e.g. a third-party implicit layer that fails to load on
    // the host) are environmental: counted apart from validation messages.
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        ++gLoaderMessages;
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++gErrors;
        const std::string id = data->pMessageIdName ? data->pMessageIdName : "";
        if (id.find("SYNC-") != std::string::npos) ++gSyncHazards;
        if (gFirstMessage.empty()) gFirstMessage = data->pMessage ? data->pMessage : id;
    }
    return VK_FALSE;
}

// Test device with the validation layer and synchronization validation when
// the layer is installed.
struct Device {
    VkInstance Instance{};
    VkPhysicalDevice Physical{};
    VkDevice Handle{};
    VkQueue Queue{};
    uint32_t Family = 0;
    VkPhysicalDeviceMemoryProperties Memory{};
    VkDebugUtilsMessengerEXT Messenger{};
    bool Validation = false;
    struct Buffer {
        VkBuffer Handle{};
        VkDeviceMemory Memory{};
        void* Mapped{};
        VkDeviceSize Size = 0;
    };

    Device() {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (const auto& l : layers) Validation = Validation || std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_1;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
                                    VK_EXT_LAYER_SETTINGS_EXTENSION_NAME};
        const VkBool32 enabled = VK_TRUE;
        const VkLayerSettingEXT setting{"VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
                                        &enabled};
        VkLayerSettingsCreateInfoEXT settings{VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
        settings.settingCount = 1;
        settings.pSettings = &setting;
        VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = OnMessage;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        features.enabledValidationFeatureCount = 1;
        features.pEnabledValidationFeatures = enables;
        features.pNext = &settings;
        settings.pNext = &messenger;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (Validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layer;
            info.enabledExtensionCount = 3;
            info.ppEnabledExtensionNames = extensions;
            info.pNext = &features;
        }
        Check(vkCreateInstance(&info, nullptr, &Instance));
        if (Validation) {
            const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(Instance, "vkCreateDebugUtilsMessengerEXT"));
            Require(create != nullptr, "debug utils unavailable");
            Check(create(Instance, &messenger, nullptr, &Messenger));
        }
        Check(vkEnumeratePhysicalDevices(Instance, &count, nullptr));
        Require(count > 0, "no Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(Instance, &count, devices.data()));
        Physical = devices[0];
        vkGetPhysicalDeviceMemoryProperties(Physical, &Memory);
        vkGetPhysicalDeviceQueueFamilyProperties(Physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(Physical, &count, queues.data());
        while (Family < count && !(queues[Family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++Family;
        Require(Family < count, "no compute queue");
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = Family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        d.queueCreateInfoCount = 1;
        d.pQueueCreateInfos = &q;
        Check(vkCreateDevice(Physical, &d, nullptr, &Handle));
        vkGetDeviceQueue(Handle, Family, 0, &Queue);
    }
    ~Device() {
        if (Handle) vkDestroyDevice(Handle, nullptr);
        if (Messenger) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(Instance, Messenger, nullptr);
        }
        if (Instance) vkDestroyInstance(Instance, nullptr);
    }
    Buffer Make(VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b;
        b.Size = std::max<VkDeviceSize>(size, 16);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = b.Size;
        info.usage = usage;
        Check(vkCreateBuffer(Handle, &info, nullptr, &b.Handle));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(Handle, b.Handle, &req);
        uint32_t type = 0;
        const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (type < Memory.memoryTypeCount &&
               !((req.memoryTypeBits & (1U << type)) && (Memory.memoryTypes[type].propertyFlags & flags) == flags))
            ++type;
        Require(type < Memory.memoryTypeCount, "no host-visible memory");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = type;
        Check(vkAllocateMemory(Handle, &alloc, nullptr, &b.Memory));
        Check(vkBindBufferMemory(Handle, b.Handle, b.Memory, 0));
        Check(vkMapMemory(Handle, b.Memory, 0, b.Size, 0, &b.Mapped));
        return b;
    }
    void Free(Buffer& b) {
        if (!b.Handle) return;
        vkUnmapMemory(Handle, b.Memory);
        vkDestroyBuffer(Handle, b.Handle, nullptr);
        vkFreeMemory(Handle, b.Memory, nullptr);
        b = {};
    }
    VkShaderModule Shader(const std::string& source) {
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
        const auto spirv = compiler.CompileGlslToSpv(source, shaderc_compute_shader, "dgi_m3", options);
        Require(spirv.GetCompilationStatus() == shaderc_compilation_status_success, spirv.GetErrorMessage().c_str());
        const std::vector<uint32_t> words(spirv.cbegin(), spirv.cend());
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = words.size() * 4;
        info.pCode = words.data();
        VkShaderModule module{};
        Check(vkCreateShaderModule(Handle, &info, nullptr, &module));
        return module;
    }
};

struct Pipeline {
    VkDescriptorSetLayout SetLayout{};
    VkPipelineLayout Layout{};
    std::vector<VkPipeline> Handles;
    uint32_t Bindings = 0;
};

class Runner {
  public:
    explicit Runner(Device& d) : mDevice(d) {
        mGather = Make(8, {BuildDiffuseGiNormalShader(), BuildDiffuseGiGatherShader(), BuildDiffuseGiPackShader()});
        mTemporal = Make(12, {BuildDiffuseGiTemporalShader()});
        mFilter = Make(6, {BuildDiffuseGiFilterShader()});
        const std::array<VkDescriptorPoolSize, 2> sizes{
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 32;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(d.Handle, &pool, nullptr, &mPool));
        VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commands.queueFamilyIndex = d.Family;
        Check(vkCreateCommandPool(d.Handle, &commands, nullptr, &mCommands));
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = mCommands;
        command.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(d.Handle, &command, &mCommand));
    }
    ~Runner() {
        vkDestroyCommandPool(mDevice.Handle, mCommands, nullptr);
        vkDestroyDescriptorPool(mDevice.Handle, mPool, nullptr);
        for (auto* p : {&mGather, &mTemporal, &mFilter}) {
            for (auto h : p->Handles) vkDestroyPipeline(mDevice.Handle, h, nullptr);
            vkDestroyPipelineLayout(mDevice.Handle, p->Layout, nullptr);
            vkDestroyDescriptorSetLayout(mDevice.Handle, p->SetLayout, nullptr);
        }
    }

    // One temporal frame plus the filter levels on the GPU. `raw` and
    // `geometry` are either uploaded (twin comparison) or already filled on
    // the GPU (full chain). History buffers persist between calls.
    struct Frame {
        uint32_t Width = 0, Height = 0;
        const TemporalShaderParams* Params = nullptr;
        std::vector<FilterShaderParams> Levels;
        bool MissingBarrier = false; // negative control for synchronization validation
    };
    struct Result {
        std::vector<float> Colour, Variance, Diag, Diag2, Filtered, FilteredVariance;
        bool GuardsIntact = true;
    };

    void Allocate(uint32_t w, uint32_t h, bool poisonHistory) {
        Release();
        const size_t pixels = static_cast<size_t>(w) * h;
        const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        Raw = mDevice.Make(pixels * 16, storage);
        Geometry = mDevice.Make(pixels * 16, storage);
        Motion = mDevice.Make(pixels * 8, storage);
        Previous = mDevice.Make(pixels * 16, storage);
        Ids = mDevice.Make(pixels * 4, storage);
        for (auto& b : History) {
            b = mDevice.Make(pixels * 64, storage);
            std::fill_n(static_cast<float*>(b.Mapped), pixels * 16,
                        poisonHistory ? std::numeric_limits<float>::quiet_NaN() : 0.0F);
        }
        Colour = mDevice.Make((pixels + kGuard) * 16, storage);
        Variance = mDevice.Make((pixels + kGuard) * 8, storage);
        Diag = mDevice.Make((pixels + kGuard) * 16, storage);
        Diag2 = mDevice.Make((pixels + kGuard) * 16, storage);
        for (auto& b : Ping) b = mDevice.Make((pixels + kGuard) * 16, storage);
        for (auto& b : PingVariance) b = mDevice.Make((pixels + kGuard) * 8, storage);
        Uniform = mDevice.Make(sizeof(TemporalShaderParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        for (auto& b : LevelUniform) b = mDevice.Make(sizeof(FilterShaderParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        mCurrent = 0;
    }

    void Upload(const TemporalFrame& f) {
        const size_t pixels = static_cast<size_t>(f.Width) * f.Height;
        auto* raw = static_cast<float*>(Raw.Mapped);
        auto* geo = static_cast<float*>(Geometry.Mapped);
        auto* motion = static_cast<float*>(Motion.Mapped);
        auto* previous = static_cast<float*>(Previous.Mapped);
        auto* ids = static_cast<uint32_t*>(Ids.Mapped);
        for (size_t i = 0; i < pixels; ++i) {
            for (int c = 0; c < 4; ++c) raw[4 * i + c] = static_cast<float>(f.Raw[4 * i + c]);
            for (int c = 0; c < 3; ++c) geo[4 * i + c] = static_cast<float>(f.Normal[3 * i + c]);
            geo[4 * i + 3] = static_cast<float>(f.Depth[i]);
            motion[2 * i] = static_cast<float>(f.MotionVectors[2 * i]);
            motion[2 * i + 1] = static_cast<float>(f.MotionVectors[2 * i + 1]);
            for (int c = 0; c < 3; ++c)
                previous[4 * i + c] = f.PreviousPosition.empty() ? 0.0F : static_cast<float>(f.PreviousPosition[3 * i + c]);
            previous[4 * i + 3] = 0.0F;
            ids[i] = f.SurfaceId.empty() ? 0U : f.SurfaceId[i];
        }
    }

    Result Run(const Frame& f) {
        const size_t pixels = static_cast<size_t>(f.Width) * f.Height;
        for (auto* b : {&Colour, &Diag, &Diag2, &Ping[0], &Ping[1]})
            std::fill_n(static_cast<float*>(b->Mapped), (pixels + kGuard) * 4, kSentinel);
        for (auto* b : {&Variance, &PingVariance[0], &PingVariance[1]})
            std::fill_n(static_cast<float*>(b->Mapped), (pixels + kGuard) * 2, kSentinel);
        std::memcpy(Uniform.Mapped, f.Params, sizeof(TemporalShaderParams));
        Check(vkResetDescriptorPool(mDevice.Handle, mPool, 0));
        const int next = 1 - mCurrent;
        const VkDescriptorSet temporal = Set(mTemporal, {&Uniform, &Raw, &Geometry, &Motion, &Previous, &Ids,
                                                         &History[mCurrent], &History[next], &Colour, &Variance, &Diag, &Diag2});
        Begin();
        // Uploads and the previous frame's history writes -> this frame's reads.
        Barrier(VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        Dispatch(mTemporal, 0, temporal, f.Width, f.Height);
        const Device::Buffer* inColour = &Colour;
        const Device::Buffer* inVariance = &Variance;
        for (size_t level = 0; level < f.Levels.size(); ++level) {
            std::memcpy(LevelUniform[level].Mapped, &f.Levels[level], sizeof(FilterShaderParams));
            Device::Buffer* outColour = &Ping[level % 2];
            Device::Buffer* outVariance = &PingVariance[level % 2];
            const VkDescriptorSet set = Set(mFilter, {&LevelUniform[level], &Geometry, inColour, inVariance, outColour, outVariance});
            // Producer (temporal or previous level) -> consumer (this level).
            if (!(f.MissingBarrier && level == 0))
                Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            Dispatch(mFilter, 0, set, f.Width, f.Height);
            inColour = outColour;
            inVariance = outVariance;
        }
        Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_HOST_READ_BIT);
        Submit();
        mCurrent = next;
        Result r;
        const auto copy = [&](const Device::Buffer& b, size_t n, std::vector<float>& out) {
            const auto* v = static_cast<const float*>(b.Mapped);
            out.assign(v, v + n);
            for (size_t i = n; i < n + kGuard * (n / std::max<size_t>(pixels, 1)); ++i)
                r.GuardsIntact = r.GuardsIntact && v[i] == kSentinel;
        };
        copy(Colour, pixels * 4, r.Colour);
        copy(Variance, pixels * 2, r.Variance);
        copy(Diag, pixels * 4, r.Diag);
        copy(Diag2, pixels * 4, r.Diag2);
        copy(*inColour, pixels * 4, r.Filtered);
        copy(*inVariance, pixels * 2, r.FilteredVariance);
        return r;
    }

    // Full chain front end: M2 normal pass, gather (stratified rotation), pack.
    void GatherOnGpu(const GatherInput& input, const GatherSettings& settings) {
        const size_t texels = static_cast<size_t>(input.BufferWidth) * input.BufferHeight;
        const size_t pixels = static_cast<size_t>(input.Viewport.Width) * input.Viewport.Height;
        const GatherShaderParams params = MakeGatherShaderParams(input, settings);
        Device::Buffer uniform = mDevice.Make(sizeof(params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        Device::Buffer depth = mDevice.Make(texels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Device::Buffer cls = mDevice.Make(texels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Device::Buffer radiance = mDevice.Make(texels * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Device::Buffer normals = mDevice.Make(texels * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Device::Buffer aux = mDevice.Make(pixels * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(uniform.Mapped, &params, sizeof(params));
        std::memcpy(depth.Mapped, input.Depth.data(), texels * 4);
        for (size_t i = 0; i < texels; ++i) {
            static_cast<uint32_t*>(cls.Mapped)[i] = input.Class[i];
            for (int c = 0; c < 3; ++c) {
                static_cast<float*>(radiance.Mapped)[4 * i + c] = input.Radiance[3 * i + c];
                static_cast<float*>(normals.Mapped)[4 * i + c] = input.FaceNormals[3 * i + c];
            }
        }
        Check(vkResetDescriptorPool(mDevice.Handle, mPool, 0));
        const VkDescriptorSet set = Set(mGather, {&uniform, &depth, &cls, &radiance, &normals, &Raw, &aux, &Geometry});
        Begin();
        Barrier(VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        for (uint32_t k = 0; k < 3; ++k) {
            if (k > 0)
                Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            Dispatch(mGather, k, set, input.Viewport.Width, input.Viewport.Height);
        }
        Submit();
        for (auto* b : {&uniform, &depth, &cls, &radiance, &normals, &aux}) mDevice.Free(*b);
    }

    Device::Buffer Raw, Geometry, Motion, Previous, Ids, History[2], Colour, Variance, Diag, Diag2, Ping[2],
        PingVariance[2], Uniform, LevelUniform[kMaxFilterLevels];

  private:
    void Release() {
        for (auto* b : {&Raw, &Geometry, &Motion, &Previous, &Ids, &History[0], &History[1], &Colour, &Variance, &Diag,
                         &Diag2, &Ping[0], &Ping[1], &PingVariance[0], &PingVariance[1], &Uniform})
            mDevice.Free(*b);
        for (auto& b : LevelUniform) mDevice.Free(b);
    }

    Pipeline Make(uint32_t bindings, const std::vector<std::string>& sources) {
        Pipeline p;
        p.Bindings = bindings;
        std::vector<VkDescriptorSetLayoutBinding> b(bindings);
        for (uint32_t i = 0; i < bindings; ++i) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = bindings;
        layout.pBindings = b.data();
        Check(vkCreateDescriptorSetLayout(mDevice.Handle, &layout, nullptr, &p.SetLayout));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &p.SetLayout;
        Check(vkCreatePipelineLayout(mDevice.Handle, &pl, nullptr, &p.Layout));
        for (const auto& source : sources) {
            const VkShaderModule module = mDevice.Shader(source);
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.layout = p.Layout;
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                          "main", nullptr};
            VkPipeline handle{};
            Check(vkCreateComputePipelines(mDevice.Handle, VK_NULL_HANDLE, 1, &info, nullptr, &handle));
            vkDestroyShaderModule(mDevice.Handle, module, nullptr);
            p.Handles.push_back(handle);
        }
        return p;
    }

    VkDescriptorSet Set(const Pipeline& p, const std::vector<const Device::Buffer*>& buffers) {
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = mPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &p.SetLayout;
        VkDescriptorSet set{};
        Check(vkAllocateDescriptorSets(mDevice.Handle, &allocation, &set));
        std::vector<VkDescriptorBufferInfo> infos(buffers.size());
        std::vector<VkWriteDescriptorSet> writes(buffers.size());
        for (uint32_t i = 0; i < buffers.size(); ++i) {
            infos[i] = {buffers[i]->Handle, 0, VK_WHOLE_SIZE};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(mDevice.Handle, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return set;
    }

    void Begin() {
        Check(vkResetCommandPool(mDevice.Handle, mCommands, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(mCommand, &begin));
    }
    void Barrier(VkPipelineStageFlags srcStage, VkAccessFlags src, VkPipelineStageFlags dstStage, VkAccessFlags dst) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = src;
        barrier.dstAccessMask = dst;
        vkCmdPipelineBarrier(mCommand, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    void Dispatch(const Pipeline& p, uint32_t index, VkDescriptorSet set, uint32_t w, uint32_t h) {
        vkCmdBindDescriptorSets(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, p.Layout, 0, 1, &set, 0, nullptr);
        vkCmdBindPipeline(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, p.Handles[index]);
        vkCmdDispatch(mCommand, (w + 7) / 8, (h + 7) / 8, 1);
    }
    void Submit() {
        Check(vkEndCommandBuffer(mCommand));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &mCommand;
        Check(vkQueueSubmit(mDevice.Queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(mDevice.Queue));
    }

    Device& mDevice;
    Pipeline mGather, mTemporal, mFilter;
    VkDescriptorPool mPool{};
    VkCommandPool mCommands{};
    VkCommandBuffer mCommand{};
    int mCurrent = 0;
};

struct Diff {
    double MeanJ = 0.0, MaxJ = 0.0, MeanA = 0.0, MaxA = 0.0, MeanOut = 0.0, MaxOut = 0.0;
    size_t FlagMismatch = 0, NonFinite = 0, Pixels = 0, Over = 0;
    void Add(const Diff& o) {
        MeanJ += o.MeanJ, MeanA += o.MeanA, MeanOut += o.MeanOut;
        MaxJ = std::max(MaxJ, o.MaxJ), MaxA = std::max(MaxA, o.MaxA), MaxOut = std::max(MaxOut, o.MaxOut);
        FlagMismatch += o.FlagMismatch, NonFinite += o.NonFinite, Pixels += o.Pixels, Over += o.Over;
    }
};

double Rel(double a, double b) { return std::abs(a - b) / std::max(1.0e-3, std::abs(b)); }

Diff Compare(const Runner::Result& g, const TemporalResult& t, const std::vector<FilteredPixel>& f) {
    Diff d;
    for (size_t i = 0; i < t.Pixels.size(); ++i) {
        const TemporalPixel& p = t.Pixels[i];
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(g.Colour[4 * i + c]) || !std::isfinite(g.Filtered[4 * i + c])) ++d.NonFinite;
        const uint32_t flags = static_cast<uint32_t>(g.Diag2[4 * i + 3]);
        const bool accepted = (flags & kTemporalDiagAccepted) != 0;
        d.FlagMismatch += (accepted != p.HistoryAccepted) || (((flags & kTemporalDiagReceiver) != 0) != p.Receiver);
        const double dj = Rel(g.Colour[4 * i + 1], p.J.G);
        const double da = std::abs(g.Colour[4 * i + 3] - p.A);
        const double dout = Rel(g.Filtered[4 * i + 1], f[i].J.G) + std::abs(g.Filtered[4 * i + 3] - f[i].ARaw);
        d.MeanJ += dj, d.MeanA += da, d.MeanOut += dout;
        d.MaxJ = std::max(d.MaxJ, dj), d.MaxA = std::max(d.MaxA, da), d.MaxOut = std::max(d.MaxOut, dout);
        d.Over += dout > 1.0e-3;
        ++d.Pixels;
    }
    return d;
}

std::vector<FilterShaderParams> Levels(const FilterInput& in, const FilterSettings& s) {
    std::vector<FilterShaderParams> out;
    for (uint32_t l = 0; l < s.Levels; ++l) out.push_back(MakeFilterShaderParams(in, s, l));
    return out;
}

} // namespace

int main() try {
    Device device;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.Physical, &properties);
    std::printf("DGI_GPU_M3 device=\"%s\" driver=0x%x api=%u.%u.%u validation_layer=%d sync_validation=%d\n",
                properties.deviceName, properties.driverVersion, VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion), device.Validation,
                device.Validation);
    Runner runner(device);
    TemporalSettings ts;
    ts.Threads = 4;
    FilterSettings fs;
    fs.Threads = 4;

    // --- Sequence twin: moving box, moving camera, resets (exposure, cut,
    // explicit), both validation modes, odd extent 97 x 61.
    {
        const Scene base = CornerRoomScene(97, 61);
        SequenceScene scene{base.Name, base.Camera, base.Primitives, std::vector<uint32_t>(base.Primitives.size(), 0U)};
        scene.Primitives.push_back(Primitive::Box({0.0, 0.0, 0.0}, {0.35, 0.4, 0.35}, {0.05, 0.05, 0.05}));
        scene.Object.push_back(1U);
        GatherSettings g;
        g.Radius = 2.5, g.Thickness = 0.5, g.Sections = 1, g.Sectors = 64, g.Threads = 4;
        g.Rotations = {RotationMode::Stratified, 2, 7, 0};
        for (const HistoryValidation mode : {HistoryValidation::FlowAndDepth, HistoryValidation::PreviousPosition}) {
            TemporalAccumulator cpu(Precision::Float32);
            runner.Allocate(97, 61, true); // NaN history: a reset frame must never read it
            Diff total;
            bool guards = true;
            std::vector<SequenceFrame> frames(36);
            for (size_t k = 0; k < frames.size(); ++k) {
                frames[k].Objects.resize(2);
                frames[k].Objects[1].Position = {-2.3 + 0.08 * k, -0.6, -5.2};
                frames[k].Camera.Position = {0.012 * k, 0.0, -0.01 * k};
                frames[k].Camera.Yaw = 0.003 * k;
                frames[k].JitterX = (k % 2 == 0) ? 0.25 : -0.25;
                frames[k].JitterY = (k % 2 == 0) ? -0.25 : 0.25;
            }
            for (size_t k = 0; k < frames.size(); ++k) {
                const RenderedFrame rendered = RenderSequenceFrame(scene, frames[k], frames[k == 0 ? 0 : k - 1]);
                const PreparedGeometry geometry = PrepareGeometry(rendered.Input);
                g.Rotations.Frame = k;
                const auto gather = GatherImage(rendered.Input, g);
                TemporalFrame f = MakeSequenceTemporalFrame(rendered, geometry, gather);
                f.FrameIndex = k;
                f.Validation = mode;
                if (k >= 12) {
                    f.PreExposure = 2.0;
                    for (size_t i = 0; i < f.Raw.size(); i += 4) f.Raw[i] *= 2.0, f.Raw[i + 1] *= 2.0, f.Raw[i + 2] *= 2.0;
                }
                f.CameraCut = k == 20;
                f.ExplicitReset = k == 28;
                if (k == 5) f.Raw[4 * 3000] = std::numeric_limits<double>::infinity();
                const TemporalResult t = cpu.Process(f, ts);
                const FilterInput in = MakeFilterInput(f, t);
                const auto filtered = FilterImage(in, fs, Precision::Float32);
                runner.Upload(f);
                Runner::Frame gf{f.Width, f.Height, &cpu.LastShaderParams(), Levels(in, fs)};
                const auto r = runner.Run(gf);
                guards = guards && r.GuardsIntact;
                total.Add(Compare(r, t, filtered));
            }
            std::printf("DGI_GPU_M3 sequence validation=%s frames=36 extent=97x61 guards=%d nonfinite=%zu flag_mismatch=%zu | "
                        "vs FP32 twin: history J mean rel=%.2e max=%.2e, A mean=%.2e max=%.2e | filtered mean=%.2e max=%.2e "
                        "pixels over 1e-3=%zu of %zu\n",
                        mode == HistoryValidation::FlowAndDepth ? "flow_and_depth" : "previous_position", guards,
                        total.NonFinite, total.FlagMismatch, total.MeanJ / total.Pixels, total.MaxJ,
                        total.MeanA / total.Pixels, total.MaxA, total.MeanOut / total.Pixels, total.MaxOut, total.Over,
                        total.Pixels);
            Require(guards, "GPU wrote past an output");
            Require(total.NonFinite == 0, "GPU produced non-finite output");
            Require(total.FlagMismatch <= total.Pixels / 1000, "GPU history decisions differ from the FP32 twin");
            Require(total.MeanJ / total.Pixels < 1.0e-4 && total.MeanOut / total.Pixels < 1.0e-3,
                    "GPU deviates from the FP32 twin beyond rounding");
            Require(total.Over <= total.Pixels / 10, "more than 10% of GPU pixels deviate by 1e-3");
        }
    }

    // --- T16: odd extents, partial workgroups, extreme counts; determinism.
    {
        size_t cases = 0, nonFinite = 0, mismatch = 0, notDeterministic = 0;
        double worst = 0.0;
        for (const auto& extent : std::vector<std::pair<uint32_t, uint32_t>>{{1, 1}, {7, 1}, {1, 7}, {13, 11}, {131, 77}})
            for (const uint32_t log2 : {0U, 3U})
                for (const uint32_t levels : {0U, 1U, 8U}) {
                    const Scene scene = PillarsScene(extent.first, extent.second);
                    const GatherInput input = Rasterize(scene);
                    const PreparedGeometry geometry = PrepareGeometry(input);
                    GatherSettings g;
                    g.Radius = 3.0, g.Thickness = 0.5, g.Sections = 1, g.Threads = 4;
                    g.Rotations = {RotationMode::Stratified, log2, log2 == 3 ? 64U : 1U, 0};
                    TemporalSettings t = ts;
                    t.TileLog2 = log2;
                    FilterSettings f = fs;
                    f.Levels = levels;
                    std::vector<float> first;
                    for (int pass = 0; pass < 2; ++pass) {
                        TemporalAccumulator cpu(Precision::Float32);
                        runner.Allocate(extent.first, extent.second, true);
                        for (uint64_t k = 0; k < 3; ++k) {
                            g.Rotations.Frame = k + (1ULL << 40);
                            TemporalFrame frame = MakeTemporalFrame(input, geometry, GatherImage(input, g));
                            frame.FrameIndex = k;
                            const size_t count = frame.Depth.size();
                            if (count > 4) {
                                frame.Raw[4 * (count / 2)] = std::numeric_limits<double>::quiet_NaN();
                                frame.MotionVectors[2 * (count / 3)] = 1.0e30;
                                frame.Normal[3 * (count - 1)] = std::numeric_limits<double>::quiet_NaN();
                            }
                            const TemporalResult tr = cpu.Process(frame, t);
                            const FilterInput in = MakeFilterInput(frame, tr);
                            const auto out = FilterImage(in, f, Precision::Float32);
                            runner.Upload(frame);
                            Runner::Frame gf{frame.Width, frame.Height, &cpu.LastShaderParams(), Levels(in, f)};
                            const auto r = runner.Run(gf);
                            const Diff d = Compare(r, tr, out);
                            nonFinite += d.NonFinite + (r.GuardsIntact ? 0 : 1);
                            mismatch += d.FlagMismatch;
                            worst = std::max(worst, d.MaxOut);
                            if (k == 2) {
                                if (pass == 0) first = r.Filtered;
                                else notDeterministic += first != r.Filtered;
                            }
                        }
                    }
                    ++cases;
                }
        std::printf("DGI_GPU_M3 t16 cases=%zu (extents 1x1, 7x1, 1x7, 13x11, 131x77; tiles 1 and 8; cycles 1 and 64; "
                    "levels 0, 1, 8; frame index 2^40; poisoned history and inputs) nonfinite_or_guard=%zu "
                    "flag_mismatch=%zu max filtered deviation=%.2e non_deterministic=%zu\n",
                    cases, nonFinite, mismatch, worst, notDeterministic);
        Require(nonFinite == 0, "T16: non-finite output or guard overwritten");
        Require(notDeterministic == 0, "T16: repeated runs differ");
    }

    // --- Full chain on the GPU (normal, gather, pack, temporal, filter)
    // against the FP32 CPU chain over a static scene.
    {
        const Scene scene = CornerRoomScene(96, 64);
        const GatherInput input = Rasterize(scene);
        GatherSettings g;
        g.Radius = 2.5, g.Thickness = 0.5, g.Sections = 1, g.Threads = 4;
        g.Rotations = {RotationMode::Stratified, 2, 7, 0};
        TemporalAccumulator cpu(Precision::Float32);
        runner.Allocate(96, 64, true);
        double meanOut = 0.0, meanRef = 0.0;
        size_t count = 0;
        for (uint64_t k = 0; k < 16; ++k) {
            g.Rotations.Frame = k;
            const PreparedGeometry geometry = PrepareGeometryF32(input);
            TemporalFrame frame = MakeTemporalFrame(input, geometry, GatherImageF32(input, g));
            frame.FrameIndex = k;
            const TemporalResult tr = cpu.Process(frame, ts);
            const FilterInput in = MakeFilterInput(frame, tr);
            const auto out = FilterImage(in, fs, Precision::Float32);
            runner.Upload(frame); // motion and previous positions; raw and geometry are overwritten on the GPU
            runner.GatherOnGpu(input, g);
            Runner::Frame gf{96, 64, &cpu.LastShaderParams(), Levels(in, fs)};
            const auto r = runner.Run(gf);
            if (k != 15) continue;
            for (size_t i = 0; i < out.size(); ++i) {
                if (!out[i].Receiver) continue;
                meanOut += std::abs(r.Filtered[4 * i + 1] - out[i].J.G);
                meanRef += std::abs(out[i].J.G);
                ++count;
            }
        }
        std::printf("DGI_GPU_M3 full_chain 96x64 frames=16 mean |dJ| / mean J (GPU chain vs FP32 CPU chain)=%.2e\n",
                    meanOut / meanRef);
        Require(meanOut / meanRef < 5.0e-3, "GPU full chain deviates from the CPU chain"); // M2 gather flips propagate
    }

    const uint32_t errorsBefore = gErrors;
    // --- Negative control: without the barrier between the temporal pass and
    // the first filter level the synchronization validation must report a hazard.
    if (device.Validation) {
        const Scene scene = PillarsScene(32, 24);
        const GatherInput input = Rasterize(scene);
        const PreparedGeometry geometry = PrepareGeometry(input);
        GatherSettings g;
        g.Radius = 3.0, g.Sections = 1;
        TemporalAccumulator cpu(Precision::Float32);
        runner.Allocate(32, 24, false);
        TemporalFrame frame = MakeTemporalFrame(input, geometry, GatherImage(input, g));
        const TemporalResult tr = cpu.Process(frame, ts);
        const FilterInput in = MakeFilterInput(frame, tr);
        runner.Upload(frame);
        Runner::Frame gf{32, 24, &cpu.LastShaderParams(), Levels(in, fs), true};
        runner.Run(gf);
    }
    const uint32_t hazardsInControl = gSyncHazards;
    std::printf("DGI_GPU_M3 validation errors in the test runs=%u (loader messages %u); negative control (missing "
                "barrier) sync hazards=%u%s%s\n",
                errorsBefore, gLoaderMessages, hazardsInControl, gFirstMessage.empty() ? "" : " first: ",
                gFirstMessage.substr(0, 160).c_str());
    Require(errorsBefore == 0, "validation errors in the test runs");
    if (device.Validation && hazardsInControl == 0)
        std::printf("DGI_GPU_M3 WARNING synchronization validation did not report the missing barrier: race freedom "
                    "rests on construction and determinism only\n");
    std::printf("DGI_GPU_M3 passed\n");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
