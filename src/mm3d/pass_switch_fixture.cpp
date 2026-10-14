// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/pass_switch_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include "mm3d/prompt_layer.h"
#include "video_core/rasterizer_cache/pixel_format.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace MM3D {
namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
double Milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}
}

void RunPassSwitchFixture(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler) {
    const auto device = instance.GetDevice();
    const auto physical = instance.GetPhysicalDevice();
    const auto memory = physical.getMemoryProperties();
    const auto allocate = [&](vk::MemoryRequirements requirements, vk::MemoryPropertyFlags flags) {
        for (unsigned i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return device.allocateMemoryUnique({.allocationSize = requirements.size, .memoryTypeIndex = i});
            }
        }
        throw std::runtime_error("No compatible pass-switch fixture memory");
    };
    Vulkan::RenderManager manager(instance, scheduler);
    {
        // Exercise chunk rollover and the memory bound with large value captures,
        // plus an ordinary-command boundary and a worker-drain boundary. No ROM.
        scheduler.SetReplayBatchingEnabled(true);
        std::vector<unsigned> order;
        bool snapshots_valid = true;
        constexpr unsigned count = 400, marker = 10000;
        for (unsigned i = 0; i < count; ++i) {
            {
                Vulkan::Scheduler::NativeDrawScope scope{scheduler};
                scheduler.Record([&order, i](vk::CommandBuffer) { order.push_back(i); });
            }
            {
                std::array<u64, 512> payload;
                payload.fill(i);
                Vulkan::RenderManager::ReplayScope scope{manager, true};
                scheduler.Record([&order, &snapshots_valid, payload, i](vk::CommandBuffer) {
                    snapshots_valid &= std::all_of(payload.begin(), payload.end(),
                        [i](u64 value) { return value == i; });
                    order.push_back(count + i);
                });
            }
            if (i == 37) {
                scheduler.Record([&order, marker](vk::CommandBuffer) { order.push_back(marker); });
            }
        }
        scheduler.WaitWorker();
        Require(!scheduler.HasReplay(), "Worker wait drains bounded replay chunks");
        Require(snapshots_valid && order.size() == count * 2 + 1,
                "Captured values survive chunk rollover without lost commands");
        unsigned next_native = 0, next_replay = 0;
        for (const auto value : order) {
            if (value == marker) {
                Require(next_native == 38 && next_replay == 38,
                        "Resource boundary follows every earlier deferred command");
            } else if (value < count) {
                Require(value == next_native++, "Native command order preserved");
            } else {
                Require(value == count + next_replay && next_replay < next_native,
                        "Replay follows its native draw and preserves replay order");
                ++next_replay;
            }
        }
        scheduler.Finish();
        scheduler.SetReplayBatchingEnabled(false);
        std::cout << "PASS guarded replay ordering: snapshots, chunk rollover, bounded batches, resource boundary, worker drain\n";
    }
    PromptLayer native(instance, scheduler, manager, true);
    PromptLayer midpoint(instance, scheduler, manager, true);
    const auto vs_code = Vulkan::CompileGLSL(R"(#version 450
layout(push_constant) uniform Params { vec4 rect; vec4 colour_depth; } p;
void main() {
    vec2 corners[3] = vec2[](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(corners[gl_VertexIndex] * p.rect.zw + p.rect.xy,
                       p.colour_depth.w, 1);
})", vk::ShaderStageFlagBits::eVertex);
    const auto fs_code = Vulkan::CompileGLSL(R"(#version 450
layout(push_constant) uniform Params { vec4 rect; vec4 colour_depth; } p;
layout(location=0) out vec4 colour;
void main() { colour = vec4(p.colour_depth.rgb, 1); }
)", vk::ShaderStageFlagBits::eFragment);
    Require(!vs_code.empty() && !fs_code.empty(), "Pass-switch shaders compile");
    auto vs = device.createShaderModuleUnique({.codeSize = vs_code.size() * 4, .pCode = vs_code.data()});
    auto fs = device.createShaderModuleUnique({.codeSize = fs_code.size() * 4, .pCode = fs_code.data()});
    const auto stages_mask = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    const vk::PushConstantRange push{stages_mask, 0, 32};
    auto layout = device.createPipelineLayoutUnique({.pushConstantRangeCount = 1, .pPushConstantRanges = &push});
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex, .module = *vs, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment, .module = *fs, .pName = "main"}};
    const vk::PipelineVertexInputStateCreateInfo input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = vk::PrimitiveTopology::eTriangleList};
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster{.polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone, .frontFace = vk::FrontFace::eCounterClockwise, .lineWidth = 1};
    const vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineDepthStencilStateCreateInfo depth{.depthTestEnable = true, .depthWriteEnable = true,
        .depthCompareOp = vk::CompareOp::eLess};
    const vk::PipelineColorBlendAttachmentState colour{.colorWriteMask = vk::ColorComponentFlagBits::eR |
        vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount = 1, .pAttachments = &colour};
    const std::array dynamic{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dyn{.dynamicStateCount = 2, .pDynamicStates = dynamic.data()};
    auto pipeline = device.createGraphicsPipelineUnique({}, vk::GraphicsPipelineCreateInfo{
        .stageCount = 2, .pStages = stages.data(), .pVertexInputState = &input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = &depth, .pColorBlendState = &blend, .pDynamicState = &dyn,
        .layout = *layout, .renderPass = manager.GetRenderpass(VideoCore::PixelFormat::RGBA8,
            VideoCore::PixelFormat::D24S8, false)}).value;

    const auto bits = physical.getQueueFamilyProperties()[instance.GetGraphicsQueueFamilyIndex()].timestampValidBits;
    const double period = physical.getProperties().limits.timestampPeriod;
    vk::UniqueQueryPool queries;
    if (bits && period > 0) {
        queries = device.createQueryPoolUnique({.queryType = vk::QueryType::eTimestamp, .queryCount = 2});
    }
    bool gpu_timing_enabled = bool(queries);
    constexpr unsigned draws = 192, warmup = 3, samples = 12;
    constexpr std::array modes{"native_only", "alternating", "grouped", "guarded_batching", "guarded_boundaries"};
    std::cout << "Pass-switch benchmark: synthetic independent targets, " << draws
              << " draws per target; timings are not gameplay FPS predictions.\n";
    for (const unsigned scale : {1u, 2u, 4u}) {
        const unsigned width = 512 * scale, height = 400 * scale;
        std::vector<unsigned> execution_order;
        const auto draw = [&](unsigned index, bool interpolated) {
            // Repeated overlapping triangles exercise depth retention across pass
            // switches; each target has a distinct camera offset and colour.
            const float x = -0.9f + float(index % 16) * 0.12f + (interpolated ? 0.025f : 0.f);
            const float y = -0.9f + float((index / 16) % 12) * 0.16f;
            const std::array<float, 8> values{x, y, .11f, .11f,
                float(1 + index % 7) / 8, interpolated ? .75f : .25f,
                float(1 + index % 5) / 6, float(1 + (index * 13) % 29) / 32};
            scheduler.Record([pipeline = *pipeline, layout = *layout, values, width, height, stages_mask,
                              &execution_order, index, interpolated](vk::CommandBuffer cmd) {
                execution_order.push_back(index + (interpolated ? draws : 0));
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
                cmd.setViewport(0, vk::Viewport{0, 0, float(width), float(height), 0, 1});
                cmd.setScissor(0, vk::Rect2D{{0, 0}, {width, height}});
                cmd.pushConstants(layout, stages_mask, 0, sizeof(values), values.data());
                cmd.draw(3, 1, 0, 0);
            });
        };
        std::array<std::vector<u8>, 2> reference;
        const vk::DeviceSize byte_count = vk::DeviceSize(width) * height * 4;
        vk::UniqueDeviceMemory read_memory;
        auto read_buffer = device.createBufferUnique({.size = byte_count, .usage = vk::BufferUsageFlagBits::eTransferDst});
        read_memory = allocate(device.getBufferMemoryRequirements(*read_buffer),
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
        device.bindBufferMemory(*read_buffer, *read_memory, 0);
        const auto read_pixels = [&](PromptLayer& target) {
            scheduler.Record([image = target.Image(), buffer = *read_buffer, width, height](vk::CommandBuffer cmd) {
                const vk::MemoryBarrier barrier{.srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                    .dstAccessMask = vk::AccessFlagBits::eTransferRead};
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                    {}, barrier, {}, {});
                const vk::BufferImageCopy copy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageExtent = {width, height, 1}};
                cmd.copyImageToBuffer(image, vk::ImageLayout::eGeneral, buffer, copy);
            });
            scheduler.Finish();
            const auto* mapped = static_cast<const u8*>(device.mapMemory(*read_memory, 0, byte_count));
            std::vector<u8> pixels(mapped, mapped + byte_count);
            device.unmapMemory(*read_memory);
            return pixels;
        };
        std::array<double, modes.size()> wall_means{}, gpu_means{};
        std::array<bool, modes.size()> gpu_complete{};
        for (unsigned mode = 0; mode < modes.size(); ++mode) {
            scheduler.SetReplayBatchingEnabled(mode >= 3);
            double wall_total = 0, wall_max = 0, gpu_total = 0;
            unsigned gpu_samples = 0;
            u64 pass_total = 0;
            for (unsigned sample = 0; sample < warmup + samples; ++sample) {
                execution_order.clear();
                native.NextFrame();
                midpoint.NextFrame();
                // Allocate/resize before timing, then start a fresh frame. Keep
                // both targets alive for every mode so allocation is comparable.
                if (sample == 0) {
                    Require(native.Begin(width, height), "Native benchmark target allocation");
                    native.End(false);
                    Require(midpoint.Begin(width, height), "Midpoint benchmark target allocation");
                    midpoint.End(false);
                    scheduler.Finish();
                    native.NextFrame();
                    midpoint.NextFrame();
                }
                const auto start = std::chrono::steady_clock::now();
                const auto start_passes = manager.PassesStarted();
                if (gpu_timing_enabled) scheduler.Record([pool = *queries](vk::CommandBuffer cmd) {
                    cmd.resetQueryPool(pool, 0, 2);
                    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pool, 0);
                });
                if (mode >= 3) {
                    for (unsigned i = 0; i < draws; ++i) {
                        {
                            Vulkan::Scheduler::NativeDrawScope native_scope{scheduler};
                            Require(native.Begin(width, height), "Batched native pass");
                            draw(i, false);
                            native.End(false);
                        }
                        {
                            Vulkan::RenderManager::ReplayScope replay_scope{manager, midpoint.CanBatch(width, height)};
                            Require(midpoint.Begin(width, height), "Batched midpoint pass");
                            draw(i, true);
                            midpoint.End(false);
                        }
                        if (mode == 4 && (i + 1) % 24 == 0) {
                            // An ordinary command is a conservative resource
                            // boundary: all preceding replay must execute first.
                            scheduler.Record([&execution_order, i](vk::CommandBuffer) {
                                execution_order.push_back(10000 + i);
                            });
                        }
                    }
                } else if (mode == 1) {
                    for (unsigned i = 0; i < draws; ++i) {
                        Require(native.Begin(width, height), "Alternating native pass");
                        draw(i, false);
                        native.End(false);
                        Require(midpoint.Begin(width, height), "Alternating midpoint pass");
                        draw(i, true);
                        midpoint.End(false);
                    }
                } else {
                    Require(native.Begin(width, height), "Grouped native pass");
                    for (unsigned i = 0; i < draws; ++i) draw(i, false);
                    native.End(false);
                    if (mode == 2) {
                        Require(midpoint.Begin(width, height), "Grouped midpoint pass");
                        for (unsigned i = 0; i < draws; ++i) draw(i, true);
                        midpoint.End(false);
                    }
                }
                if (gpu_timing_enabled) scheduler.Record([pool = *queries](vk::CommandBuffer cmd) {
                    cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, 1);
                });
                scheduler.Finish();
                const double wall_ms = Milliseconds(std::chrono::steady_clock::now() - start);
                Require(!scheduler.HasReplay(), "Submission drains deferred replay");
                if (mode >= 3) {
                    std::vector<unsigned> expected;
                    const unsigned group = mode == 3 ? draws : 24;
                    for (unsigned first = 0; first < draws; first += group) {
                        for (unsigned i = first; i < first + group; ++i) expected.push_back(i);
                        for (unsigned i = first; i < first + group; ++i) expected.push_back(draws + i);
                        if (mode == 4) expected.push_back(10000 + first + group - 1);
                    }
                    Require(execution_order == expected, "Captured draws precede resource boundaries in order");
                }
                double gpu_ms = 0;
                bool gpu_ready = false;
                if (gpu_timing_enabled) {
                    // A host-observed timeline signal need not make MoltenVK's
                    // timestamp readout immediately available. Poll outside wall
                    // timing, with a deadline; never hang or abandon pixel checks.
                    std::array<u64, 4> values{};
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                    do {
                        const auto result = device.getQueryPoolResults(*queries, 0, 2,
                            sizeof(values), values.data(), 2 * sizeof(u64),
                            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
                        Require(result == vk::Result::eSuccess || result == vk::Result::eNotReady,
                                "Unexpected Vulkan timestamp query error");
                        if (result == vk::Result::eSuccess && values[1] && values[3]) {
                            const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
                            gpu_ms = double((values[2] - values[0]) & mask) * period / 1e6;
                            gpu_ready = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } while (std::chrono::steady_clock::now() < deadline);
                    if (!gpu_ready) {
                        // Retain the pool until all work is drained at teardown.
                        // Missing results must not appear as zero-cost GPU work.
                        gpu_timing_enabled = false;
                        std::cout << "NOTE: GPU timestamps unavailable after bounded retry; "
                                     "continuing wall-clock timings and exact image checks.\n";
                    }
                }
                if (sample >= warmup) {
                    wall_total += wall_ms;
                    wall_max = std::max(wall_max, wall_ms);
                    pass_total += manager.PassesStarted() - start_passes;
                    if (gpu_ready) {
                        gpu_total += gpu_ms;
                        ++gpu_samples;
                    }
                }
            }
            auto pixels = read_pixels(native);
            if (mode == 0) {
                reference[0] = std::move(pixels);
                bool has_colour = false;
                for (std::size_t i = 0; i < reference[0].size(); i += 4)
                    has_colour |= reference[0][i] != 0;
                Require(has_colour, "Benchmark native scene contains rendered pixels");
            } else {
                Require(pixels == reference[0], "Native pixels match with alternating/grouped replay");
                pixels = read_pixels(midpoint);
                if (mode == 1) {
                    Require(pixels != reference[0], "Midpoint scene differs from native scene");
                    reference[1] = std::move(pixels);
                } else {
                    Require(pixels == reference[1], "Midpoint pixels match after grouping draws");
                }
            }
            wall_means[mode] = wall_total / samples;
            gpu_complete[mode] = gpu_samples == samples;
            gpu_means[mode] = gpu_samples ? gpu_total / gpu_samples : 0;
            std::cout << "PASS pass-switch: scale=" << scale << " mode=" << modes[mode]
                      << " mean_passes=" << double(pass_total) / samples
                      << " samples=" << samples << " wall_mean_ms=" << wall_means[mode]
                      << " wall_max_ms=" << wall_max << " gpu_samples=" << gpu_samples
                      << " gpu_mean_ms=";
            if (gpu_complete[mode]) std::cout << gpu_means[mode];
            else std::cout << "unavailable";
            std::cout << std::endl;
        }
        std::cout << "Pass-switch comparison: scale=" << scale
                  << " wall_alternating_over_grouped=" << wall_means[1] / wall_means[2]
                  << " gpu_alternating_over_grouped=";
        if (gpu_complete[1] && gpu_complete[2] && gpu_means[2] > 0)
            std::cout << gpu_means[1] / gpu_means[2];
        else
            std::cout << "unavailable";
        std::cout << "; synthetic workload, no live game-resource mutations" << std::endl;
    }
    scheduler.Finish();
    scheduler.WaitWorker();
}
} // namespace MM3D
