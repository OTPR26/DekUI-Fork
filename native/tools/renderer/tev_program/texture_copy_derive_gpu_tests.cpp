#include "fast/renderer3ds/pica_texture_copy_derive_gpu.h"
#include "vulkan_test_device.h"

#include <array>
#include <cstring>
#include <span>

namespace {
struct Image {
    Gpu& G;
    VkImage Handle{};
    VkDeviceMemory Memory{};
    VkImageView View{};
    Image(Gpu& gpu, uint32_t width, uint32_t height) : G(gpu) {
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { width, height, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateImage(G.Device, &info, nullptr, &Handle));
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(G.Device, Handle, &requirements);
        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = G.Type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(G.Device, &alloc, nullptr, &Memory));
        Check(vkBindImageMemory(G.Device, Handle, Memory, 0));
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = Handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Check(vkCreateImageView(G.Device, &view, nullptr, &View));
    }
    ~Image() {
        vkDestroyImageView(G.Device, View, nullptr);
        vkDestroyImage(G.Device, Handle, nullptr);
        vkFreeMemory(G.Device, Memory, nullptr);
    }
};

struct Push {
    std::array<uint32_t, 4> Rect;
    std::array<int32_t, 4> Delta;
    std::array<uint32_t, 4> Layout;
    std::array<uint32_t, 4> Extent;
};
static_assert(sizeof(Push) == 64);
using Pixel = std::array<uint8_t, 4>;

Pixel Quantize(Pixel p, uint32_t format) {
    const auto expand5 = [](uint8_t v) { return uint8_t((v & 248) | (v >> 5)); };
    if (format == 1)
        p[3] = 255;
    if (format == 2 || format == 3) {
        p[0] = expand5(p[0]);
        p[2] = expand5(p[2]);
        p[1] = format == 2 ? uint8_t((p[1] & 252) | (p[1] >> 6)) : expand5(p[1]);
        p[3] = format == 2 || p[3] >= 128 ? 255 : 0;
    }
    if (format == 4)
        for (auto& c : p)
            c = uint8_t((c >> 4) * 17);
    return p;
}

void Run() {
    Gpu gpu;
    // Non-integral vertical scale, padded destination and two adjacent writes:
    // catches native-grid reduction, translation errors and loss of earlier regions.
    constexpr uint32_t sw = 48, sh = 50, dw = 96, dh = 128;
    std::vector<Pixel> pixels(sw * sh);
    for (uint32_t y = 0; y < sh; ++y)
        for (uint32_t x = 0; x < sw; ++x)
            pixels[y * sw + x] = { uint8_t(x * 17 + y * 3), uint8_t(x * 7 + y * 19), uint8_t(x * 11 + y * 5),
                                   uint8_t(x * 13 + y * 23) };
    Image source(gpu, sw, sh), destination(gpu, dw, dh);
    auto upload = gpu.MakeBuffer(pixels.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(upload.Mapped, pixels.data(), pixels.size() * 4);
    auto readback = gpu.MakeBuffer(dw * dh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDescriptorSetLayoutBinding bindings[]{
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
    };
    VkDescriptorSetLayoutCreateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setInfo.bindingCount = 3;
    setInfo.pBindings = bindings;
    VkDescriptorSetLayout setLayout{};
    Check(vkCreateDescriptorSetLayout(gpu.Device, &setInfo, nullptr, &setLayout));
    VkPushConstantRange pushRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout{};
    Check(vkCreatePipelineLayout(gpu.Device, &layoutInfo, nullptr, &layout));
    auto shader =
        gpu.Shader(std::string(Fast::Renderer3ds::PicaTextureCopyDeriveComputeSource), shaderc_compute_shader);
    VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.layout = layout;
    pipelineInfo.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "main";
    VkPipeline pipeline{};
    Check(vkCreateComputePipelines(gpu.Device, {}, 1, &pipelineInfo, nullptr, &pipeline));
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.maxLod = 0;
    VkSampler sampler{};
    Check(vkCreateSampler(gpu.Device, &samplerInfo, nullptr, &sampler));
    VkDescriptorPoolSize sizes[]{ { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 },
                                  { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
                                  { VK_DESCRIPTOR_TYPE_SAMPLER, 1 } };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    Check(vkCreateDescriptorPool(gpu.Device, &poolInfo, nullptr, &pool));
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout;
    VkDescriptorSet set{};
    Check(vkAllocateDescriptorSets(gpu.Device, &allocate, &set));
    VkDescriptorImageInfo images[]{ { {}, source.View, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                    { {}, destination.View, VK_IMAGE_LAYOUT_GENERAL },
                                    { sampler, {}, VK_IMAGE_LAYOUT_UNDEFINED } };
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(gpu.Device, 3, writes.data(), 0, nullptr);
    VkCommandPoolCreateInfo commandPoolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    commandPoolInfo.queueFamilyIndex = gpu.Family;
    commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commandPool{};
    Check(vkCreateCommandPool(gpu.Device, &commandPoolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo commandInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    commandInfo.commandPool = commandPool;
    commandInfo.commandBufferCount = 1;
    VkCommandBuffer command{};
    Check(vkAllocateCommandBuffers(gpu.Device, &commandInfo, &command));
    const auto barrier = [&](VkImage image, VkImageLayout before, VkImageLayout after, VkAccessFlags src,
                             VkAccessFlags dst) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = before;
        b.newLayout = after;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    };
    uint64_t failures = 0;
    // Each format in both row orders: a source decoded from native bytes (rows
    // as the destination stores them) and a captured attachment (rows mirrored).
    for (uint32_t pass = 0; pass < 10; ++pass) {
        const uint32_t format = pass % 5;
        const int32_t attachmentRows = pass < 5 ? 0 : 1;
        Check(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        Check(vkBeginCommandBuffer(command, &begin));
        if (pass == 0) {
            barrier(source.Handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { sw, sh, 1 };
            vkCmdCopyBufferToImage(command, upload.Handle, source.Handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
            barrier(source.Handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        barrier(destination.Handle, pass == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, pass == 0 ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue base{ { 17.0F / 255, 31.0F / 255, 63.0F / 255, 127.0F / 255 } };
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(command, destination.Handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &base, 1, &range);
        barrier(destination.Handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        for (uint32_t half = 0; half < 2; ++half) {
            Push push{ { 24 + half * 24, 32, 24, 64 }, { -8, -8, attachmentRows, 0 }, { 3, 4, 16, 16 },
                       { sw, sh, format, 0 } };
            vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(command, 3, 8, 1);
        }
        barrier(destination.Handle, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { dw, dh, 1 };
        vkCmdCopyImageToBuffer(command, destination.Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.Handle, 1,
                               &copy);
        VkMemoryBarrier host{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                             nullptr, 0, nullptr);
        Check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(gpu.Queue, 1, &submit, {}));
        Check(vkQueueWaitIdle(gpu.Queue));
        const auto* actual = static_cast<const Pixel*>(readback.Mapped);
        for (uint32_t y = 0; y < dh; ++y)
            for (uint32_t x = 0; x < dw; ++x) {
                Pixel expected{ 17, 31, 63, 127 };
                if (x >= 24 && x < 72 && y >= 32 && y < 96) {
                    // Half-texel rows over the 16 native rows at density 4: a
                    // mirrored source reads native row 16 - v instead of v.
                    const uint32_t row = 2 * (y - 32) + 1;
                    const uint32_t sx = ((2 * (x - 24) + 1) * sw) / 96;
                    const uint32_t sy = ((attachmentRows ? 128 - row : row) * sh) / 128;
                    expected = Quantize(pixels[sy * sw + sx], format);
                }
                if (actual[y * dw + x] != expected) {
                    if (failures < 4)
                        std::cerr << "format=" << format << " attachment_rows=" << attachmentRows
                                  << " pixel=" << x << ',' << y
                                  << " expected.r=" << unsigned(expected[0])
                                  << " actual.r=" << unsigned(actual[y * dw + x][0]) << '\n';
                    ++failures;
                }
            }
    }
    std::cout << "GPU copy derivation: " << dw * dh * 10 << " pixels, mismatches=" << failures << '\n';
    vkDestroyCommandPool(gpu.Device, commandPool, nullptr);
    vkDestroyDescriptorPool(gpu.Device, pool, nullptr);
    vkDestroySampler(gpu.Device, sampler, nullptr);
    vkDestroyPipeline(gpu.Device, pipeline, nullptr);
    vkDestroyShaderModule(gpu.Device, shader, nullptr);
    vkDestroyPipelineLayout(gpu.Device, layout, nullptr);
    vkDestroyDescriptorSetLayout(gpu.Device, setLayout, nullptr);
    gpu.Free(upload);
    gpu.Free(readback);
    Require(failures == 0, "GPU copy changes source pixels or untouched padding");
}
} // namespace

int main() {
    try {
        Run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
