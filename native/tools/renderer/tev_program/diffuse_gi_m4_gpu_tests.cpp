// GPU twin of the M4 paths of the diffuse GI (docs/TRIAEVUM_SCREEN_SPACE_GI.md,
// "M4"): hierarchy level 0 and reduction with the radiance cache and SH sums,
// the gather with the metric radial distribution, hierarchy footprints,
// cache, guiding and blue-noise rotations, and the guide update. The GLSL
// kernels are compiled at run time and compared with the FP32 C++ twins
// (same operation order) on synthetic scenes with odd extents (partial 8 x 8
// workgroups), guard sentinels after every output, a padded and poisoned
// viewport, and the Khronos validation layer when installed. Lines starting
// with DGI_GPU_M4 are quoted in the document. Not wired into the renderer.
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_gather_shader.h"
#include "fast/renderer/diffuse_gi_guiding.h"
#include "fast/renderer/diffuse_gi_hierarchy.h"
#include "fast/renderer/diffuse_gi_m4_shader.h"
#include "fast/renderer/diffuse_gi_noise.h"
#include "fast/renderer/diffuse_gi_scene.h"

#include <vulkan/vulkan.h>
#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
constexpr uint32_t kBindings = 16;

uint32_t gErrors = 0, gLoaderMessages = 0;
std::string gFirstMessage;

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        ++gLoaderMessages; // environmental (third-party implicit layers)
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++gErrors;
        if (gFirstMessage.empty()) gFirstMessage = data->pMessage ? data->pMessage : "";
    }
    return VK_FALSE;
}

// Test device with the validation layer when installed (same construction
// as the M3 GPU test).
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
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        messenger.pfnUserCallback = OnMessage;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (Validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layer;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = &extension;
            info.pNext = &messenger;
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
        const auto spirv = compiler.CompileGlslToSpv(source, shaderc_compute_shader, "dgi_m4", options);
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

struct GpuFrame {
    std::vector<float> Results, Aux, Normals, Cells, Sh, Records;
    bool GuardsIntact = true;
};

class M4Gpu {
  public:
    enum Kernel { Normal, Level0, Reduce, Gather, Update, KernelCount };

    M4Gpu() {
        std::vector<VkDescriptorSetLayoutBinding> bindings(kBindings);
        for (uint32_t i = 0; i < kBindings; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = (i == 0 || i == 7) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setLayout.bindingCount = kBindings;
        setLayout.pBindings = bindings.data();
        Check(vkCreateDescriptorSetLayout(D.Handle, &setLayout, nullptr, &mSetLayout));
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &mSetLayout;
        Check(vkCreatePipelineLayout(D.Handle, &layout, nullptr, &mLayout));
        const std::string sources[KernelCount] = {BuildDiffuseGiNormalShader(), BuildDiffuseGiHierarchyLevel0Shader(),
                                                  BuildDiffuseGiHierarchyReduceShader(), BuildDiffuseGiGatherM4Shader(),
                                                  BuildDiffuseGiGuideUpdateShader()};
        for (int k = 0; k < KernelCount; ++k) {
            const VkShaderModule shader = D.Shader(sources[k]);
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.layout = mLayout;
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader,
                          "main", nullptr};
            Check(vkCreateComputePipelines(D.Handle, VK_NULL_HANDLE, 1, &info, nullptr, &mPipelines[k]));
            vkDestroyShaderModule(D.Handle, shader, nullptr);
        }
        const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(D.Handle, &pool, nullptr, &mPool));
        VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commands.queueFamilyIndex = D.Family;
        Check(vkCreateCommandPool(D.Handle, &commands, nullptr, &mCommands));
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = mCommands;
        command.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(D.Handle, &command, &mCommand));
    }
    ~M4Gpu() {
        vkDestroyCommandPool(D.Handle, mCommands, nullptr);
        vkDestroyDescriptorPool(D.Handle, mPool, nullptr);
        for (auto pipeline : mPipelines) vkDestroyPipeline(D.Handle, pipeline, nullptr);
        vkDestroyPipelineLayout(D.Handle, mLayout, nullptr);
        vkDestroyDescriptorSetLayout(D.Handle, mSetLayout, nullptr);
    }

    // Normal pass, hierarchy (level 0, reductions), gather; one submission per
    // stage (the level being reduced is a uniform value).
    GpuFrame Run(const GatherInput& input, const GatherSettings& settings, const HierarchySettings& hierarchy,
                 const GuideTable* guide, const GuidingSettings* guiding, const NoiseMask* noise, bool records) {
        const size_t texels = static_cast<size_t>(input.BufferWidth) * input.BufferHeight;
        const size_t pixels = static_cast<size_t>(input.Viewport.Width) * input.Viewport.Height;
        const uint32_t levels = std::clamp(hierarchy.Levels, 1U, kMaxHierarchyLevels);
        const size_t cells = HierarchyCellCount(input.Viewport.Width, input.Viewport.Height, levels);
        const uint32_t shCount = hierarchy.ShOrder == 0U ? 1U : ShCount(std::min(hierarchy.ShOrder, kMaxShOrder));
        const GatherShaderParams gather = MakeGatherShaderParams(input, settings);
        M4ShaderParams m4 = MakeM4ShaderParams(input, settings, hierarchy, guide, guiding, noise, records);
        const std::vector<float> table = guide != nullptr ? PackGuideTable(*guide) : std::vector<float>(4, 0.0F);
        std::vector<Device::Buffer> b(kBindings);
        const auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        b[0] = D.Make(sizeof(gather), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        b[1] = D.Make(texels * 4, storage);
        b[2] = D.Make(texels * 4, storage);
        b[3] = D.Make(texels * 16, storage);
        b[4] = D.Make(texels * 16, storage);
        b[5] = D.Make((pixels + kGuard) * 16, storage);
        b[6] = D.Make((pixels + kGuard) * 16, storage);
        b[7] = D.Make(sizeof(m4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        b[8] = D.Make((3 * cells + kGuard) * 16, storage);
        b[9] = D.Make((shCount * cells + kGuard) * 16, storage);
        b[10] = D.Make(table.size() * 4, storage);
        b[11] = D.Make((pixels * settings.Sections + kGuard) * 16, storage);
        b[12] = D.Make((noise != nullptr ? noise->Count() : 4U) * 4, storage);
        for (uint32_t k = 13; k < kBindings; ++k) b[k] = D.Make(16, storage);
        std::memcpy(b[0].Mapped, &gather, sizeof(gather));
        std::memcpy(b[1].Mapped, input.Depth.data(), texels * 4);
        auto* classes = static_cast<uint32_t*>(b[2].Mapped);
        auto* radiance = static_cast<float*>(b[3].Mapped);
        auto* normals = static_cast<float*>(b[4].Mapped);
        for (size_t i = 0; i < texels; ++i) {
            classes[i] = input.Class[i];
            for (int c = 0; c < 3; ++c) {
                radiance[4 * i + c] = input.Radiance[3 * i + c];
                normals[4 * i + c] = input.FaceNormals.empty() ? 0.0F : input.FaceNormals[3 * i + c];
            }
            radiance[4 * i + 3] = 0.0F;
            normals[4 * i + 3] = 0.0F;
        }
        for (const int k : {5, 6, 8, 9, 11}) {
            auto* v = static_cast<float*>(b[k].Mapped);
            std::fill(v, v + b[k].Size / 4, kSentinel);
        }
        std::memcpy(b[10].Mapped, table.data(), table.size() * 4);
        if (noise != nullptr) std::memcpy(b[12].Mapped, noise->Rank.data(), noise->Count() * 4);
        Bind(b);
        const uint32_t gx = (input.Viewport.Width + 7) / 8, gy = (input.Viewport.Height + 7) / 8;
        Dispatch(Normal, gx, gy, b, m4);
        Dispatch(Level0, gx, gy, b, m4);
        for (uint32_t l = 1; l < levels; ++l) {
            m4.Build[0] = l;
            const uint32_t w = (input.Viewport.Width + (1U << l) - 1U) >> l, h = (input.Viewport.Height + (1U << l) - 1U) >> l;
            Dispatch(Reduce, (w + 7) / 8, (h + 7) / 8, b, m4);
        }
        m4.Build[0] = 0;
        Dispatch(Gather, gx, gy, b, m4);
        GpuFrame out;
        const auto copy = [](const Device::Buffer& buffer, size_t floats) {
            const auto* v = static_cast<const float*>(buffer.Mapped);
            return std::vector<float>(v, v + floats);
        };
        out.Results = copy(b[5], pixels * 4);
        out.Aux = copy(b[6], pixels * 4);
        out.Normals = copy(b[4], texels * 4);
        out.Cells = copy(b[8], cells * 12);
        out.Sh = copy(b[9], cells * shCount * 4);
        out.Records = copy(b[11], pixels * settings.Sections * 4);
        const auto guard = [&](const Device::Buffer& buffer, size_t used) {
            const auto* v = static_cast<const float*>(buffer.Mapped);
            for (size_t i = used; i < used + kGuard * 4; ++i) out.GuardsIntact = out.GuardsIntact && v[i] == kSentinel;
        };
        guard(b[5], pixels * 4);
        guard(b[6], pixels * 4);
        guard(b[8], cells * 12);
        guard(b[9], cells * shCount * 4);
        guard(b[11], pixels * settings.Sections * 4);
        for (auto& buffer : b) D.Free(buffer);
        return out;
    }

    // Guide update from records (binding 11) and a packed state.
    std::pair<std::vector<float>, std::vector<float>> UpdateGuide(const GatherInput& input, const GuideTable& table,
                                                                  const GuidingSettings& guiding,
                                                                  const std::vector<float>& records,
                                                                  const std::vector<float>& state, uint32_t sections) {
        GatherSettings settings;
        settings.Sections = sections;
        const GatherShaderParams gather = MakeGatherShaderParams(input, settings);
        HierarchySettings hs;
        M4ShaderParams m4 = MakeM4ShaderParams(input, settings, hs, &table, &guiding, nullptr, true);
        const uint32_t regions = table.RegionsX * table.RegionsY;
        std::vector<Device::Buffer> b(kBindings);
        const auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        for (uint32_t k = 0; k < kBindings; ++k) b[k] = D.Make(16, (k == 0 || k == 7) ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : storage);
        D.Free(b[0]);
        b[0] = D.Make(sizeof(gather), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        D.Free(b[7]);
        b[7] = D.Make(sizeof(m4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        D.Free(b[11]);
        b[11] = D.Make(records.size() * 4, storage);
        D.Free(b[13]);
        b[13] = D.Make(state.size() * 4, storage);
        D.Free(b[14]);
        b[14] = D.Make((state.size() + kGuard * 4) * 4, storage);
        D.Free(b[15]);
        const size_t tableFloats = static_cast<size_t>(regions) * (2U * table.Bins + 1U);
        b[15] = D.Make((tableFloats + kGuard * 4) * 4, storage);
        std::memcpy(b[0].Mapped, &gather, sizeof(gather));
        std::memcpy(b[11].Mapped, records.data(), records.size() * 4);
        std::memcpy(b[13].Mapped, state.data(), state.size() * 4);
        for (const int k : {14, 15}) {
            auto* v = static_cast<float*>(b[k].Mapped);
            std::fill(v, v + b[k].Size / 4, kSentinel);
        }
        Bind(b);
        Dispatch(Update, (table.RegionsX + 7) / 8, (table.RegionsY + 7) / 8, b, m4);
        const auto* s = static_cast<const float*>(b[14].Mapped);
        const auto* t = static_cast<const float*>(b[15].Mapped);
        std::pair<std::vector<float>, std::vector<float>> out{std::vector<float>(s, s + state.size()),
                                                              std::vector<float>(t, t + tableFloats)};
        mUpdateGuards = true;
        for (size_t i = state.size(); i < state.size() + kGuard * 4; ++i) mUpdateGuards = mUpdateGuards && s[i] == kSentinel;
        for (size_t i = tableFloats; i < tableFloats + kGuard * 4; ++i) mUpdateGuards = mUpdateGuards && t[i] == kSentinel;
        for (auto& buffer : b) D.Free(buffer);
        return out;
    }
    [[nodiscard]] bool UpdateGuards() const { return mUpdateGuards; }

    Device D{};

  private:
    void Bind(const std::vector<Device::Buffer>& b) {
        Check(vkResetDescriptorPool(D.Handle, mPool, 0));
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = mPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &mSetLayout;
        Check(vkAllocateDescriptorSets(D.Handle, &allocation, &mSet));
        std::array<VkDescriptorBufferInfo, kBindings> infos{};
        std::array<VkWriteDescriptorSet, kBindings> writes{};
        for (uint32_t k = 0; k < kBindings; ++k) {
            infos[k] = {b[k].Handle, 0, VK_WHOLE_SIZE};
            writes[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[k].dstSet = mSet;
            writes[k].dstBinding = k;
            writes[k].descriptorCount = 1;
            writes[k].descriptorType = (k == 0 || k == 7) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[k].pBufferInfo = &infos[k];
        }
        vkUpdateDescriptorSets(D.Handle, kBindings, writes.data(), 0, nullptr);
    }

    // One kernel in its own submission; the barrier makes the writes visible
    // to the next stage (producer -> consumer) and to the host.
    void Dispatch(Kernel kernel, uint32_t groupsX, uint32_t groupsY, const std::vector<Device::Buffer>& b,
                  const M4ShaderParams& m4) {
        std::memcpy(b[7].Mapped, &m4, sizeof(m4));
        Check(vkResetCommandPool(D.Handle, mCommands, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(mCommand, &begin));
        vkCmdBindDescriptorSets(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSet, 0, nullptr);
        vkCmdBindPipeline(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, mPipelines[kernel]);
        vkCmdDispatch(mCommand, std::max(groupsX, 1U), std::max(groupsY, 1U), 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(mCommand, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
        Check(vkEndCommandBuffer(mCommand));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &mCommand;
        Check(vkQueueSubmit(D.Queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(D.Queue));
    }

    VkDescriptorSetLayout mSetLayout{};
    VkPipelineLayout mLayout{};
    VkPipeline mPipelines[KernelCount]{};
    VkDescriptorPool mPool{};
    VkDescriptorSet mSet{};
    VkCommandPool mCommands{};
    VkCommandBuffer mCommand{};
    bool mUpdateGuards = true;
};

double Luma(const float* j) { return 0.2126 * j[0] + 0.7152 * j[1] + 0.0722 * j[2]; }

struct Case {
    std::string Name;
    Scene SceneValue;
    GatherSettings Settings;
    bool FaceNormals = false;
};

} // namespace

int main() try {
    M4Gpu gpu;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu.D.Physical, &properties);
    std::printf("DGI_GPU_M4 device=\"%s\" driver=0x%x api=%u.%u.%u validation_layer=%d\n", properties.deviceName,
                properties.driverVersion, VK_API_VERSION_MAJOR(properties.apiVersion),
                VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion), gpu.D.Validation);
    const auto start = std::chrono::steady_clock::now();
    NoiseMaskSettings ns;
    ns.Width = ns.Height = 16, ns.Depth = 8;
    const NoiseMask noise = GenerateBlueNoiseMask(ns);
    const HierarchySettings hs; // defaults (5 levels, 1e-3, 0.9995, SH order 2)

    // --- Cases: odd extents, a padded and poisoned viewport, each M4 path.
    std::vector<Case> cases;
    const auto radial = [](double radius, double thickness) {
        GatherSettings g;
        g.Radius = radius, g.Thickness = thickness, g.Sections = 4, g.Sectors = 64;
        g.Schedule = CandidateSchedule::Radial, g.CandidatesPerSide = 16, g.RadialExponent = 2.0, g.RadialJitter = 0.5;
        g.Threads = 8;
        return g;
    };
    {
        Case c{"corner_97x61_metric", CornerRoomScene(97, 61), radial(2.5, 0.5)};
        c.Settings.Schedule = CandidateSchedule::MetricRadial;
        cases.push_back(c);
    }
    {
        Case c{"corner_97x61_footprint", CornerRoomScene(97, 61), radial(2.5, 0.5)};
        c.Settings.HierarchyFootprint = true;
        cases.push_back(c);
    }
    {
        Case c{"checker_101x67_cache_mean", CheckerRoomScene(101, 67), radial(2.5, 0.5)};
        c.Settings.Cache = CacheMode::Mean;
        cases.push_back(c);
    }
    {
        Case c{"checker_101x67_footprint_sh", CheckerRoomScene(101, 67), radial(2.5, 0.5)};
        c.Settings.HierarchyFootprint = true, c.Settings.Cache = CacheMode::SphericalHarmonics;
        cases.push_back(c);
    }
    {
        Case c{"pillars_91x63_guiding_independent_A", PillarsScene(91, 63), radial(3.0, 0.5)};
        c.Settings.Guiding = true;
        cases.push_back(c);
    }
    {
        Case c{"pillars_91x63_guiding_same_pdf_A", PillarsScene(91, 63), radial(3.0, 0.5)};
        c.Settings.Guiding = true, c.Settings.GuidingAccessibility = GuidedAccessibility::SamePdf;
        cases.push_back(c);
    }
    {
        Case c{"t06_83x59_blue_noise_face_normals", BlackOccluderScene(83, 59, true), radial(5.0, 1.0)};
        c.Settings.Rotations.Mode = RotationMode::BlueNoise, c.Settings.Rotations.Frame = 5;
        c.FaceNormals = true;
        cases.push_back(c);
    }
    {
        Case c{"t06_83x59_all", BlackOccluderScene(83, 59, true), radial(5.0, 1.0)};
        c.Settings.Schedule = CandidateSchedule::MetricRadial, c.Settings.HierarchyFootprint = true;
        c.Settings.Cache = CacheMode::Mean, c.Settings.Guiding = true;
        c.Settings.Rotations.Mode = RotationMode::BlueNoise, c.Settings.Rotations.Frame = 3;
        cases.push_back(c);
    }
    {
        Case c{"t06_83x59_all_padded", BlackOccluderScene(83, 59, true), cases.back().Settings};
        c.SceneValue.PadLeft = 5, c.SceneValue.PadTop = 3, c.SceneValue.PadRight = 9, c.SceneValue.PadBottom = 7;
        cases.push_back(c);
    }

    std::vector<float> unpadded;
    size_t totalPixels = 0;
    for (auto& c : cases) {
        GatherInput input = Rasterize(c.SceneValue);
        if (c.FaceNormals) input.NormalSource = GeometricNormalSource::RendererFaceNormal;
        const uint32_t w = input.Viewport.Width, h = input.Viewport.Height;
        const PreparedGeometry g32 = PrepareGeometryF32(input);
        double maxRadiance = 1.0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t bi = static_cast<size_t>(y + input.Viewport.Y) * input.BufferWidth + x + input.Viewport.X;
                for (int k = 0; k < 3; ++k) maxRadiance = std::max(maxRadiance, static_cast<double>(input.Radiance[3 * bi + k]));
            }
        const DepthHierarchy h32 = BuildDepthHierarchy(input, g32, hs, true);
        // A guide table learned from one uniform CPU frame (double), frozen and
        // shared by both sides.
        GuidingSettings guiding;
        GuideField learner(w, h, guiding);
        {
            std::vector<GuideSample> r;
            GatherAcceleration a;
            a.Guide = &learner.Frozen();
            a.Samples = &r;
            GatherSettings first = c.Settings;
            first.Guiding = false, first.Sections = 8, first.Rotations = RotationSequence{};
            first.HierarchyFootprint = false, first.Cache = CacheMode::Off, first.Schedule = CandidateSchedule::Radial;
            (void)GatherImage(input, first, a);
            learner.Update(r, first.Sections);
        }
        const GuideTable table = learner.Frozen();
        GatherAcceleration acc;
        acc.Hierarchy = &h32, acc.Guide = &table, acc.Noise = &noise;
        std::vector<GuideSample> cpuRecords;
        acc.Samples = &cpuRecords;
        Require(*ValidateGatherAcceleration(input, c.Settings, acc) == '\0', ValidateGatherAcceleration(input, c.Settings, acc));
        const GpuFrame frame = gpu.Run(input, c.Settings, hs, &table, &guiding, &noise, true);
        const auto cpu = GatherImageF32(input, c.Settings, acc);
        // Hierarchy: flags (the GPU uses its own normal pass), planes, cache, SH.
        // A texel on two planes may take the other plane's normal in the GPU
        // normal pass (the M2 tie tolerance); the cells above such a texel are
        // counted apart from the exact comparisons.
        std::vector<uint8_t> tieCell(h32.Cells.size(), 0U);
        size_t tieTexels = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const size_t bi = static_cast<size_t>(y + input.Viewport.Y) * input.BufferWidth + x + input.Viewport.X;
                const Vec3& n = g32.Normal[static_cast<size_t>(y) * w + x];
                const double dot = n.X * frame.Normals[4 * bi] + n.Y * frame.Normals[4 * bi + 1] + n.Z * frame.Normals[4 * bi + 2];
                if (!(Dot(n, n) > 0.0) || dot >= 0.9999) continue;
                ++tieTexels;
                for (uint32_t l = 0; l < h32.Levels.size(); ++l) tieCell[h32.Index(l, x >> l, y >> l)] = 1U;
            }
        size_t flagFlips = 0, planarBoth = 0, countMismatch = 0, tieCells = 0;
        double planeDiff = 0.0, meanDiff = 0.0, normalDiff = 0.0, shDiff = 0.0, boundDiff = 0.0;
        const uint32_t shc = ShCount(hs.ShOrder);
        for (size_t i = 0; i < h32.Cells.size(); ++i) {
            const float* c0 = &frame.Cells[12 * i];
            uint32_t bits;
            std::memcpy(&bits, &c0[3], 4);
            const HierarchyCell& cell = h32.Cells[i];
            if ((bits >> 8U) != cell.Count) ++countMismatch;
            if (tieCell[i] != 0U) {
                ++tieCells;
                continue;
            }
            if ((bits & 0xFFU) != cell.Flags) ++flagFlips;
            if (cell.ZMin <= cell.ZMax)
                // In units of the depth resolution of perspective storage at z
                // (dz = dd (f - n) z^2 / (n f), about z^2 / n per unit of d).
                boundDiff = std::max({boundDiff, std::abs(c0[7] - cell.ZMin) * input.Camera.Near / (cell.ZMin * cell.ZMin),
                                      std::abs(c0[11] - cell.ZMax) * input.Camera.Near / (cell.ZMax * cell.ZMax)});
            // SH sums relative to the cell's total weight D_00 times the radiance
            // scale (coefficients of near-cancelling sums carry absolute error).
            const double shScale = std::max(1.0, std::abs(h32.Sh[4 * (i * shc) + 3])) * maxRadiance;
            for (uint32_t k = 0; k < shc && (bits & 0xFFU) == cell.Flags && (cell.Flags & kCellGeometry) != 0U; ++k)
                for (int e = 0; e < 4; ++e)
                    shDiff = std::max(shDiff, std::abs(frame.Sh[4 * (i * shc + k) + e] - h32.Sh[4 * (i * shc + k) + e]) /
                                                  shScale);
            if (((bits & cell.Flags) & kCellPlanar) == 0U) continue;
            ++planarBoth;
            planeDiff = std::max(planeDiff, std::abs(c0[0] - cell.W) / cell.W);
            normalDiff = std::max({normalDiff, std::abs(c0[4] - cell.Normal.X), std::abs(c0[5] - cell.Normal.Y),
                                   std::abs(c0[6] - cell.Normal.Z)});
            meanDiff = std::max({meanDiff, std::abs(c0[8] - cell.Mean.R), std::abs(c0[9] - cell.Mean.G),
                                 std::abs(c0[10] - cell.Mean.B)});
        }
        // Gather against the FP32 twin.
        double meanJ = 0.0, meanA = 0.0, maxJ = 0.0, maxA = 0.0;
        size_t over = 0, overStrict = 0, nonFinite = 0, validMismatch = 0, valid = 0;
        for (size_t i = 0; i < cpu.size(); ++i) {
            const float* r = &frame.Results[4 * i];
            const float* a = &frame.Aux[4 * i];
            for (int k = 0; k < 4; ++k) nonFinite += std::isfinite(r[k]) && std::isfinite(a[k]) ? 0U : 1U;
            if ((a[1] != 0.0F) != cpu[i].Valid) ++validMismatch;
            if (!cpu[i].Valid) continue;
            ++valid;
            const double scale = std::max({1.0, std::abs(cpu[i].J.R), std::abs(cpu[i].J.G), std::abs(cpu[i].J.B)});
            const double dj = std::max({std::abs(r[0] - cpu[i].J.R), std::abs(r[1] - cpu[i].J.G), std::abs(r[2] - cpu[i].J.B)}) / scale;
            const double da = std::abs(r[3] - cpu[i].ARaw);
            meanJ += dj, meanA += da, maxJ = std::max(maxJ, dj), maxA = std::max(maxA, da);
            if (dj > 1.0e-4 || da > 1.0e-4) ++overStrict;
            if (dj > 1.0e-3 || da > 1.0e-3) ++over;
        }
        meanJ /= std::max<size_t>(valid, 1), meanA /= std::max<size_t>(valid, 1);
        // Guide records (per section: decision flips as in the gather).
        size_t binMismatch = 0, records = 0, recordFlips = 0;
        double contributionDiff = 0.0;
        for (size_t i = 0; i < cpuRecords.size(); ++i) {
            const float* r = &frame.Records[4 * i];
            uint32_t bin;
            std::memcpy(&bin, &r[3], 4);
            if (bin != cpuRecords[i].Bin) {
                ++binMismatch;
                continue;
            }
            if (bin == kNoGuideBin) continue;
            ++records;
            const double d = std::abs(r[0] - cpuRecords[i].Contribution) / std::max(1.0, std::abs(cpuRecords[i].Contribution));
            contributionDiff += d;
            if (d > 1.0e-4 || r[1] != static_cast<float>(cpuRecords[i].Pdf)) ++recordFlips;
        }
        contributionDiff /= std::max<size_t>(records, 1);
        // Guide update: the GPU records through the FP32 C++ update and the GPU
        // update kernel, from the uniform state and then from a learned state.
        std::vector<GuideSample> gpuRecords(w * h * c.Settings.Sections);
        for (size_t i = 0; i < gpuRecords.size(); ++i) {
            const float* r = &frame.Records[4 * i];
            std::memcpy(&gpuRecords[i].Bin, &r[3], 4);
            gpuRecords[i].Contribution = r[0], gpuRecords[i].Pdf = r[1], gpuRecords[i].Phi = r[2];
        }
        double tableDiff = 0.0, stateDiff = 0.0;
        GuideField cpuField(w, h, guiding, true);
        for (int step = 0; step < 2; ++step) {
            const std::vector<float> state = PackGuideState(cpuField);
            const auto [gpuState, gpuTable] = gpu.UpdateGuide(input, cpuField.Frozen(), guiding, frame.Records, state,
                                                              c.Settings.Sections);
            Require(gpu.UpdateGuards(), "GPU guide update wrote past its outputs");
            cpuField.Update(gpuRecords, c.Settings.Sections);
            const std::vector<float> cpuState = PackGuideState(cpuField), cpuTable = PackGuideTable(cpuField.Frozen());
            for (size_t i = 0; i < cpuTable.size(); ++i) tableDiff = std::max(tableDiff, static_cast<double>(std::abs(gpuTable[i] - cpuTable[i])));
            for (size_t i = 0; i < cpuState.size(); ++i)
                stateDiff = std::max(stateDiff, static_cast<double>(std::abs(gpuState[i] - cpuState[i]) /
                                                                    std::max(1.0F, std::abs(cpuState[i]))));
        }
        std::printf("DGI_GPU_M4 case=%s extent=%ux%u guards=%d | hierarchy cells=%zu tie_texels=%zu tie_cells=%zu "
                    "flag_flips=%zu count_mismatch=%zu planar_both=%zu plane_rel=%.1e normal=%.1e cache_mean=%.1e "
                    "bounds_in_depth_steps=%.1e sh_rel=%.1e | gather vs cpu_f32 meanJ=%.2e meanA=%.2e maxJ=%.2e maxA=%.2e "
                    "over1e-4=%zu over1e-3=%zu/%zu nonfinite=%zu valid_mismatch=%zu | records=%zu bin_mismatch=%zu "
                    "contribution_mean_rel=%.1e record_flips=%zu | guide_update table=%.1e state_rel=%.1e\n",
                    c.Name.c_str(), w, h, frame.GuardsIntact, h32.Cells.size(), tieTexels, tieCells, flagFlips, countMismatch,
                    planarBoth, planeDiff, normalDiff, meanDiff, boundDiff, shDiff, meanJ, meanA, maxJ, maxA, overStrict, over, valid,
                    nonFinite, validMismatch, records, binMismatch, contributionDiff, recordFlips, tableDiff, stateDiff);
        Require(tieTexels <= static_cast<size_t>(w) * h / 1000, "more than 0.1% of GPU normals flip between junction planes");
        Require(recordFlips <= records / 50, "more than 2% of guide records flip");
        Require(frame.GuardsIntact, "GPU wrote past an output");
        Require(nonFinite == 0, "GPU produced non-finite output");
        Require(validMismatch == 0, "GPU validity differs from the FP32 twin");
        Require(countMismatch == 0, "GPU hierarchy counts differ");
        Require(flagFlips <= h32.Cells.size() / 500, "more than 0.2% of hierarchy flags flip");
        // The GPU normal pass agrees with the FP32 twin within dot >= 0.99999
        // (M2 GPU model, components within about 4.5e-3); the hierarchy built
        // on it inherits that: planes 1e-4 relative, normals 5e-3, SH sums
        // 1e-3 of the cell weight; cache means at rounding; depth bounds 1e-6
        // of the perspective depth step z^2 / n (about 16 float steps of the
        // stored depth: decoding amplifies a contracted multiply-add with z).
        Require(planeDiff < 1.0e-4 && normalDiff < 5.0e-3 && meanDiff < 1.0e-5 && boundDiff < 1.0e-6,
                "GPU hierarchy planes, normals, cache or bounds beyond the declared model");
        Require(shDiff < 1.0e-3, "GPU SH sums beyond the declared model");
        // Declared M4 FP32 model: means below 5e-4; isolated decision flips
        // (sector centres, chords, thresholds, levels, strip ends) beyond 1e-3
        // in at most 2% of pixels. The 1e-4 count also holds the continuous
        // differences that the cells inherit from the GPU normal pass (SH and
        // cache radiance of textured cells, plane gradients).
        Require(meanJ < 5.0e-4 && meanA < 5.0e-4, "GPU mean deviation beyond the FP32 model");
        Require(over <= valid / 50, "more than 2% of GPU pixels flip");
        Require(binMismatch <= cpuRecords.size() / 200, "more than 0.5% of guide bins differ");
        Require(tableDiff < 1.0e-5 && stateDiff < 1.0e-4, "GPU guide update differs from the FP32 twin");
        if (c.Name == "t06_83x59_all") unpadded = frame.Results;
        if (c.Name == "t06_83x59_all_padded") Require(unpadded == frame.Results, "shifted viewport changes the GPU result");
        totalPixels += w * h;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("DGI_GPU_M4 validation_errors=%u loader_messages=%u first=\"%s\" pixels=%zu seconds=%.2f\n", gErrors,
                gLoaderMessages, gFirstMessage.c_str(), totalPixels, seconds);
    Require(gErrors == 0, "validation errors");
    std::printf("DGI_GPU_M4 passed\n");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
