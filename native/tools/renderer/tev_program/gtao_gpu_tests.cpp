#include "fast/renderer/gtao_shader.h"
#include "vulkan_test_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using Fast::Renderer::GtaoConstants;

namespace {
struct Image {
    VkImage Handle{};
    VkImageView View{};
    VkDeviceMemory Memory{};
};

// Actual production kernels, native RGB8 normals and half-resolution RG16F
// scratch. Odd extents exercise the cooperative resolve's partial workgroups.
struct GtaoFixture {
    static constexpr uint32_t Width = 129, Height = 97;
    Gpu Device{ VkPhysicalDeviceFeatures{ .shaderStorageImageExtendedFormats = VK_TRUE } };
    std::array<Image, 4> Images{};
    Gpu::Buffer Depths{}, Normals{}, Result{};
    VkSampler Sampler{};
    VkDescriptorSetLayout SetLayout{};
    VkDescriptorPool Pool{};
    std::array<VkDescriptorSet, 2> Sets{};
    VkPipelineLayout Layout{};
    std::array<VkPipeline, 2> Pipelines{};
    VkCommandPool Commands{};
    VkCommandBuffer Command{};

    Image MakeImage(VkFormat format, uint32_t width, uint32_t height) {
        Image image;
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Check(vkCreateImage(Device.Device, &info, nullptr, &image.Handle));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(Device.Device, image.Handle, &requirements);
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = Device.Type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(Device.Device, &allocation, nullptr, &image.Memory));
        Check(vkBindImageMemory(Device.Device, image.Handle, image.Memory, 0));
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = image.Handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Check(vkCreateImageView(Device.Device, &view, nullptr, &image.View));
        return image;
    }

    GtaoFixture() {
        Images = { MakeImage(VK_FORMAT_R32_SFLOAT, Width, Height), MakeImage(VK_FORMAT_R8G8B8A8_UNORM, Width, Height),
                   MakeImage(VK_FORMAT_R16G16_SFLOAT, (Width + 1) / 2, (Height + 1) / 2),
                   MakeImage(VK_FORMAT_R32_SFLOAT, Width, Height) };
        Depths = Device.MakeBuffer(Width * Height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Normals = Device.MakeBuffer(Width * Height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Result = Device.MakeBuffer(Width * Height * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(vkCreateSampler(Device.Device, &sampler, nullptr, &Sampler));
        std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i == 4   ? VK_DESCRIPTOR_TYPE_SAMPLER
                                         : i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        VkDescriptorSetLayoutCreateInfo setLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setLayout.bindingCount = static_cast<uint32_t>(bindings.size());
        setLayout.pBindings = bindings.data();
        Check(vkCreateDescriptorSetLayout(Device.Device, &setLayout, nullptr, &SetLayout));
        const std::array<VkDescriptorPoolSize, 3> sizes{ { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6 },
                                                           { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
                                                           { VK_DESCRIPTOR_TYPE_SAMPLER, 2 } } };
        VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets = 2;
        pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool.pPoolSizes = sizes.data();
        Check(vkCreateDescriptorPool(Device.Device, &pool, nullptr, &Pool));
        const std::array<VkDescriptorSetLayout, 2> layouts{ SetLayout, SetLayout };
        VkDescriptorSetAllocateInfo allocation{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocation.descriptorPool = Pool;
        allocation.descriptorSetCount = 2;
        allocation.pSetLayouts = layouts.data();
        Check(vkAllocateDescriptorSets(Device.Device, &allocation, Sets.data()));
        for (uint32_t stage = 0; stage < 2; ++stage) {
            std::array<VkDescriptorImageInfo, 5> images{};
            std::array<VkWriteDescriptorSet, 5> writes{};
            for (uint32_t binding = 0; binding < 5; ++binding) {
                const uint32_t index = binding == 3 ? stage + 2 : binding == 2 ? (stage ? 2 : 0) : binding;
                images[binding].imageView = binding == 4 ? VK_NULL_HANDLE : Images[index].View;
                images[binding].imageLayout =
                    binding == 3 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                images[binding].sampler = binding == 4 ? Sampler : VK_NULL_HANDLE;
                writes[binding] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[binding].dstSet = Sets[stage];
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = bindings[binding].descriptorType;
                writes[binding].pImageInfo = &images[binding];
            }
            vkUpdateDescriptorSets(Device.Device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GtaoConstants) };
        VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &SetLayout;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        Check(vkCreatePipelineLayout(Device.Device, &layout, nullptr, &Layout));
        const std::array sources{ Fast::Renderer::BuildMultiscaleGtaoShader(),
                                  Fast::Renderer::BuildMultiscaleGtaoResolveShader() };
        for (uint32_t stage = 0; stage < 2; ++stage) {
            const auto shader = Device.Shader(sources[stage], shaderc_compute_shader);
            VkComputePipelineCreateInfo pipeline{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            pipeline.layout = Layout;
            pipeline.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                               nullptr,
                               0,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               shader,
                               "main",
                               nullptr };
            Check(vkCreateComputePipelines(Device.Device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &Pipelines[stage]));
            vkDestroyShaderModule(Device.Device, shader, nullptr);
        }
        VkCommandPoolCreateInfo commands{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        commands.queueFamilyIndex = Device.Family;
        Check(vkCreateCommandPool(Device.Device, &commands, nullptr, &Commands));
        VkCommandBufferAllocateInfo command{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command.commandPool = Commands;
        command.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(Device.Device, &command, &Command));
    }

    void Barrier(const Image& image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags from,
                 VkAccessFlags to, VkPipelineStageFlags source, VkPipelineStageFlags destination) {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = from;
        barrier.dstAccessMask = to;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.Handle;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(Command, source, destination, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    std::vector<float> Run(const std::vector<float>& depth, const GtaoConstants& constants, uint8_t coverage = 255,
                           float slope = 0) {
        auto* encodedDepth = static_cast<float*>(Depths.Mapped);
        auto* encodedNormals = static_cast<std::array<uint8_t, 4>*>(Normals.Mapped);
        const float length = std::sqrt(1 + slope * slope);
        const std::array<uint8_t, 4> normal = { static_cast<uint8_t>(std::round((.5F + .5F * slope / length) * 255)),
                                                128, static_cast<uint8_t>(std::round((.5F + .5F / length) * 255)),
                                                coverage };
        for (size_t i = 0; i < depth.size(); ++i) {
            encodedDepth[i] = constants.Depth[3] != 0 ? (depth[i] - 1) / 99 : (100 - 100 / depth[i]) / 99;
            encodedNormals[i] = normal;
        }
        Check(vkResetCommandPool(Device.Device, Commands, 0));
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        Check(vkBeginCommandBuffer(Command, &begin));
        for (uint32_t i = 0; i < 2; ++i) {
            Barrier(Images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { Width, Height, 1 };
            vkCmdCopyBufferToImage(Command, i == 0 ? Depths.Handle : Normals.Handle, Images[i].Handle,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(Images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        for (uint32_t i = 2; i < 4; ++i)
            Barrier(Images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        for (uint32_t stage = 0; stage < 2; ++stage) {
            vkCmdBindPipeline(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Pipelines[stage]);
            vkCmdBindDescriptorSets(Command, VK_PIPELINE_BIND_POINT_COMPUTE, Layout, 0, 1, &Sets[stage], 0, nullptr);
            vkCmdPushConstants(Command, Layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            const uint32_t width = stage == 0 ? (Width + 1) / 2 : Width;
            const uint32_t height = stage == 0 ? (Height + 1) / 2 : Height;
            const uint32_t groupWidth = stage == 0 ? 8 : 16;
            vkCmdDispatch(Command, (width + groupWidth - 1) / groupWidth, (height + 7) / 8, 1);
            if (stage == 0)
                Barrier(Images[2], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        Barrier(Images[3], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { Width, Height, 1 };
        vkCmdCopyImageToBuffer(Command, Images[3].Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Result.Handle, 1,
                               &copy);
        VkMemoryBarrier host{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                             nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(Command));
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &Command;
        Check(vkQueueSubmit(Device.Queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(Device.Queue));
        std::vector<float> values(depth.size());
        std::memcpy(values.data(), Result.Mapped, values.size() * sizeof(float));
        for (float value : values)
            Require(std::isfinite(value) && value >= 0 && value <= 1, "nonfinite or out-of-range AO visibility");
        return values;
    }

    ~GtaoFixture() {
        vkDestroyCommandPool(Device.Device, Commands, nullptr);
        for (auto pipeline : Pipelines)
            vkDestroyPipeline(Device.Device, pipeline, nullptr);
        vkDestroyPipelineLayout(Device.Device, Layout, nullptr);
        vkDestroyDescriptorPool(Device.Device, Pool, nullptr);
        vkDestroyDescriptorSetLayout(Device.Device, SetLayout, nullptr);
        vkDestroySampler(Device.Device, Sampler, nullptr);
        Device.Free(Depths);
        Device.Free(Normals);
        Device.Free(Result);
        for (const auto& image : Images) {
            vkDestroyImageView(Device.Device, image.View, nullptr);
            vkDestroyImage(Device.Device, image.Handle, nullptr);
            vkFreeMemory(Device.Device, image.Memory, nullptr);
        }
    }
};

float Difference(const std::vector<float>& a, const std::vector<float>& b) {
    float difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference = std::max(difference, std::abs(a[i] - b[i]));
    return difference;
}
} // namespace

int main() try {
    GtaoFixture gpu;
    GtaoConstants constants;
    constants.Width = GtaoFixture::Width;
    constants.Height = GtaoFixture::Height;
    constants.ViewX = { 2, 0, -1, 0 };
    constants.ViewY = { 0, 2, -1, 0 };
    constants.Depth = { 100, 100, 99, 0 };
    constants.RadiusFade = { .45F, 3, 50, 100 };
    constants.Response = { 1, 1, 1, 1 };
    constants.Filter = { .8F, 4.0F / (255.0F * 255.0F), 0, 0 };
    const std::vector<float> flat(GtaoFixture::Width * GtaoFixture::Height, 10), identity(flat.size(), 1);
    Require(Difference(gpu.Run(flat, constants), identity) < .0002F, "RGB8 empty plane darkens");
    auto sloped = flat;
    for (uint32_t y = 0; y < GtaoFixture::Height; ++y)
        for (uint32_t x = 0; x < GtaoFixture::Width; ++x)
            sloped[y * GtaoFixture::Width + x] = 10 / (1 - .3F * (2 * (x + .5F) / GtaoFixture::Width - 1));
    // Failed by 0.0213 visibility before accounting for native normal precision.
    Require(Difference(gpu.Run(sloped, constants, 255, .3F), identity) < .001F, "RGB8 sloped plane darkens");
    auto scene = flat;
    for (uint32_t y = 15; y < 80; ++y)
        for (uint32_t x = 15; x < 110; ++x) {
            if (x < 40)
                scene[y * GtaoFixture::Width + x] = 9.8F;
            else if (x > 75)
                scene[y * GtaoFixture::Width + x] = 8.5F;
        }
    const auto combined = gpu.Run(scene, constants);
    Require(Difference(combined, gpu.Run(scene, constants, 64)) == 0, "valid Grass coverage was excluded");
    auto fineOnly = constants, broadOnly = constants;
    fineOnly.Response[1] = 0;
    broadOnly.Response[0] = 0;
    const auto fine = gpu.Run(scene, fineOnly), broad = gpu.Run(scene, broadOnly);
    Require(*std::min_element(fine.begin(), fine.end()) < .99F && *std::min_element(broad.begin(), broad.end()) < .99F,
            "one occlusion scale is inert");
    for (size_t i = 0; i < fine.size(); ++i)
        Require(std::abs(combined[i] - std::max(0.0F, fine[i] + broad[i] - 1)) < .001F,
                "fine and broad gains are coupled");
    auto disabled = constants;
    disabled.Response[0] = disabled.Response[1] = 0;
    Require(Difference(gpu.Run(scene, disabled), identity) == 0, "zero gains alter visibility");
    Require(Difference(gpu.Run(scene, constants, 0), identity) == 0, "missing scene coverage darkens");
    auto wBuffer = constants;
    wBuffer.Depth = { 1, 100, 99, 1 };
    Require(Difference(combined, gpu.Run(scene, wBuffer)) < .001F, "W-buffer and perspective reconstruction disagree");
    Require(Difference(combined, gpu.Run(scene, constants)) == 0, "AO depends on previous frame contents");
    std::cout
        << "GTAO GPU: RGB8 planes, independent scales, coverage, W-buffer, odd extents and repeatability passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
