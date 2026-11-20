// GPU compute twin of the diffuse GI gathering (milestone M2,
// docs/TRIAEVUM_SCREEN_SPACE_GI.md): the GLSL kernels compiled at run time run
// on the synthetic scenes of the CPU reference and are compared with the FP32
// twin (same operation order) and the double reference. Odd extents exercise
// partial 8 x 8 workgroups; guard sentinels and a padded, poisoned viewport
// detect out-of-range reads and writes. Lines starting with DGI_GPU are quoted
// in the document. Not wired into the renderer.
#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_gather_shader.h"
#include "fast/renderer/diffuse_gi_scene.h"
#include "vulkan_test_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

namespace {

constexpr float kSentinel = 12345.0F;
constexpr uint32_t kGuard = 64; // vec4 elements after every output

struct ComputePipeline {
    VkDescriptorSetLayout SetLayout{};
    VkPipelineLayout Layout{};
    std::vector<VkPipeline> Pipelines;
};

struct GpuResult {
    std::vector<float> Results; // J.rgb, A raw per viewport pixel
    std::vector<float> Aux;     // C, valid, scheduled, occupied
    std::vector<float> Normals; // xyz, valid per buffer texel
    bool GuardsIntact = true;
    bool OutsideNormalsUntouched = true;
};

class DiffuseGiGpu {
  public:
    DiffuseGiGpu() {
        mGather = MakePipelines(7, {BuildDiffuseGiNormalShader(), BuildDiffuseGiGatherShader()});
        mUnit = MakePipelines(3, {BuildDiffuseGiUnitShader()});
        const std::array<VkDescriptorPoolSize, 2> sizes{
            {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 2;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(Device.Device, &pool, nullptr, &mPool));
        VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        commands.queueFamilyIndex = Device.Family;
        Check(vkCreateCommandPool(Device.Device, &commands, nullptr, &mCommands));
        VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command.commandPool = mCommands;
        command.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(Device.Device, &command, &mCommand));
    }

    ~DiffuseGiGpu() {
        vkDestroyCommandPool(Device.Device, mCommands, nullptr);
        vkDestroyDescriptorPool(Device.Device, mPool, nullptr);
        for (auto* pipeline : {&mGather, &mUnit}) {
            for (auto handle : pipeline->Pipelines) vkDestroyPipeline(Device.Device, handle, nullptr);
            vkDestroyPipelineLayout(Device.Device, pipeline->Layout, nullptr);
            vkDestroyDescriptorSetLayout(Device.Device, pipeline->SetLayout, nullptr);
        }
    }

    GpuResult Run(const GatherInput& input, const GatherSettings& settings) {
        const size_t texels = static_cast<size_t>(input.BufferWidth) * input.BufferHeight;
        const size_t pixels = static_cast<size_t>(input.Viewport.Width) * input.Viewport.Height;
        const GatherShaderParams params = MakeGatherShaderParams(input, settings);
        std::array<Gpu::Buffer, 7> buffers{
            Device.MakeBuffer(sizeof(params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
            Device.MakeBuffer(texels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
            Device.MakeBuffer(texels * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
            Device.MakeBuffer(texels * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
            Device.MakeBuffer(texels * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
            Device.MakeBuffer((pixels + kGuard) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
            Device.MakeBuffer((pixels + kGuard) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)};
        std::memcpy(buffers[0].Mapped, &params, sizeof(params));
        std::memcpy(buffers[1].Mapped, input.Depth.data(), texels * 4);
        auto* classes = static_cast<uint32_t*>(buffers[2].Mapped);
        auto* radiance = static_cast<float*>(buffers[3].Mapped);
        auto* normals = static_cast<float*>(buffers[4].Mapped);
        for (size_t i = 0; i < texels; ++i) {
            classes[i] = input.Class[i];
            for (int c = 0; c < 3; ++c) {
                radiance[4 * i + c] = input.Radiance[3 * i + c];
                normals[4 * i + c] = input.FaceNormals.empty() ? 0.0F : input.FaceNormals[3 * i + c];
            }
            radiance[4 * i + 3] = 0.0F;
            normals[4 * i + 3] = kSentinel;
        }
        std::vector<float> normalsBefore(normals, normals + 4 * texels);
        for (int b = 5; b < 7; ++b) {
            auto* values = static_cast<float*>(buffers[b].Mapped);
            std::fill(values, values + (pixels + kGuard) * 4, kSentinel);
        }
        Check(vkResetDescriptorPool(Device.Device, mPool, 0));
        VkDescriptorSet set = Allocate(mGather);
        std::array<VkDescriptorBufferInfo, 7> infos{};
        std::array<VkWriteDescriptorSet, 7> writes{};
        for (uint32_t b = 0; b < 7; ++b) {
            infos[b] = {buffers[b].Handle, 0, VK_WHOLE_SIZE};
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(Device.Device, 7, writes.data(), 0, nullptr);
        Begin();
        vkCmdBindDescriptorSets(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, mGather.Layout, 0, 1, &set, 0, nullptr);
        const uint32_t groupsX = (input.Viewport.Width + 7) / 8, groupsY = (input.Viewport.Height + 7) / 8;
        vkCmdBindPipeline(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, mGather.Pipelines[0]);
        vkCmdDispatch(mCommand, groupsX, groupsY, 1);
        // Producer (normal pass) -> consumer (gather): explicit dependency.
        Barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vkCmdBindPipeline(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, mGather.Pipelines[1]);
        vkCmdDispatch(mCommand, groupsX, groupsY, 1);
        Barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        Submit();
        GpuResult out;
        const auto* results = static_cast<const float*>(buffers[5].Mapped);
        const auto* aux = static_cast<const float*>(buffers[6].Mapped);
        out.Results.assign(results, results + pixels * 4);
        out.Aux.assign(aux, aux + pixels * 4);
        out.Normals.assign(normals, normals + texels * 4);
        for (size_t i = pixels * 4; i < (pixels + kGuard) * 4; ++i)
            out.GuardsIntact = out.GuardsIntact && results[i] == kSentinel && aux[i] == kSentinel;
        for (uint32_t y = 0; y < input.BufferHeight; ++y)
            for (uint32_t x = 0; x < input.BufferWidth; ++x) {
                const bool insideViewport = x >= input.Viewport.X && y >= input.Viewport.Y &&
                                            x < input.Viewport.X + input.Viewport.Width &&
                                            y < input.Viewport.Y + input.Viewport.Height;
                const size_t i = static_cast<size_t>(y) * input.BufferWidth + x;
                if (!insideViewport)
                    for (int c = 0; c < 4; ++c)
                        out.OutsideNormalsUntouched = out.OutsideNormalsUntouched &&
                                                      std::memcmp(&normals[4 * i + c], &normalsBefore[4 * i + c], 4) == 0;
            }
        for (auto& buffer : buffers) Device.Free(buffer);
        return out;
    }

    // mode 0: centre-rule masks of [x, y); mode 1: sector weights.
    std::vector<uint32_t> RunUnit(const std::vector<std::array<float, 4>>& inputs, uint32_t sectors, uint32_t mode) {
        const auto count = static_cast<uint32_t>(inputs.size());
        const std::array<uint32_t, 4> info{count, sectors, mode, 0U};
        Gpu::Buffer uniform = Device.MakeBuffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        Gpu::Buffer in = Device.MakeBuffer(count * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        Gpu::Buffer out = Device.MakeBuffer(count * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(uniform.Mapped, info.data(), 16);
        std::memcpy(in.Mapped, inputs.data(), count * 16);
        Check(vkResetDescriptorPool(Device.Device, mPool, 0));
        VkDescriptorSet set = Allocate(mUnit);
        std::array<VkDescriptorBufferInfo, 3> infos{
            {{uniform.Handle, 0, VK_WHOLE_SIZE}, {in.Handle, 0, VK_WHOLE_SIZE}, {out.Handle, 0, VK_WHOLE_SIZE}}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t b = 0; b < 3; ++b) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &infos[b];
        }
        vkUpdateDescriptorSets(Device.Device, 3, writes.data(), 0, nullptr);
        Begin();
        vkCmdBindDescriptorSets(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, mUnit.Layout, 0, 1, &set, 0, nullptr);
        vkCmdBindPipeline(mCommand, VK_PIPELINE_BIND_POINT_COMPUTE, mUnit.Pipelines[0]);
        vkCmdDispatch(mCommand, (count + 63) / 64, 1, 1);
        Barrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT);
        Submit();
        const auto* words = static_cast<const uint32_t*>(out.Mapped);
        std::vector<uint32_t> result(words, words + count * 8);
        Device.Free(uniform);
        Device.Free(in);
        Device.Free(out);
        return result;
    }

    Gpu Device{};

  private:
    ComputePipeline MakePipelines(uint32_t bindingCount, const std::vector<std::string>& sources) {
        ComputePipeline pipeline;
        std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
        for (uint32_t i = 0; i < bindingCount; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setLayout.bindingCount = bindingCount;
        setLayout.pBindings = bindings.data();
        Check(vkCreateDescriptorSetLayout(Device.Device, &setLayout, nullptr, &pipeline.SetLayout));
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &pipeline.SetLayout;
        Check(vkCreatePipelineLayout(Device.Device, &layout, nullptr, &pipeline.Layout));
        for (const auto& source : sources) {
            const auto shader = Device.Shader(source, shaderc_compute_shader);
            VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            info.layout = pipeline.Layout;
            info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                          shader, "main", nullptr};
            VkPipeline handle{};
            Check(vkCreateComputePipelines(Device.Device, VK_NULL_HANDLE, 1, &info, nullptr, &handle));
            vkDestroyShaderModule(Device.Device, shader, nullptr);
            pipeline.Pipelines.push_back(handle);
        }
        return pipeline;
    }

    VkDescriptorSet Allocate(const ComputePipeline& pipeline) {
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = mPool;
        allocation.descriptorSetCount = 1;
        allocation.pSetLayouts = &pipeline.SetLayout;
        VkDescriptorSet set{};
        Check(vkAllocateDescriptorSets(Device.Device, &allocation, &set));
        return set;
    }

    void Begin() {
        Check(vkResetCommandPool(Device.Device, mCommands, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(mCommand, &begin));
    }

    void Barrier(VkAccessFlags from, VkAccessFlags to, VkPipelineStageFlags destination) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = from;
        barrier.dstAccessMask = to;
        vkCmdPipelineBarrier(mCommand, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, destination, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
    }

    void Submit() {
        Check(vkEndCommandBuffer(mCommand));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &mCommand;
        Check(vkQueueSubmit(Device.Queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(Device.Queue));
    }

    ComputePipeline mGather, mUnit;
    VkDescriptorPool mPool{};
    VkCommandPool mCommands{};
    VkCommandBuffer mCommand{};
};

struct Comparison {
    double MaxJ = 0.0, MaxA = 0.0, MaxC = 0.0, MeanJ = 0.0, MeanA = 0.0;
    size_t OverJ = 0, OverA = 0, ValidMismatch = 0, NonFinite = 0, Pixels = 0;
};

// |dJ| relative to max(1, |J|) and |dA|; "over" counts pixels beyond `threshold`.
// Excluded pixels (geometric-normal tie flips) are counted apart.
Comparison Compare(const GpuResult& gpu, const std::vector<PixelGather>& cpu, double threshold,
                   const std::vector<bool>& excluded) {
    Comparison c;
    c.Pixels = cpu.size();
    for (size_t i = 0; i < cpu.size(); ++i) {
        if (excluded[i]) continue;
        const float* r = &gpu.Results[4 * i];
        const float* a = &gpu.Aux[4 * i];
        if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]) || !std::isfinite(r[3]) ||
            !std::isfinite(a[0]))
            ++c.NonFinite;
        if ((a[1] != 0.0F) != cpu[i].Valid) ++c.ValidMismatch;
        const double scale = std::max({1.0, std::abs(cpu[i].J.R), std::abs(cpu[i].J.G), std::abs(cpu[i].J.B)});
        const double dj = std::max({std::abs(r[0] - cpu[i].J.R), std::abs(r[1] - cpu[i].J.G),
                                    std::abs(r[2] - cpu[i].J.B)}) / scale;
        const double da = std::abs(r[3] - cpu[i].ARaw);
        c.MaxJ = std::max(c.MaxJ, dj);
        c.MaxA = std::max(c.MaxA, da);
        c.MaxC = std::max(c.MaxC, std::abs(a[0] - cpu[i].Confidence));
        c.MeanJ += dj;
        c.MeanA += da;
        if (dj > threshold) ++c.OverJ;
        if (da > threshold) ++c.OverA;
    }
    c.MeanJ /= static_cast<double>(std::max<size_t>(c.Pixels, 1));
    c.MeanA /= static_cast<double>(std::max<size_t>(c.Pixels, 1));
    return c;
}

struct Case {
    std::string Name;
    Scene SceneValue;
    GatherSettings Settings;
    bool FaceNormals = false;
    bool QuantizeNormals = false;
};

uint32_t Lcg(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}
float Uniform(uint32_t& state, float lo, float hi) {
    return lo + (hi - lo) * static_cast<float>(Lcg(state) >> 8) / 16777216.0F;
}

} // namespace

int main() try {
    DiffuseGiGpu gpu;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu.Device.Physical, &properties);
    std::printf("DGI_GPU device=\"%s\" driver=0x%x api=%u.%u.%u\n", properties.deviceName, properties.driverVersion,
                VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
                VK_API_VERSION_PATCH(properties.apiVersion));

    // --- Unit kernel: masks (bit-exact against the FP32 CPU rule) and weights.
    uint32_t state = 12345U;
    size_t maskCases = 0, maskMismatch = 0;
    for (const uint32_t sectors : {1U, 2U, 31U, 32U, 33U, 63U, 64U, 65U, 96U, 100U, 255U, 256U}) {
        std::vector<std::array<float, 4>> intervals;
        const float step = 2.0F * static_cast<float>(kPi) / static_cast<float>(sectors);
        intervals.push_back({-static_cast<float>(kPi), static_cast<float>(kPi), 0, 0}); // full
        intervals.push_back({0.5F, 0.5F, 0, 0});                                        // empty
        intervals.push_back({1.0F, 0.2F, 0, 0});                                        // reversed: empty
        for (uint32_t j = 0; j + 1 < sectors && j < 40; ++j)                            // across word boundaries
            intervals.push_back({-static_cast<float>(kPi) + step * (static_cast<float>(j) + 0.25F),
                                 -static_cast<float>(kPi) + step * (static_cast<float>(j) + 33.75F), 0, 0});
        for (int k = 0; k < 400; ++k) {
            float a = Uniform(state, -static_cast<float>(kPi), static_cast<float>(kPi));
            float b = Uniform(state, -static_cast<float>(kPi), static_cast<float>(kPi));
            if (a > b) std::swap(a, b);
            intervals.push_back({a, b, 0, 0});
        }
        const auto words = gpu.RunUnit(intervals, sectors, 0);
        for (size_t i = 0; i < intervals.size(); ++i) {
            SectorMask expected;
            const auto [first, end] = CentreRuleRangeF32(intervals[i][0], intervals[i][1], sectors);
            SetSectorRange(expected, first, end);
            ++maskCases;
            for (uint32_t w = 0; w < kMaskWords; ++w)
                if (words[8 * i + w] != expected.Words[w]) {
                    ++maskMismatch;
                    break;
                }
        }
        Require(words[0] == (sectors >= 32 ? 0xFFFFFFFFU : (1U << sectors) - 1U), "full interval mask is wrong");
    }
    std::vector<std::array<float, 4>> weightInputs;
    for (int k = 0; k < 4096; ++k) {
        const float tilt = Uniform(state, 0.0F, 3.1F), azimuth = Uniform(state, -3.1F, 3.1F);
        float lo = Uniform(state, -3.14159F, 3.14159F), hi = Uniform(state, -3.14159F, 3.14159F);
        if (lo > hi) std::swap(lo, hi);
        weightInputs.push_back({std::cos(tilt), std::sin(tilt) * std::cos(azimuth), lo, hi});
    }
    const auto weights = gpu.RunUnit(weightInputs, 0, 1);
    double weightVsDouble = 0.0, weightVsF32 = 0.0;
    for (size_t i = 0; i < weightInputs.size(); ++i) {
        float value;
        std::memcpy(&value, &weights[8 * i], 4);
        const auto& w = weightInputs[i];
        weightVsDouble = std::max(weightVsDouble, std::abs(value - SectorWeightAnalytic(w[0], w[1], w[2], w[3])));
        weightVsF32 = std::max(weightVsF32,
                               std::abs(static_cast<double>(value) - SectorWeightAnalyticF32(w[0], w[1], w[2], w[3])));
    }
    std::printf("DGI_GPU unit masks=%zu mismatches_vs_cpu_f32=%zu weights=%zu max_abs_vs_f64=%.2e max_abs_vs_cpu_f32=%.2e\n",
                maskCases, maskMismatch, weightInputs.size(), weightVsDouble, weightVsF32);
    Require(maskMismatch == 0, "GPU sector masks differ from the CPU centre rule");
    Require(weightVsDouble < 2.0e-6, "GPU sector weights exceed the FP32 tolerance");

    // --- Gathering on synthetic scenes.
    std::vector<Case> cases;
    {
        Case c{"t06_131x77_every_N64", BlackOccluderScene(131, 77, true), {}};
        c.Settings.Radius = 5.0, c.Settings.Thickness = 1.0, c.Settings.Sectors = 64;
        cases.push_back(c);
    }
    {
        Case c{"t06_padded_viewport", BlackOccluderScene(131, 77, true), {}};
        c.SceneValue.PadLeft = 5, c.SceneValue.PadTop = 3, c.SceneValue.PadRight = 9, c.SceneValue.PadBottom = 7;
        c.Settings.Radius = 5.0, c.Settings.Thickness = 1.0, c.Settings.Sectors = 64;
        cases.push_back(c);
    }
    {
        Case c{"t05_words_97x61_N96", WordBoundaryScene(97, 61), {}};
        c.Settings.Radius = 1.5, c.Settings.Thickness = 0.4, c.Settings.Sectors = 96;
        cases.push_back(c);
    }
    {
        Case c{"corner_127x93_radial16_N32_S6", CornerRoomScene(127, 93), {}};
        c.Settings.Radius = 2.5, c.Settings.Thickness = 0.5, c.Settings.Sectors = 32, c.Settings.Sections = 6;
        c.Settings.Schedule = CandidateSchedule::Radial, c.Settings.CandidatesPerSide = 16;
        c.Settings.Rotation = 0.3;
        cases.push_back(c);
    }
    {
        Case c{"t07_bar_101x67_N256_tau0.1", ThinBarScene(101, 67), {}};
        c.Settings.Radius = 4.0, c.Settings.Thickness = 0.1, c.Settings.Sectors = 256;
        cases.push_back(c);
    }
    {
        Case c{"t07_block_89x57_N33_sides_unmerged", ThickBlockScene(89, 57), {}};
        c.Settings.Radius = 6.0, c.Settings.Thickness = 0.5, c.Settings.Sectors = 33;
        c.Settings.InterleaveSides = false;
        cases.push_back(c);
    }
    {
        Case c{"t08_near_83x59_N64", NearPlaneScene(83, 59), {}};
        c.Settings.Radius = 0.5, c.Settings.Thickness = 0.05, c.Settings.Sectors = 64;
        cases.push_back(c);
    }
    {
        Case c{"t08_reversed_infinite_far", BlackOccluderScene(77, 53, true), {}};
        c.SceneValue.Camera.Encoding = DepthEncoding::ReversedPerspectiveZeroToOne;
        c.SceneValue.Camera.Far = std::numeric_limits<double>::infinity();
        c.Settings.Radius = 5.0, c.Settings.Thickness = 1.0, c.Settings.Sectors = 64;
        cases.push_back(c);
    }
    {
        Case c{"t08_linear_wbuffer_hud", PillarsScene(91, 63), {}};
        c.SceneValue.Camera.Encoding = DepthEncoding::LinearWBuffer;
        c.SceneValue.Unavailable.push_back({40, 20, 60, 34});
        c.Settings.Radius = 3.0, c.Settings.Thickness = 0.5, c.Settings.Sectors = 64;
        cases.push_back(c);
    }
    {
        Case c{"t08_zero_thickness", PillarsScene(91, 63), {}};
        c.Settings.Radius = 3.0, c.Settings.Thickness = 0.0, c.Settings.Sectors = 64;
        cases.push_back(c);
    }
    {
        Case c{"t06_renderer_face_normals_f32", BlackOccluderScene(103, 71, true), {}};
        c.Settings.Radius = 5.0, c.Settings.Thickness = 1.0, c.Settings.Sectors = 64;
        c.FaceNormals = true;
        cases.push_back(c);
    }
    {
        Case c{"sloped_rgb8_face_normals", SlopedPlaneScene(96, 64, 1.05), {}};
        c.Settings.Radius = 1.5, c.Settings.Thickness = 0.5, c.Settings.Sectors = 64;
        c.FaceNormals = true, c.QuantizeNormals = true;
        cases.push_back(c);
    }

    std::vector<float> unpaddedT06;
    for (auto& c : cases) {
        c.Settings.Threads = 4;
        GatherInput input = Rasterize(c.SceneValue);
        if (c.FaceNormals) input.NormalSource = GeometricNormalSource::RendererFaceNormal;
        if (c.QuantizeNormals) QuantizeFaceNormals8(input);
        Require(*ValidateGatherInput(input) == '\0', ValidateGatherInput(input));
        Require(*ValidateGatherSettings(c.Settings) == '\0', ValidateGatherSettings(c.Settings));
        const GpuResult result = gpu.Run(input, c.Settings);
        const auto f32 = GatherImageF32(input, c.Settings);
        const auto f64 = GatherImage(input, c.Settings);
        // Normals: GPU normal pass against the FP32 twin. At a texel lying on
        // two planes (a junction) the two implementations may pick different
        // planes within the tie tolerance: allowed only if the GPU normal is
        // the exact face normal of a 3x3 neighbour (never a mixed vector).
        const PreparedGeometry g32 = PrepareGeometryF32(input);
        size_t normalMismatch = 0, normalTieFlips = 0;
        std::vector<bool> excluded(f64.size(), false);
        for (uint32_t y = 0; y < input.Viewport.Height; ++y)
            for (uint32_t x = 0; x < input.Viewport.Width; ++x) {
                const size_t bi = static_cast<size_t>(y + input.Viewport.Y) * input.BufferWidth + x + input.Viewport.X;
                const Vec3& n = g32.Normal[static_cast<size_t>(y) * input.Viewport.Width + x];
                const Vec3 g{result.Normals[4 * bi], result.Normals[4 * bi + 1], result.Normals[4 * bi + 2]};
                const bool cpuValid = Dot(n, n) > 0.0;
                if (cpuValid != (result.Normals[4 * bi + 3] != 0.0F)) {
                    ++normalMismatch;
                    continue;
                }
                if (!cpuValid || Dot(n, g) >= 0.99999) continue;
                bool neighbourFace = false;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = static_cast<int>(x) + dx, yy = static_cast<int>(y) + dy;
                        if (xx < 0 || yy < 0 || xx >= static_cast<int>(input.Viewport.Width) ||
                            yy >= static_cast<int>(input.Viewport.Height))
                            continue;
                        const size_t ni =
                            static_cast<size_t>(yy + input.Viewport.Y) * input.BufferWidth + xx + input.Viewport.X;
                        const Vec3 face{input.FaceNormals[3 * ni], input.FaceNormals[3 * ni + 1],
                                        input.FaceNormals[3 * ni + 2]};
                        neighbourFace = neighbourFace || Dot(face, g) > 0.9999;
                    }
                if (neighbourFace && !c.FaceNormals) {
                    ++normalTieFlips;
                    excluded[static_cast<size_t>(y) * input.Viewport.Width + x] = true;
                } else {
                    ++normalMismatch;
                }
            }
        const Comparison vs32 = Compare(result, f32, 1.0e-4, excluded);
        const Comparison vs64 = Compare(result, f64, 1.0e-3, excluded);
        std::printf("DGI_GPU case=%s extent=%ux%u N=%u S=%u guards=%d outside_untouched=%d nonfinite=%zu valid_mismatch=%zu "
                    "normals_mismatch=%zu normal_tie_flips=%zu | vs_cpu_f32: maxJ=%.2e maxA=%.2e meanJ=%.2e meanA=%.2e "
                    "over1e-4 J=%zu A=%zu maxC=%.1e | vs_f64: maxJ=%.2e maxA=%.2e meanJ=%.2e meanA=%.2e over1e-3 J=%zu A=%zu\n",
                    c.Name.c_str(), input.Viewport.Width, input.Viewport.Height, c.Settings.Sectors, c.Settings.Sections,
                    result.GuardsIntact, result.OutsideNormalsUntouched, vs32.NonFinite, vs32.ValidMismatch,
                    normalMismatch, normalTieFlips, vs32.MaxJ, vs32.MaxA, vs32.MeanJ, vs32.MeanA, vs32.OverJ, vs32.OverA,
                    vs32.MaxC, vs64.MaxJ, vs64.MaxA, vs64.MeanJ, vs64.MeanA, vs64.OverJ, vs64.OverA);
        Require(result.GuardsIntact, "GPU wrote past the output extent");
        Require(result.OutsideNormalsUntouched, "GPU wrote normals outside the viewport");
        Require(vs32.NonFinite == 0, "GPU produced non-finite output");
        Require(vs32.ValidMismatch == 0, "GPU validity differs from the CPU");
        Require(normalMismatch == 0, "GPU geometric normals differ from the FP32 twin");
        Require(normalTieFlips <= vs32.Pixels / 1000, "more than 0.1% of GPU normals flip between junction planes");
        // FP32 tolerance model (document): continuous quantities agree to
        // rounding; isolated discrete decisions (sector-centre, chord, self
        // and R thresholds) may flip, each bounded by one sector of one
        // section: 2 pi / (N S) of A and that times the radiance of J.
        float maxRadiance = 1.0F;
        for (uint32_t y = 0; y < input.Viewport.Height; ++y)
            for (uint32_t x = 0; x < input.Viewport.Width; ++x) {
                const size_t bi = static_cast<size_t>(y + input.Viewport.Y) * input.BufferWidth + x + input.Viewport.X;
                for (int k = 0; k < 3; ++k) maxRadiance = std::max(maxRadiance, input.Radiance[3 * bi + k]);
            }
        const double flip = 2.0 * kPi / (c.Settings.Sectors * c.Settings.Sections);
        for (const Comparison* comparison : {&vs32, &vs64}) {
            Require(comparison->MeanJ < 2.0e-5 && comparison->MeanA < 2.0e-5, "GPU mean deviation beyond FP32 rounding");
            Require(comparison->MaxA <= 2.0 * flip, "GPU accessibility deviation beyond two sector flips");
            Require(comparison->MaxJ <= 2.0 * flip * maxRadiance, "GPU irradiance deviation beyond two sector flips");
            Require(comparison->MaxC < 5.0e-3, "GPU confidence deviation beyond one scheduled candidate");
        }
        Require(vs32.OverJ + vs32.OverA <= vs32.Pixels / 100, "more than 1% of GPU pixels flip");
        if (c.Name == "t06_131x77_every_N64") unpaddedT06 = result.Results;
        if (c.Name == "t06_padded_viewport")
            Require(unpaddedT06 == result.Results, "shifted viewport changes the GPU result");
        if (c.Settings.Thickness == 0.0)
            for (size_t i = 0; i < f64.size(); ++i)
                Require(result.Results[4 * i] == 0.0F && result.Results[4 * i + 1] == 0.0F &&
                            result.Results[4 * i + 2] == 0.0F,
                        "zero thickness produced light");
    }
    std::printf("DGI_GPU passed\n");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
