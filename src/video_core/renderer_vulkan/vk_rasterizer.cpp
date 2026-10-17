#include "mm3d/effect_capture.h"
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/alignment.h"
#include "common/literals.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/vertex_loader.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture/texture_decode.h"

#ifdef ENABLE_MM3D_PRESENTATION
#include "mm3d/diagnostics.h"
#include "mm3d/cpu_interpolation.h"
#include "mm3d/glare_interpolation.h"
#include "mm3d/ripple_interpolation.h"
#include "mm3d/splash_interpolation.h"
#include "mm3d/input_latency.h"
#include "mm3d/interpolation_performance.h"
#include "mm3d/screen_shrink.h"
#include "mm3d/link_pose_capture.h"
#include "mm3d/bloom_sampling.h"
#include "mm3d/prompt_layer.h"
#include "mm3d/minimap.h"
#include "mm3d/notebook.h"
#include "mm3d/draw_probe.h"
#include "mm3d/texture_picker.h"
#include "mm3d/trace.h"
#include "mm3d/camera_live.h"
#include <chrono>
#include "mm3d/widescreen.h"
#include "mm3d/widescreen_probe.h"
#endif

namespace Vulkan {

namespace {

MICROPROFILE_DEFINE(Vulkan_VS, "Vulkan", "Vertex Shader Setup", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(Vulkan_GS, "Vulkan", "Geometry Shader Setup", MP_RGB(128, 192, 128));
MICROPROFILE_DEFINE(Vulkan_Drawing, "Vulkan", "Drawing", MP_RGB(128, 128, 192));

using TriangleTopology = Pica::PipelineRegs::TriangleTopology;
using VideoCore::SurfaceType;

using namespace Common::Literals;
using namespace Pica::Shader::Generator;

constexpr u64 STREAM_BUFFER_SIZE = 64_MiB;
constexpr u64 UNIFORM_BUFFER_SIZE = 8_MiB;
constexpr u64 TEXTURE_BUFFER_SIZE = 2_MiB;

constexpr vk::BufferUsageFlags BUFFER_USAGE =
    vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer;

struct DrawParams {
    u32 vertex_count;
    s32 vertex_offset;
    u32 binding_count;
    std::array<u32, 16> bindings;
    bool is_indexed;
};

[[nodiscard]] u64 TextureBufferSize(const Instance& instance) {
    // Use the smallest texel size from the texel views
    // which corresponds to eR32G32Sfloat
    const u64 max_size = instance.MaxTexelBufferElements() * 8;
    return std::min(max_size, TEXTURE_BUFFER_SIZE);
}

} // Anonymous namespace

RasterizerVulkan::RasterizerVulkan(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                                   VideoCore::CustomTexManager& custom_tex_manager,
                                   VideoCore::RendererBase& renderer,
                                   Frontend::EmuWindow& emu_window, const Instance& instance,
                                   Scheduler& scheduler, RenderManager& renderpass_cache,
                                   DescriptorUpdateQueue& update_queue_, u32 image_count)
    : RasterizerAccelerated{memory, pica}, instance{instance}, scheduler{scheduler},
      renderpass_cache{renderpass_cache}, update_queue{update_queue_},
      pipeline_cache{instance, scheduler, renderpass_cache, update_queue},
      runtime{instance, scheduler, renderpass_cache, update_queue, image_count},
      res_cache{memory, custom_tex_manager, runtime, regs, renderer},
      stream_buffer{instance, scheduler, BUFFER_USAGE, STREAM_BUFFER_SIZE},
      uniform_buffer{instance, scheduler, vk::BufferUsageFlagBits::eUniformBuffer,
                     UNIFORM_BUFFER_SIZE},
      texture_buffer{instance, scheduler, vk::BufferUsageFlagBits::eUniformTexelBuffer,
                     TextureBufferSize(instance)},
      texture_lf_buffer{instance, scheduler, vk::BufferUsageFlagBits::eUniformTexelBuffer,
                        TextureBufferSize(instance)},
      async_shaders{Settings::values.async_shader_compilation.GetValue()} {

#ifdef ENABLE_MM3D_PRESENTATION
    ResetPause();
    MM3D::PauseCapture::backend.store(this);
    const char* depth_option = std::getenv("DEKUI_ASYNC_VISIBILITY_DEPTH");
    visibility_depth_enabled = !depth_option || std::string_view{depth_option} != "0";
    const char* refresh_option = std::getenv("DEKUI_REFRESH_VISIBILITY_DEPTH");
    visibility_depth_refresh_enabled = !refresh_option || std::string_view{refresh_option} != "0";
    MM3D::VisibilityDepth::backend.store(this);
    LOG_INFO(Render_Vulkan, "Bounded visibility depth snapshots enabled={} background_refresh={}",
             visibility_depth_enabled, visibility_depth_refresh_enabled);
#endif
    vertex_buffers.fill(stream_buffer.Handle());

    // Query uniform buffer alignment.
    uniform_buffer_alignment = instance.UniformMinAlignment();
    uniform_size_aligned_vs_pica =
        Common::AlignUp<u32>(sizeof(VSPicaUniformData), uniform_buffer_alignment);
    uniform_size_aligned_vs = Common::AlignUp<u32>(sizeof(VSUniformData), uniform_buffer_alignment);
    constexpr u32 fs_size=sizeof(FSUniformData)
#ifdef ENABLE_MM3D_PRESENTATION
        +sizeof(MM3D::Atlas::Uniforms)
#endif
        ;
    uniform_size_aligned_fs = Common::AlignUp<u32>(fs_size, uniform_buffer_alignment);

    // Define vertex layout for software shaders
    MakeSoftwareVertexLayout();
    pipeline_info.state.vertex_layout = software_layout;

    const vk::Device device = instance.GetDevice();
    texture_lf_view = device.createBufferViewUnique({
        .buffer = texture_lf_buffer.Handle(),
        .format = vk::Format::eR32G32Sfloat,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    });
    texture_rg_view = device.createBufferViewUnique({
        .buffer = texture_buffer.Handle(),
        .format = vk::Format::eR32G32Sfloat,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    });
    texture_rgba_view = device.createBufferViewUnique({
        .buffer = texture_buffer.Handle(),
        .format = vk::Format::eR32G32B32A32Sfloat,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    });

    scheduler.RegisterOnSubmit([&renderpass_cache] { renderpass_cache.EndRendering(); });

    // Prepare the static buffer descriptor set.
    const auto buffer_set = pipeline_cache.Acquire(DescriptorHeapType::Buffer);
    update_queue.AddBuffer(buffer_set, 0, uniform_buffer.Handle(), 0, sizeof(VSPicaUniformData));
    update_queue.AddBuffer(buffer_set, 1, uniform_buffer.Handle(), 0, sizeof(VSUniformData));
    update_queue.AddBuffer(buffer_set, 2, uniform_buffer.Handle(), 0, fs_size);
    update_queue.AddTexelBuffer(buffer_set, 3, *texture_lf_view);
    update_queue.AddTexelBuffer(buffer_set, 4, *texture_rg_view);
    update_queue.AddTexelBuffer(buffer_set, 5, *texture_rgba_view);

    const auto texture_set = pipeline_cache.Acquire(DescriptorHeapType::Texture);
    Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
    Sampler& null_sampler = res_cache.GetSampler(VideoCore::NULL_SAMPLER_ID);

    // Prepare texture and utility descriptor sets.
    for (u32 i = 0; i < 3; i++) {
        update_queue.AddImageSampler(texture_set, i, 0, null_surface.ImageView(),
                                     null_sampler.Handle());
    }

    const auto utility_set = pipeline_cache.Acquire(DescriptorHeapType::Utility);
    update_queue.AddStorageImage(utility_set, 0, null_surface.StorageView());
    update_queue.AddImageSampler(utility_set, 1, 0, null_surface.ImageView(),
                                 null_sampler.Handle());
    update_queue.Flush();
}

RasterizerVulkan::~RasterizerVulkan() {
#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::PauseCapture::Backend* expected=this;
    MM3D::PauseCapture::backend.compare_exchange_strong(expected,nullptr);
    MM3D::VisibilityDepth::Backend* depth_expected = this;
    MM3D::VisibilityDepth::backend.compare_exchange_strong(depth_expected, nullptr);
    runtime.Finish();
#endif
}

void RasterizerVulkan::TickFrame() {
#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::Widescreen::corrected_draws.store(MM3D::Widescreen::frame_corrected_draws);
    MM3D::Widescreen::frame_corrected_draws=0;
#endif
#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::CameraMidpoint::copied_draws=midpoint_copies;
    MM3D::CameraMidpoint::rejected_draws=midpoint_rejected;
    MM3D::CameraMidpoint::fallback_draws=midpoint_fallback;
    midpoint_copies=midpoint_rejected=midpoint_fallback=0;
    if(!MM3D::CameraMidpoint::enabled.load())midpoint_history.Reset();
    MM3D::Minimap::NextFrame();
    MM3D::Profiles::CompleteDetectionFrame();
    const auto preview_now=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (auto& [id,layer] : prompt_layers) {
        if(id!="@camera_midpoint" && id!="@camera_midpoint_fade" && id!="@camera_midpoint_hud") {
            layer->NextFrame();
        } else if(!MM3D::CameraMidpoint::enabled.load() ||
                  midpoint_generation!=MM3D::CameraMidpoint::generation.load() ||
                  preview_now-midpoint_last_draw_ms>200) {
            layer->NextFrame();
            midpoint_target_frame.reset();
        }
    }
#endif
    scheduler.WaitWorker();
    res_cache.TickFrame();
}

void RasterizerVulkan::LoadDefaultDiskResources(
    const std::atomic_bool& stop_loading, const VideoCore::DiskResourceLoadCallback& callback) {

    u64 program_id;
    if (Core::System::GetInstance().GetAppLoader().ReadProgramId(program_id) !=
        Loader::ResultStatus::Success) {
        program_id = 0;
    }

    if (callback) {
        callback(VideoCore::LoadCallbackStage::Prepare, 0, 0, "");
    }

    pipeline_cache.SetProgramID(program_id);
    pipeline_cache.SetAccurateMul(accurate_mul);
    pipeline_cache.LoadCache(stop_loading, callback);

    if (callback) {
        callback(VideoCore::LoadCallbackStage::Complete, 0, 0, "");
    }
}

void RasterizerVulkan::SyncDrawState() {
    SyncDrawUniforms();

    // SyncCullMode();
    pipeline_info.state.rasterization.cull_mode.Assign(regs.rasterizer.cull_mode);
    // If the framebuffer is flipped, request to also flip vulkan viewport
    const bool is_flipped = regs.framebuffer.framebuffer.IsFlipped();
    pipeline_info.state.rasterization.flip_viewport.Assign(is_flipped);
    // SyncBlendEnabled();
    pipeline_info.state.blending.blend_enable = regs.framebuffer.output_merger.alphablend_enable;
    // SyncBlendFuncs();
    pipeline_info.state.blending.color_blend_eq.Assign(
        regs.framebuffer.output_merger.alpha_blending.blend_equation_rgb);
    pipeline_info.state.blending.alpha_blend_eq.Assign(
        regs.framebuffer.output_merger.alpha_blending.blend_equation_a);
    pipeline_info.state.blending.src_color_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_source_rgb);
    pipeline_info.state.blending.dst_color_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_dest_rgb);
    pipeline_info.state.blending.src_alpha_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_source_a);
    pipeline_info.state.blending.dst_alpha_blend_factor.Assign(
        regs.framebuffer.output_merger.alpha_blending.factor_dest_a);
    // SyncBlendColor();
    pipeline_info.dynamic_info.blend_color = regs.framebuffer.output_merger.blend_const.raw;
    // SyncLogicOp();
    // SyncColorWriteMask();
    pipeline_info.state.blending.logic_op = regs.framebuffer.output_merger.logic_op;

    const u32 color_mask = regs.framebuffer.framebuffer.allow_color_write != 0
                               ? (regs.framebuffer.output_merger.depth_color_mask >> 8) & 0xF
                               : 0;
    pipeline_info.state.blending.color_write_mask = color_mask;

    // SyncStencilTest();
    const auto& stencil_test = regs.framebuffer.output_merger.stencil_test;
    const bool test_enable = stencil_test.enable && regs.framebuffer.framebuffer.depth_format ==
                                                        Pica::FramebufferRegs::DepthFormat::D24S8;

    pipeline_info.state.depth_stencil.stencil_test_enable.Assign(test_enable);
    pipeline_info.state.depth_stencil.stencil_fail_op.Assign(stencil_test.action_stencil_fail);
    pipeline_info.state.depth_stencil.stencil_pass_op.Assign(stencil_test.action_depth_pass);
    pipeline_info.state.depth_stencil.stencil_depth_fail_op.Assign(stencil_test.action_depth_fail);
    pipeline_info.state.depth_stencil.stencil_compare_op.Assign(stencil_test.func);
    pipeline_info.dynamic_info.stencil_reference = stencil_test.reference_value;
    pipeline_info.dynamic_info.stencil_compare_mask = stencil_test.input_mask;
    // SyncStencilWriteMask();
    pipeline_info.dynamic_info.stencil_write_mask =
        (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0)
            ? static_cast<u32>(regs.framebuffer.output_merger.stencil_test.write_mask)
            : 0;
    // SyncDepthTest();
    const bool test_enabled = regs.framebuffer.output_merger.depth_test_enable == 1 ||
                              regs.framebuffer.output_merger.depth_write_enable == 1;
    const auto compare_op = regs.framebuffer.output_merger.depth_test_enable == 1
                                ? regs.framebuffer.output_merger.depth_test_func.Value()
                                : Pica::FramebufferRegs::CompareFunc::Always;

    pipeline_info.state.depth_stencil.depth_test_enable.Assign(test_enabled);
    pipeline_info.state.depth_stencil.depth_compare_op.Assign(compare_op);
    // SyncDepthWriteMask();
    const bool write_enable = (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0 &&
                               regs.framebuffer.output_merger.depth_write_enable);
    pipeline_info.state.depth_stencil.depth_write_enable.Assign(write_enable);
}

void RasterizerVulkan::SetupVertexArray() {
    const auto [vs_input_index_min, vs_input_index_max, vs_input_size] = vertex_info;
    auto [array_ptr, array_offset, invalidate] = stream_buffer.Map(vs_input_size, 16);

    /**
     * The Nintendo 3DS has 12 attribute loaders which are used to tell the GPU
     * how to interpret vertex data. The program firsts sets GPUREG_ATTR_BUF_BASE to the base
     * address containing the vertex array data. The data for each attribute loader (i) can be found
     * by adding GPUREG_ATTR_BUFi_OFFSET to the base address. Attribute loaders can be thought
     * as something analogous to Vulkan bindings. The user can store attributes in separate loaders
     * or interleave them in the same loader.
     **/
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    const PAddr base_address = vertex_attributes.GetPhysicalBaseAddress(); // GPUREG_ATTR_BUF_BASE
    const u32 stride_alignment = instance.GetMinVertexStrideAlignment();

    VertexLayout& layout = pipeline_info.state.vertex_layout;
    layout.binding_count = 0;
    layout.attribute_count = 16;
    enable_attributes.fill(false);

    u32 buffer_offset = 0;
    for (const auto& loader : vertex_attributes.attribute_loaders) {
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }

        // Analyze the attribute loader by checking which attributes it provides
        u32 offset = 0;
        for (u32 comp = 0; comp < loader.component_count && comp < 12; comp++) {
            const u32 attribute_index = loader.GetComponent(comp);
            if (attribute_index >= 12) {
                // Attribute ids 12, to 15 signify 4, 8, 12 and 16-byte paddings respectively.
                offset = Common::AlignUp(offset, 4);
                offset += (attribute_index - 11) * 4;
                continue;
            }

            const u32 size = vertex_attributes.GetNumElements(attribute_index);
            if (size == 0) {
                continue;
            }

            offset =
                Common::AlignUp(offset, vertex_attributes.GetElementSizeInBytes(attribute_index));

            const u32 input_reg = regs.vs.GetRegisterForAttribute(attribute_index);
            const auto format = vertex_attributes.GetFormat(attribute_index);

            VertexAttribute& attribute = layout.attributes[input_reg];
            attribute.binding.Assign(layout.binding_count);
            attribute.location.Assign(input_reg);
            attribute.offset.Assign(offset);
            attribute.type.Assign(format);
            attribute.size.Assign(size);

            enable_attributes[input_reg] = true;
            offset += vertex_attributes.GetStride(attribute_index);
        }

        const PAddr data_addr =
            base_address + loader.data_offset + (vs_input_index_min * loader.byte_count);
        const u32 vertex_num = vs_input_index_max - vs_input_index_min + 1;
        u32 data_size = loader.byte_count * vertex_num;
#ifdef ENABLE_MM3D_PRESENTATION
        MM3D::InterpolationPerformance::ReadbackScope readback{"vertex data",data_addr,data_size};
#endif
        res_cache.FlushRegion(data_addr, data_size);

        const MemoryRef src_ref = memory.GetPhysicalRef(data_addr);
        if (src_ref.GetSize() < data_size) {
            LOG_ERROR(Render_Vulkan,
                      "Vertex buffer size {} exceeds available space {} at address {:#016X}",
                      data_size, src_ref.GetSize(), data_addr);
        }

        const u8* src_ptr = src_ref.GetPtr();
        u8* dst_ptr = array_ptr + buffer_offset;

        // Align stride up if required by Vulkan implementation.
        const u32 aligned_stride =
            Common::AlignUp(static_cast<u32>(loader.byte_count), stride_alignment);
        if (aligned_stride == loader.byte_count) {
            std::memcpy(dst_ptr, src_ptr, data_size);
        } else {
            for (std::size_t vertex = 0; vertex < vertex_num; vertex++) {
                std::memcpy(dst_ptr + vertex * aligned_stride, src_ptr + vertex * loader.byte_count,
                            loader.byte_count);
            }
        }

        // Create the binding associated with this loader
        VertexBinding& binding = layout.bindings[layout.binding_count];
        binding.binding.Assign(layout.binding_count);
        binding.fixed.Assign(0);
        // Will be adjusted on pipeline build, to keep the info transferable.
        binding.byte_count.Assign(loader.byte_count);

        // Keep track of the binding offsets so we can bind the vertex buffer later
        binding_offsets[layout.binding_count++] = static_cast<u32>(array_offset + buffer_offset);
        buffer_offset += Common::AlignUp(aligned_stride * vertex_num, 4);
    }

    stream_buffer.Commit(buffer_offset);

#ifdef ENABLE_MM3D_PRESENTATION
    glare_vertex_offset.reset();
    const auto& merger=regs.framebuffer.output_merger;
    const auto& loader=vertex_attributes.attribute_loaders[0];
    const auto& uv=vertex_attributes.attribute_loaders[1];
    if(MM3D::CameraMidpoint::present_60.load() &&
       pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
       pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL &&
       regs.pipeline.num_vertices==72 && vs_input_index_min==0 && vs_input_index_max==48 &&
       loader.component_count==1 && loader.GetComponent(0)==0 && loader.byte_count==12 &&
       uv.component_count==1 && uv.GetComponent(0)==1 && uv.byte_count==8 &&
       vertex_attributes.GetFormat(0)==Pica::PipelineRegs::VertexAttributeFormat::FLOAT &&
       vertex_attributes.GetNumElements(0)==3 && regs.vs.GetRegisterForAttribute(0)==0 &&
       merger.alphablend_enable && merger.depth_color_mask==3856 &&
       merger.alpha_blending.factor_source_rgb==Pica::FramebufferRegs::BlendFactor::SourceAlpha &&
       merger.alpha_blending.factor_dest_rgb==Pica::FramebufferRegs::BlendFactor::One &&
       merger.alpha_blending.blend_equation_rgb==Pica::FramebufferRegs::BlendEquation::Add &&
       pica.vs_setup.uniforms.f[3].w.ToFloat32()==1.f) {
        const u64 address=u64(base_address)+loader.data_offset;
        if(address<=0xffffffff) {
            const auto src=memory.GetPhysicalRef(static_cast<PAddr>(address));
            MM3D::GlareInterpolation::Points points;
            if(src.GetSize()>=sizeof(points)) {
                std::memcpy(points.data(),src.GetPtr(),sizeof(points));
                if(auto blended=glare_history.Apply(MM3D::guest_frames.load(),
                    regs.texturing.texture0.GetPhysicalAddress(),MM3D::CameraMidpoint::generation.load(),points)) {
                    const auto stride=Common::AlignUp(12u,stride_alignment);
                    auto [dst,offset,unused]=stream_buffer.Map(stride*49,16);
                    for(unsigned i=0;i<49;++i)std::memcpy(dst+i*stride,(*blended)[i].data(),12);
                    stream_buffer.Commit(stride*49);
                    glare_vertex_offset=static_cast<u32>(offset);
                }
            }
        }
    }
    ripple_offsets.reset();
    const auto& color_loader=vertex_attributes.attribute_loaders[1];
    const auto& tex_loader=vertex_attributes.attribute_loaders[2];
    const unsigned ripple_count=vs_input_index_max+1;
    if(MM3D::CameraMidpoint::present_60.load() &&
       pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
       pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL &&
       vs_input_index_min==0 && ripple_count>=9 && ripple_count<=257 && (ripple_count-1)%8==0 &&
       regs.pipeline.num_vertices==(ripple_count-1)/8*12 &&
       loader.component_count==1 && loader.GetComponent(0)==0 && loader.byte_count==12 &&
       color_loader.component_count==1 && color_loader.GetComponent(0)==1 && color_loader.byte_count==16 &&
       tex_loader.component_count==1 && tex_loader.GetComponent(0)==2 && tex_loader.byte_count==8 &&
       vertex_attributes.GetFormat(0)==Pica::PipelineRegs::VertexAttributeFormat::FLOAT && vertex_attributes.GetNumElements(0)==3 &&
       vertex_attributes.GetFormat(1)==Pica::PipelineRegs::VertexAttributeFormat::FLOAT && vertex_attributes.GetNumElements(1)==4 &&
       vertex_attributes.GetFormat(2)==Pica::PipelineRegs::VertexAttributeFormat::FLOAT && vertex_attributes.GetNumElements(2)==2 &&
       regs.vs.GetRegisterForAttribute(0)==0 && regs.vs.GetRegisterForAttribute(1)==3 && regs.vs.GetRegisterForAttribute(2)==4 &&
       merger.alphablend_enable && merger.alpha_blending.factor_source_rgb==Pica::FramebufferRegs::BlendFactor::SourceAlpha &&
       merger.alpha_blending.factor_dest_rgb==Pica::FramebufferRegs::BlendFactor::One &&
       merger.alpha_blending.blend_equation_rgb==Pica::FramebufferRegs::BlendEquation::Add) {
        const u64 pa=u64(base_address)+loader.data_offset,ca=u64(base_address)+color_loader.data_offset,
                  ta=u64(base_address)+tex_loader.data_offset;
        if(pa<=0xffffffff && ca<=0xffffffff && ta<=0xffffffff) {
            const auto positions=memory.GetPhysicalRef(PAddr(pa)),colors=memory.GetPhysicalRef(PAddr(ca)),coords=memory.GetPhysicalRef(PAddr(ta));
            if(positions.GetSize()>=ripple_count*12 && colors.GetSize()>=ripple_count*16 && coords.GetSize()>=ripple_count*8) {
                bool valid=true;
                for(unsigned i=0;i+1<ripple_count;++i) {
                    std::array<float,2> uv_value;std::memcpy(uv_value.data(),coords.GetPtr()+i*8,8);
                    valid &= uv_value[0]==(i%2?0.f:1.f) && uv_value[1]==(i%4<2?0.f:1.f);
                }
                if(valid) {
                    MM3D::RippleInterpolation::Mesh mesh;mesh.points.resize(ripple_count);mesh.colors.resize(ripple_count);
                    std::memcpy(mesh.points.data(),positions.GetPtr(),ripple_count*12);
                    std::memcpy(mesh.colors.data(),colors.GetPtr(),ripple_count*16);
                    if(auto half=ripple_history.Apply(MM3D::guest_frames.load(),regs.texturing.texture0.GetPhysicalAddress(),
                                                     MM3D::CameraMidpoint::generation.load(),mesh)) {
                        std::array<u32,2> offsets{};
                        for(unsigned part=0;part<2;++part) {
                            const unsigned size=part?16:12,stride=Common::AlignUp(size,stride_alignment);
                            auto [dst,offset,unused]=stream_buffer.Map(stride*ripple_count,16);
                            for(unsigned i=0;i<ripple_count;++i)std::memcpy(dst+i*stride,
                                part?static_cast<const void*>(half->colors[i].data()):static_cast<const void*>(half->points[i].data()),size);
                            stream_buffer.Commit(stride*ripple_count);offsets[part]=u32(offset);
                        }
                        ripple_offsets=offsets;
                    }
                }
            }
        }
    }
    splash_offsets.reset();splash_blend=false;
    if(MM3D::CameraMidpoint::present_60.load() &&
       pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
       pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL &&
       regs.pipeline.num_vertices==6 && vs_input_index_min==0 && vs_input_index_max==4 &&
       loader.component_count==1 && loader.GetComponent(0)==0 && loader.byte_count==12 &&
       color_loader.component_count==1 && color_loader.GetComponent(0)==1 && color_loader.byte_count==16 &&
       tex_loader.component_count==1 && tex_loader.GetComponent(0)==2 && tex_loader.byte_count==8 &&
       vertex_attributes.GetFormat(0)==Pica::PipelineRegs::VertexAttributeFormat::FLOAT && vertex_attributes.GetNumElements(0)==3 &&
       vertex_attributes.GetFormat(1)==Pica::PipelineRegs::VertexAttributeFormat::FLOAT && vertex_attributes.GetNumElements(1)==4 &&
       vertex_attributes.GetFormat(2)==Pica::PipelineRegs::VertexAttributeFormat::FLOAT && vertex_attributes.GetNumElements(2)==2 &&
       regs.vs.GetRegisterForAttribute(0)==0 && regs.vs.GetRegisterForAttribute(1)==3 && regs.vs.GetRegisterForAttribute(2)==4 &&
       merger.alphablend_enable && merger.alpha_blending.factor_source_rgb==Pica::FramebufferRegs::BlendFactor::SourceAlpha &&
       merger.alpha_blending.factor_dest_rgb==Pica::FramebufferRegs::BlendFactor::One &&
       merger.alpha_blending.blend_equation_rgb==Pica::FramebufferRegs::BlendEquation::Add) {
        const u64 pa=u64(base_address)+loader.data_offset,ca=u64(base_address)+color_loader.data_offset,ta=u64(base_address)+tex_loader.data_offset;
        if(pa<=0xffffffff && ca<=0xffffffff && ta<=0xffffffff) {
            const auto p=memory.GetPhysicalRef(PAddr(pa)),c=memory.GetPhysicalRef(PAddr(ca)),t=memory.GetPhysicalRef(PAddr(ta));
            if(p.GetSize()>=60 && c.GetSize()>=80 && t.GetSize()>=40) {
                MM3D::SplashInterpolation::Sprite sprite;
                std::memcpy(sprite.points.data(),p.GetPtr(),60);std::memcpy(sprite.colors.data(),c.GetPtr(),80);std::memcpy(sprite.uv.data(),t.GetPtr(),40);
                if(auto mid=splash_history.Apply(MM3D::guest_frames.load(),regs.texturing.texture0.GetPhysicalAddress(),MM3D::CameraMidpoint::generation.load(),sprite)) {
                    std::array<u32,3> offsets;
                    for(unsigned part=0;part<3;++part) {
                        const unsigned size=part==0?12:part==1?16:8,stride=Common::AlignUp(size,stride_alignment);
                        auto [dst,offset,unused]=stream_buffer.Map(stride*5,16);
                        for(unsigned i=0;i<5;++i)std::memcpy(dst+i*stride,part==0?static_cast<const void*>(mid->sprite.points[i].data()):
                            part==1?static_cast<const void*>(mid->sprite.colors[i].data()):static_cast<const void*>(mid->old_uv[i].data()),size);
                        stream_buffer.Commit(stride*5);offsets[part]=u32(offset);
                    }
                    splash_offsets=offsets;splash_blend=mid->blend;
                }
            }
        }
    }
#endif
    // Assign the rest of the attributes to the last binding
    SetupFixedAttribs();
}

void RasterizerVulkan::SetupFixedAttribs() {
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    VertexLayout& layout = pipeline_info.state.vertex_layout;

    auto [fixed_ptr, fixed_offset, _] = stream_buffer.Map(16 * sizeof(Common::Vec4f), 0);
    binding_offsets[layout.binding_count] = static_cast<u32>(fixed_offset);

    // Reserve the last binding for fixed and default attributes
    // Place the default attrib at offset zero for easy access
    static const Common::Vec4f default_attrib{0.f, 0.f, 0.f, 1.f};
    std::memcpy(fixed_ptr, default_attrib.AsArray(), sizeof(Common::Vec4f));

    // Find all fixed attributes and assign them to the last binding
    u32 offset = sizeof(Common::Vec4f);
    for (std::size_t i = 0; i < 16; i++) {
        if (vertex_attributes.IsDefaultAttribute(i)) {
            const u32 reg = regs.vs.GetRegisterForAttribute(i);
            if (!enable_attributes[reg]) {
                const auto& attr = pica.input_default_attributes[i];
                const std::array data = {attr.x.ToFloat32(), attr.y.ToFloat32(), attr.z.ToFloat32(),
                                         attr.w.ToFloat32()};

                const u32 data_size = sizeof(float) * static_cast<u32>(data.size());
                std::memcpy(fixed_ptr + offset, data.data(), data_size);

                VertexAttribute& attribute = layout.attributes[reg];
                attribute.binding.Assign(layout.binding_count);
                attribute.location.Assign(reg);
                attribute.offset.Assign(offset);
                attribute.type.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
                attribute.size.Assign(4);

                offset += data_size;
                enable_attributes[reg] = true;
            }
        }
    }

    // Loop one more time to find unused attributes and assign them to the default one
    // If the attribute is just disabled, shove the default attribute to avoid
    // errors if the shader ever decides to use it.
    for (u32 i = 0; i < 16; i++) {
        if (!enable_attributes[i]) {
            VertexAttribute& attribute = layout.attributes[i];
            attribute.binding.Assign(layout.binding_count);
            attribute.location.Assign(i);
            attribute.offset.Assign(0);
            attribute.type.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
            attribute.size.Assign(4);
        }
    }

    // Define the fixed+default binding
    VertexBinding& binding = layout.bindings[layout.binding_count];
    binding.binding.Assign(layout.binding_count++);
    binding.fixed.Assign(1);
    binding.byte_count.Assign(offset);

    stream_buffer.Commit(offset);
}

bool RasterizerVulkan::SetupVertexShader() {
    MICROPROFILE_SCOPE(Vulkan_VS);
    return pipeline_cache.UseProgrammableVertexShader(regs, pica.vs_setup,
                                                      pipeline_info.state.vertex_layout);
}

bool RasterizerVulkan::SetupGeometryShader() {
    MICROPROFILE_SCOPE(Vulkan_GS);

    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) {
        LOG_ERROR(Render_Vulkan, "Accelerate draw doesn't support geometry shader");
        return false;
    }

    // Enable the quaternion fix-up geometry-shader only if we are actually doing per-fragment
    // lighting and care about proper quaternions. Otherwise just use standard vertex+fragment
    // shaders. We also don't need a geometry shader if the barycentric extension is supported,
    // but that will be decided later as the GS config needs to be cached anyways.
    if (regs.lighting.disable) {
        pipeline_cache.UseTrivialGeometryShader();
        return true;
    }

    return pipeline_cache.UseFixedGeometryShader(regs);
}

bool RasterizerVulkan::AccelerateDrawBatch(bool is_indexed) {
    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) {
        if (regs.pipeline.gs_config.mode != Pica::PipelineRegs::GSMode::Point) {
            return false;
        }
        if (regs.pipeline.triangle_topology != Pica::PipelineRegs::TriangleTopology::Shader) {
            return false;
        }
    }

    pipeline_info.state.rasterization.topology.Assign(regs.pipeline.triangle_topology);
    if (regs.pipeline.triangle_topology == TriangleTopology::Fan &&
        !instance.IsTriangleFanSupported()) {
        LOG_DEBUG(Render_Vulkan,
                  "Skipping accelerated draw with unsupported triangle fan topology");
        return false;
    }

    // Vertex data setup might involve scheduler flushes so perform it
    // early to avoid invalidating our state in the middle of the draw.
    vertex_info = AnalyzeVertexArray(is_indexed, instance.GetMinVertexStrideAlignment());
    if (vertex_info.Invalid()) {
        // Do not draw anything if the vertex array is invalid.
        return true;
    }
    SetupVertexArray();

    if (!SetupVertexShader()) {
        return false;
    }
    if (!SetupGeometryShader()) {
        return false;
    }

    return Draw(true, is_indexed);
}

bool RasterizerVulkan::AccelerateDrawBatchInternal(bool is_indexed) {
    if (is_indexed && !scheduler.ReplayBatchingEnabled()) {
        SetupIndexArray();
    }

    const bool wait_built = !async_shaders || regs.pipeline.num_vertices <= 6;
    if (!pipeline_cache.BindPipeline(pipeline_info, wait_built)) {
        return true;
    }

    const DrawParams params = {
        .vertex_count = regs.pipeline.num_vertices,
        .vertex_offset = -static_cast<s32>(vertex_info.vs_input_index_min),
        .binding_count = pipeline_info.state.vertex_layout.binding_count,
        .bindings = binding_offsets,
        .is_indexed = is_indexed,
    };

    scheduler.Record([params, buffers = vertex_buffers, index_buffer = stream_buffer.Handle(),
                      index_offset = replay_index_offset, index_type = replay_index_type](vk::CommandBuffer cmdbuf) {
        std::array<vk::DeviceSize, 16> offsets;
        std::transform(params.bindings.begin(), params.bindings.end(), offsets.begin(),
                       [](u32 offset) { return static_cast<vk::DeviceSize>(offset); });
        cmdbuf.bindVertexBuffers(0, params.binding_count, buffers.data(), offsets.data());
        if (params.is_indexed) {
            cmdbuf.bindIndexBuffer(index_buffer, index_offset, index_type);
            cmdbuf.drawIndexed(params.vertex_count, 1, 0, params.vertex_offset, 0);
        } else {
            cmdbuf.draw(params.vertex_count, 1, 0, 0);
        }
    });

    return true;
}

void RasterizerVulkan::SetupIndexArray() {
    const bool index_u8 = regs.pipeline.index_array.format == 0;
    const bool native_u8 = index_u8 && instance.IsIndexTypeUint8Supported();
    const u32 index_buffer_size = regs.pipeline.num_vertices * (native_u8 ? 1 : 2);
    const vk::IndexType index_type = native_u8 ? vk::IndexType::eUint8EXT : vk::IndexType::eUint16;

    const u8* index_data =
        memory.GetPhysicalPointer(regs.pipeline.vertex_attributes.GetPhysicalBaseAddress() +
                                  regs.pipeline.index_array.offset);

    auto [index_ptr, index_offset, _] = stream_buffer.Map(index_buffer_size, 2);

    if (index_u8 && !native_u8) {
        u16* index_ptr_u16 = reinterpret_cast<u16*>(index_ptr);
        for (u32 i = 0; i < regs.pipeline.num_vertices; i++) {
            index_ptr_u16[i] = index_data[i];
        }
    } else {
        std::memcpy(index_ptr, index_data, index_buffer_size);
    }

    stream_buffer.Commit(index_buffer_size);
    replay_index_offset = index_offset;
    replay_index_type = index_type;

    // AccelerateDrawBatchInternal binds the captured index range with its draw.
}

void RasterizerVulkan::DrawTriangles() {
    if (vertex_batch.empty()) {
        return;
    }

    pipeline_info.state.rasterization.topology.Assign(Pica::PipelineRegs::TriangleTopology::List);
    pipeline_info.state.vertex_layout = software_layout;

    pipeline_cache.UseTrivialVertexShader();
    pipeline_cache.UseTrivialGeometryShader();

    Draw(false, false);
}

bool RasterizerVulkan::Draw(bool accelerate, bool is_indexed) {
    MICROPROFILE_SCOPE(Vulkan_Drawing);
    bool cpu_interpolation = false;
#ifdef ENABLE_MM3D_PRESENTATION
    cpu_interpolation = !accelerate && MM3D::CpuInterpolation::Enabled() &&
                        MM3D::CameraMidpoint::enabled.load();
    const bool replay_geometry = accelerate || cpu_interpolation;
    const bool replay_world = accelerate || (cpu_interpolation && pica.CanReplayCpuVertices());
    MM3D::InterpolationPerformance::Scope performance_draw{MM3D::InterpolationPerformance::Part::Draw};
    MM3D::Profiles::ClearDrawTexcoordBounds();
#endif
    SyncDrawState();

    const bool shadow_rendering = regs.framebuffer.IsShadowRendering();
    const bool has_stencil = regs.framebuffer.HasStencil();

    const bool write_color_fb = shadow_rendering || pipeline_info.GetFinalColorWriteMask(instance);
    const bool write_depth_fb = pipeline_info.IsDepthWriteEnabled();
    const bool using_color_fb =
        regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress() != 0 && write_color_fb;
    const bool using_depth_fb =
        !shadow_rendering && regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress() != 0 &&
        (write_depth_fb || regs.framebuffer.output_merger.depth_test_enable != 0 ||
         (has_stencil && pipeline_info.state.depth_stencil.stencil_test_enable));

    const auto fb_helper = res_cache.GetFramebufferSurfaces(using_color_fb, using_depth_fb);
    const Framebuffer* framebuffer = fb_helper.Framebuffer();
    if (!framebuffer->Handle()) {
        return true;
    }

    pipeline_info.state.attachments.color = framebuffer->Format(SurfaceType::Color);
    pipeline_info.state.attachments.depth = framebuffer->Format(SurfaceType::Depth);

    // Update scissor uniforms
    const auto [scissor_x1, scissor_y2, scissor_x2, scissor_y1] = fb_helper.Scissor();
    if (fs_data.scissor_x1 != scissor_x1 || fs_data.scissor_x2 != scissor_x2 ||
        fs_data.scissor_y1 != scissor_y1 || fs_data.scissor_y2 != scissor_y2) {

        fs_data.scissor_x1 = scissor_x1;
        fs_data.scissor_x2 = scissor_x2;
        fs_data.scissor_y1 = scissor_y1;
        fs_data.scissor_y2 = scissor_y2;
        fs_data_dirty = true;
    }

    // Sync and bind the texture surfaces
#ifdef ENABLE_MM3D_PRESENTATION
    // MM3D uses a 480x400 top viewport (the display itself is 240x400). Compress its display-X
    // clip coordinate before clipping: perspective draws reveal more world,
    // while orthographic UI recovers its proportions in the 16:9 compositor.
    const auto wide_viewport=regs.rasterizer.GetViewportRect();
    const bool wide_top=MM3D::Widescreen::Active() && !shadow_rendering && using_color_fb &&
        MM3D::Widescreen::IsTopViewport(wide_viewport.GetWidth(),wide_viewport.GetHeight());
    bool wide_bypass=false;
    if(wide_top && regs.pipeline.num_vertices<=24 && !write_depth_fb) {
        const auto& textures=regs.texturing.main_config;
        const auto& merger=regs.framebuffer.output_merger;
        const auto scissor=fb_helper.Scissor();
        wide_bypass=MM3D::WidescreenProbe::Observe({pica.vs_setup.GetProgramCodeHash(),
            pica.vs_setup.GetSwizzleDataHash(),u32(regs.pipeline.num_vertices),
            unsigned(wide_viewport.GetWidth()),unsigned(wide_viewport.GetHeight()),
            unsigned(textures.texture0_enable)|(unsigned(textures.texture1_enable)<<1)|(unsigned(textures.texture2_enable)<<2),
            unsigned(merger.alpha_blending.factor_source_rgb.Value())|
                (unsigned(merger.alpha_blending.factor_dest_rgb.Value())<<4)|
                (unsigned(merger.alpha_blending.blend_equation_rgb.Value())<<8)|
                (unsigned(merger.alphablend_enable)<<16),u32(merger.depth_color_mask)},
            {int(scissor.left),int(scissor.top),int(scissor.right),int(scissor.bottom)});
        if(wide_bypass) {
            const auto row=[&](unsigned r) {
                std::array<float,4> result{};
                for(unsigned c=0;c<4;++c)result[c]=pica.vs_setup.uniforms.f[r][c].ToFloat32();
                return result;
            };
            if(MM3D::WidescreenProbe::LocalLayoutPane(pica.vs_setup.GetProgramCodeHash(),
                pica.vs_setup.GetSwizzleDataHash(),u32(regs.pipeline.num_vertices),
                unsigned(textures.texture0_enable)|(unsigned(textures.texture1_enable)<<1)|
                    (unsigned(textures.texture2_enable)<<2),row(10),row(11),row(12)))
                wide_bypass=false;
        }
    }
    const float wide_scale=wide_top && !wide_bypass?MM3D::Widescreen::HorizontalScale:1.f;
    // Depth testing is a provisional world-draw filter, not a verified camera identity.
    // Keep zoom independent of the widescreen checkbox and exclude shadow/offscreen layouts.
    const auto& depth = regs.framebuffer.output_merger;
    const bool zoom_world = MM3D::Widescreen::supported_layout.load() &&
        !shadow_rendering && using_color_fb && !wide_bypass &&
        MM3D::Widescreen::IsTopViewport(wide_viewport.GetWidth(), wide_viewport.GetHeight()) &&
        depth.depth_test_enable != 0 &&
        depth.depth_test_func != Pica::FramebufferRegs::CompareFunc::Always &&
        depth.depth_test_func != Pica::FramebufferRegs::CompareFunc::Never;
    const float zoom_scale = zoom_world ?
        1.f / std::clamp(MM3D::Widescreen::camera_zoom_out.load(), 1.f, 1.5f) : 1.f;
    if(vs_data.clip_scale_x != zoom_scale || vs_data.clip_scale_y != wide_scale * zoom_scale) {
        vs_data.clip_scale_x = zoom_scale;
        vs_data.clip_scale_y = wide_scale * zoom_scale;
        vs_data_dirty = true;
    }
    if(wide_top) ++MM3D::Widescreen::frame_corrected_draws;
    MM3D::Textures::hide_current_draw = false;
    mm3d_draw_profiles.clear();
    panel_excluded = false;
    panel_draw_identities.clear();
    if (accelerate) {
        Pica::VertexLoader loader(memory, regs.pipeline);
        Pica::AttributeBuffer input{}, defaults{};
        float min_x=1.f,min_y=1.f,max_x=0.f,max_y=0.f;
        const u32 attr_reg=regs.vs.GetRegisterForAttribute(1); // conventional texture0 input
        for (u32 i=vertex_info.vs_input_index_min; i<=vertex_info.vs_input_index_max; ++i) {
            loader.LoadVertex(regs.pipeline.vertex_attributes.GetPhysicalBaseAddress(), i, i, input, defaults);
            min_x=std::min(min_x,input[attr_reg][0].ToFloat32()); min_y=std::min(min_y,input[attr_reg][1].ToFloat32());
            max_x=std::max(max_x,input[attr_reg][0].ToFloat32()); max_y=std::max(max_y,input[attr_reg][1].ToFloat32());
        }
        if (max_x>=min_x && max_y>=min_y) MM3D::Profiles::SetDrawTexcoordBounds(min_x,min_y,max_x,max_y);
    } else if (!vertex_batch.empty()) {
        float min_x=1.f,min_y=1.f,max_x=0.f,max_y=0.f;
        for (const auto& v:vertex_batch) { min_x=std::min(min_x,v.tex_coord0.x); min_y=std::min(min_y,v.tex_coord0.y); max_x=std::max(max_x,v.tex_coord0.x); max_y=std::max(max_y,v.tex_coord0.y); }
        MM3D::Profiles::SetDrawTexcoordBounds(min_x,min_y,max_x,max_y);
    }
    MM3D::Minimap::marker_draw=false;
#endif
    SyncTextureUnits(framebuffer);
    SyncUtilityTextures(framebuffer);
#ifdef ENABLE_MM3D_PRESENTATION
    using AtlasBF=Pica::FramebufferRegs::BlendFactor;
    const auto& atlas_blend=pipeline_info.state.blending;
    const bool atlas_supported=!shadow_rendering &&
        !pipeline_info.state.depth_stencil.stencil_test_enable && !write_depth_fb &&
        (!pipeline_info.state.depth_stencil.depth_test_enable ||
         pipeline_info.state.depth_stencil.depth_compare_op==Pica::FramebufferRegs::CompareFunc::Always) &&
        atlas_blend.blend_enable && atlas_blend.src_color_blend_factor==AtlasBF::SourceAlpha &&
        atlas_blend.dst_color_blend_factor==AtlasBF::OneMinusSourceAlpha &&
        atlas_blend.color_blend_eq==Pica::FramebufferRegs::BlendEquation::Add &&
        (atlas_blend.color_write_mask&7)==7 && regs.lighting.disable.Value()!=0 &&
        regs.texturing.texture0.type==Pica::TexturingRegs::TextureConfig::Texture2D;
    std::erase_if(mm3d_draw_profiles,[&](const auto& p){
        if(p.atlas_selection && !atlas_supported) { ++MM3D::prompt_unsupported; return true; }
        return false;
    });
    MM3D::Atlas::Uniforms guest_regions;
    // Overlapping guest groups use the first match, so the same pixels are never drawn twice.
    std::erase_if(mm3d_draw_profiles,[&](const auto& p){
        if(!p.atlas_selection || p.presentation) return false;
        MM3D::Atlas::Uniforms mask;
        if(!MM3D::Atlas::Build(p,mask) || MM3D::Atlas::Overlaps(guest_regions,mask)) {++MM3D::prompt_unsupported;return true;}
        auto combined=guest_regions;
        for(int i=0;i<mask.info[0];++i) if(!MM3D::Atlas::Add(combined,mask.regions[i])) {++MM3D::prompt_unsupported;return true;}
        guest_regions=combined;return false;
    });
    const bool had_atlas=atlas_uniforms.info[0]>0||atlas_uniforms.pause[0]||atlas_uniforms.pause[1]||atlas_uniforms.pause[2]||atlas_uniforms.pause[3];
    guest_regions.pause=pause_draw;
    atlas_uniforms=guest_regions;
    atlas_uniforms.info[1]=1; // Original pass keeps everything outside the selected regions.
    const bool use_pause=pause_draw[0]||pause_draw[1]||pause_draw[2]||pause_draw[3];
    fs_data_dirty |= had_atlas || guest_regions.info[0]>0 || use_pause;
#endif

    // Sync and bind the shader
    pipeline_cache.UseFragmentShader(regs, user_config
#ifdef ENABLE_MM3D_PRESENTATION
        , guest_regions.info[0]>0 || use_pause
#endif
    );

    // Sync the LUTs within the texture buffer
    SyncAndUploadLUTs();
    SyncAndUploadLUTsLF();
#ifdef ENABLE_MM3D_PRESENTATION
    if (MM3D::Trace::active.load() || MM3D::CameraCapture::active.load()) {
        MM3D::Trace::Uniforms observed{};
        if (accelerate) {
            for (unsigned slot = 0; slot < 96; ++slot) {
                const auto& value = pica.vs_setup.uniforms.f[slot];
                observed[slot] = {value.x.ToFloat32(), value.y.ToFloat32(),
                                  value.z.ToFloat32(), value.w.ToFloat32()};
            }
        }
        if(!shadow_rendering && using_color_fb) {
            MM3D::CameraCapture::Observe(MM3D::guest_frames.load(), pica.vs_setup.GetProgramCodeHash(),
                pica.vs_setup.GetSwizzleDataHash(), regs.pipeline.num_vertices, accelerate,
                wide_viewport.GetWidth(), wide_viewport.GetHeight(),
                u32(regs.framebuffer.output_merger.depth_color_mask), observed,
                {.vertex_base=regs.pipeline.vertex_attributes.GetPhysicalBaseAddress(),
                 .first_vertex=accelerate ? vertex_info.vs_input_index_min : 0,
                 .last_vertex=accelerate ? vertex_info.vs_input_index_max : 0,
                 .index_offset=is_indexed ? u32(regs.pipeline.index_array.offset) : 0,
                 .topology=u32(regs.pipeline.triangle_topology.Value()),
                 .indexed=is_indexed, .index16=is_indexed && regs.pipeline.index_array.format!=0,
                 .valid=accelerate});
        }
        MM3D::Trace::Observe(MM3D::guest_frames.load(), pica.vs_setup.GetProgramCodeHash(),
            pica.vs_setup.GetSwizzleDataHash(), regs.pipeline.num_vertices, accelerate, observed,
            std::span<const u32>(pica.vs_setup.GetProgramCode()).first(
                std::min<u32>(pica.vs_setup.GetBiggestProgramSize(), Pica::MAX_PROGRAM_CODE_LENGTH)));
    }
#endif
#ifdef ENABLE_MM3D_PRESENTATION
    // The original PICA palette is still available when vertices ran on the CPU.
    // Capture both methods so actor poses can be correlated with their actual draws.
    if(MM3D::LinkPoseCapture::Recording() && !shadow_rendering && using_color_fb &&
       MM3D::Widescreen::IsTopViewport(wide_viewport.GetWidth(),wide_viewport.GetHeight()) &&
       pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
       pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL) {
        MM3D::LinkPoseCapture::Draw sample;
        sample.frame=MM3D::guest_frames.load();sample.vertices=regs.pipeline.num_vertices;
        sample.vertex_base=regs.pipeline.vertex_attributes.GetPhysicalBaseAddress();
        sample.index_offset=is_indexed?u32(regs.pipeline.index_array.offset):0;
        for(unsigned bone=0;bone<10;++bone)for(unsigned i=0;i<12;++i)
            sample.palette[bone][i]=pica.vs_setup.uniforms.f[20+bone*3+i/4][i%4].ToFloat32();
        MM3D::LinkPoseCapture::Observe(std::move(sample));
    }
#endif
#ifdef ENABLE_MM3D_PRESENTATION
    if(MM3D::EffectCapture::Recording(MM3D::guest_frames.load()) && !shadow_rendering && using_color_fb &&
       MM3D::Widescreen::IsTopViewport(wide_viewport.GetWidth(),wide_viewport.GetHeight())) {
        MM3D::EffectCapture::Draw d;
        d.frame=MM3D::guest_frames.load();d.shader=pica.vs_setup.GetProgramCodeHash();
        d.swizzle=pica.vs_setup.GetSwizzleDataHash();d.vertices=regs.pipeline.num_vertices;
        d.base=regs.pipeline.vertex_attributes.GetPhysicalBaseAddress();
        d.offset=is_indexed?u32(regs.pipeline.index_array.offset):0;
        d.indexed=is_indexed;d.accelerated=accelerate;
        d.textures={regs.texturing.texture0.GetPhysicalAddress(),regs.texturing.texture1.GetPhysicalAddress(),regs.texturing.texture2.GetPhysicalAddress()};
        const auto& b=pipeline_info.state.blending;
        d.state={unsigned(b.blend_enable),unsigned(b.src_color_blend_factor.Value()),unsigned(b.dst_color_blend_factor.Value()),
            unsigned(b.color_blend_eq.Value()),unsigned(b.color_write_mask),
            unsigned(pipeline_info.state.depth_stencil.depth_test_enable),unsigned(pipeline_info.state.depth_stencil.stencil_test_enable)};
        for(unsigned r=0;r<96;++r)for(unsigned c=0;c<4;++c)d.uniforms[r][c]=pica.vs_setup.uniforms.f[r][c].ToFloat32();
        if(cpu_interpolation && pica.CanReplayCpuVertices() &&
           d.shader==0x89b1ab98da3d9241ULL && d.vertices<=144 &&
           b.blend_enable && b.src_color_blend_factor==Pica::FramebufferRegs::BlendFactor::SourceAlpha &&
           pipeline_info.state.depth_stencil.depth_test_enable) {
            const auto& replay=pica.GetCpuVertexReplay();
            if(replay.Inputs().size()<=144 && replay.Order().size()<=144) {
                d.cpu_topology=unsigned(regs.pipeline.triangle_topology.Value());
                for(unsigned a=0;a<16;++a)d.cpu_input_registers[a]=regs.vs.GetRegisterForAttribute(a);
                d.cpu_inputs.resize(replay.Inputs().size());
                for(std::size_t i=0;i<replay.Inputs().size();++i)
                    for(unsigned a=0;a<16;++a)for(unsigned c=0;c<4;++c)
                        d.cpu_inputs[i][a][c]=replay.Inputs()[i][a][c].ToFloat32();
                d.cpu_order=replay.Order();
            }
        }
        if(accelerate && d.shader==0x89b1ab98da3d9241ULL && d.vertices<=144 &&
           b.blend_enable && b.src_color_blend_factor==Pica::FramebufferRegs::BlendFactor::SourceAlpha &&
           pipeline_info.state.depth_stencil.depth_test_enable) {
            const auto& attributes=regs.pipeline.vertex_attributes;
            for(unsigned i=0;i<12;++i)d.formats[i]={unsigned(attributes.GetFormat(i)),
                attributes.GetNumElements(i),regs.vs.GetRegisterForAttribute(i)};
            unsigned total=0;
            for(const auto& loader:attributes.attribute_loaders) {
                if(!loader.component_count || !loader.byte_count ||
                   vertex_info.vs_input_index_max<vertex_info.vs_input_index_min)continue;
                const u64 address=u64(attributes.GetPhysicalBaseAddress())+loader.data_offset+
                    u64(vertex_info.vs_input_index_min)*loader.byte_count;
                const u64 size=(u64(vertex_info.vs_input_index_max)-vertex_info.vs_input_index_min+1)*loader.byte_count;
                if(address>0xffffffff || !size || size>4096 || total+size>8192)continue;
                const auto src=memory.GetPhysicalRef(static_cast<PAddr>(address));
                if(src.GetSize()<size)continue;
                MM3D::EffectCapture::Buffer buffer;
                buffer.first=vertex_info.vs_input_index_min;buffer.last=vertex_info.vs_input_index_max;
                buffer.stride=loader.byte_count;
                for(unsigned i=0;i<loader.component_count && i<12;++i)buffer.components.push_back(loader.GetComponent(i));
                buffer.bytes.assign(src.GetPtr(),src.GetPtr()+size);total+=unsigned(size);
                d.buffers.push_back(std::move(buffer));
            }
        }
        MM3D::EffectCapture::Observe(std::move(d));
    }
#endif
    UploadUniforms(accelerate);
#ifdef ENABLE_MM3D_PRESENTATION
    if (MM3D::Textures::hide_current_draw) {
        MM3D::Textures::hidden.fetch_add(1);
        return true;
    }
    if (MM3D::draw_probe.Hide(MM3D::guest_frames.load(), pica.vs_setup.GetProgramCodeHash(),
                              regs.pipeline.num_vertices, accelerate)) {
        return true;
    }
#endif

    // CPU geometry must be uploaded outside recording scopes: Map can drain
    // batches or submit work. Retain immutable offsets for both target draws.
    struct CpuDraw { u32 offset{}, count{}; };
    CpuDraw native_cpu_draw{}, midpoint_cpu_draw{};
    bool use_midpoint_cpu_draw = false;
    const auto upload_cpu_draw = [&](const std::vector<HardwareVertex>& vertices) {
        CpuDraw draw;
        draw.count = static_cast<u32>(vertices.size());
        if (!draw.count) return draw;
        const u32 bytes = draw.count * sizeof(HardwareVertex);
        const auto [data, offset, invalidated] = stream_buffer.Map(bytes, sizeof(HardwareVertex));
        std::memcpy(data, vertices.data(), bytes);
        stream_buffer.Commit(bytes);
        draw.offset = offset;
        return draw;
    };
    if (cpu_interpolation) native_cpu_draw = upload_cpu_draw(vertex_batch);

    // Begin rendering
    // Mapping indices may submit work and end an open pass. In batched mode
    // upload once before beginning the native pass, then reuse for its replays.
    if (accelerate && is_indexed && scheduler.ReplayBatchingEnabled()) SetupIndexArray();
    Scheduler::NativeDrawScope native_draw_scope{scheduler,
        (accelerate || cpu_interpolation) && !shadow_rendering};
    const auto draw_rect = fb_helper.DrawRect();
    renderpass_cache.BeginRendering(framebuffer, draw_rect);

    // Configure viewport and scissor
    const auto viewport = fb_helper.Viewport();
    pipeline_info.dynamic_info.viewport = Common::Rectangle<s32>{
        viewport.x,
        viewport.y,
        viewport.x + viewport.width,
        viewport.y + viewport.height,
    };
    pipeline_info.dynamic_info.scissor = draw_rect;
#ifdef ENABLE_MM3D_PRESENTATION
    auto guest_profile = std::find_if(mm3d_draw_profiles.begin(),mm3d_draw_profiles.end(),
        [](const auto& p){return !p.presentation && !p.atlas_selection && p.id == "@preview";});
    if (guest_profile == mm3d_draw_profiles.end())
        guest_profile = std::find_if(mm3d_draw_profiles.begin(),mm3d_draw_profiles.end(),
            [](const auto& p){return !p.presentation && !p.atlas_selection;});
    const auto transform_viewport=[&](const MM3D::Profiles::Profile& transform) {
        // Diagnostic screen-space transform; shader animation and guest data stay intact.
        // Preserve viewport orientation, including flipped/negative-height viewports.
        auto& bounds = pipeline_info.dynamic_info.viewport;
        const s32 dx = bounds.right - bounds.left;
        const s32 dy = bounds.bottom - bounds.top;
        if (std::abs(dx) >= 2 && std::abs(dy) >= 2) {
            const float scale = std::clamp(transform.scale, .1f, 2.f);
            const s32 width = static_cast<s32>(dx * scale);
            const s32 height = static_cast<s32>(dy * scale);
            const s32 cx = bounds.left + dx / 2 + static_cast<s32>(dx * transform.y);
            const s32 cy = bounds.top + dy / 2 + static_cast<s32>(dy * transform.x);
            bounds.left = cx - width / 2;
            bounds.top = cy - height / 2;
            bounds.right = bounds.left + width;
            bounds.bottom = bounds.top + height;
            // Crop follows the transformed draw. Display X/Y are rotated relative to PICA X/Y.
            const auto& crop = transform.crop;
            auto& scissor = pipeline_info.dynamic_info.scissor;
            const s32 min_x = std::min(bounds.left,bounds.right), min_y = std::min(bounds.top,bounds.bottom);
            const s32 size_x = std::abs(bounds.right-bounds.left), size_y = std::abs(bounds.bottom-bounds.top);
            const s32 left = std::clamp<s32>(min_x+size_x*(1.f-crop[3]),scissor.left,scissor.right);
            const s32 right = std::clamp<s32>(min_x+size_x*(1.f-crop[1]),left,scissor.right);
            const s32 bottom = std::clamp<s32>(min_y+size_y*crop[0],scissor.bottom,scissor.top);
            const s32 top = std::clamp<s32>(min_y+size_y*crop[2],bottom,scissor.top);
            scissor = {u32(left),u32(top),u32(right),u32(bottom)};
            MM3D::Textures::scaled_draws.fetch_add(1);
        }
    };
    if (guest_profile != mm3d_draw_profiles.end()) transform_viewport(*guest_profile);
    MM3D::draw_probe.ObserveScissor(MM3D::guest_frames.load(),
        pica.vs_setup.GetProgramCodeHash(), regs.pipeline.num_vertices,
        pipeline_info.dynamic_info.scissor.left, pipeline_info.dynamic_info.scissor.top,
        pipeline_info.dynamic_info.scissor.right, pipeline_info.dynamic_info.scissor.bottom);
    if(guest_regions.info[0]>0) pipeline_cache.BindPipeline(pipeline_info,true);
#endif

    // Every replay must use the same geometry path as the original draw. CPU
    // shader output is already a triangle list in vertex_batch; do not route it
    // through the accelerated path's guest attribute/index bindings.
    const auto draw_batch = [&]() -> bool {
        if (accelerate) {
            return AccelerateDrawBatchInternal(is_indexed);
        }
        if (!pipeline_cache.BindPipeline(pipeline_info, true)) {
            return false;
        }
        if (cpu_interpolation) {
            const auto draw = use_midpoint_cpu_draw ? midpoint_cpu_draw : native_cpu_draw;
            scheduler.Record([buffer = stream_buffer.Handle(), draw](vk::CommandBuffer cmdbuf) {
                cmdbuf.bindVertexBuffers(0, buffer, draw.offset);
                cmdbuf.draw(draw.count, 1, 0, 0);
            });
            return true;
        }
        const u32 vertex_count = static_cast<u32>(vertex_batch.size());
        const u32 vertex_size = vertex_count * sizeof(HardwareVertex);
        const auto [buffer, offset, _] = stream_buffer.Map(vertex_size, sizeof(HardwareVertex));
        std::memcpy(buffer, vertex_batch.data(), vertex_size);
        stream_buffer.Commit(vertex_size);
        scheduler.Record([this, offset, vertex_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindVertexBuffers(0, stream_buffer.Handle(), offset);
            cmdbuf.draw(vertex_count, 1, 0, 0);
        });
        return true;
    };
#ifdef ENABLE_MM3D_PRESENTATION
    if(MM3D::CameraMidpoint::suppress_transform_feedback.load() &&
       MM3D::CameraMidpoint::present_60.load() && replay_geometry && !shadow_rendering &&
       using_color_fb && !use_pause &&
       MM3D::Widescreen::IsTopViewport(wide_viewport.GetWidth(),wide_viewport.GetHeight()) &&
       pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
       pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL) {
        using namespace MM3D::CameraMidpoint;
        Matrix projection{},model=Identity();
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
            projection[i][j]=pica.vs_setup.uniforms.f[i][j].ToFloat32();
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<4;++j)
            model[i][j]=pica.vs_setup.uniforms.f[20+i][j].ToFloat32();
        const auto& blend=pipeline_info.state.blending;
        const auto& textures=regs.texturing.main_config;
        using BF=Pica::FramebufferRegs::BlendFactor;
        if(blend.blend_enable && blend.src_color_blend_factor==BF::SourceAlpha &&
           blend.dst_color_blend_factor==BF::OneMinusSourceAlpha &&
           blend.color_blend_eq==Pica::FramebufferRegs::BlendEquation::Add &&
           blend.color_write_mask==15 && !pipeline_info.state.depth_stencil.stencil_test_enable &&
           TransformationFeedback(projection,model,
               midpoint_scene_copies.PreviousContains(MM3D::guest_frames.load(),
                   regs.texturing.texture0.GetPhysicalAddress()),regs.pipeline.num_vertices,
               unsigned(textures.texture0_enable)|(unsigned(textures.texture1_enable)<<1)|
                   (unsigned(textures.texture2_enable)<<2))) {
            vertex_batch.clear();return true;
        }
    }
#endif
    const bool succeeded = draw_batch();
    native_draw_scope.End();

#ifdef ENABLE_MM3D_PRESENTATION
    const bool midpoint_title_logo=[&] {
        if(!MM3D::CameraMidpoint::enabled.load() ||
           pica.vs_setup.GetProgramCodeHash()!=0x89b1ab98da3d9241ULL ||
           pica.vs_setup.GetSwizzleDataHash()!=0x6f0b398b1fc186a9ULL)return false;
        auto camera=MM3D::CameraMidpoint::Identity();
        MM3D::CameraMidpoint::Matrix projection{};
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
            projection[r][c]=pica.vs_setup.uniforms.f[r][c].ToFloat32();
        for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)
            camera[r][c]=pica.vs_setup.uniforms.f[70+r][c].ToFloat32();
        return MM3D::CameraMidpoint::TitleLogoCamera(camera,projection);
    }();
    // Immediate replay retains the original draw's geometry/textures. Only this
    // private diagnostic target gets a different camera; the guest target stays native.
    // Depth-disabled/always-pass perspective draws include sky and translucent
    // effects. Keep their native depth/blending state and original draw order;
    // perspective + verified shader identify this path, not depth testing alone.
    if(MM3D::CameraMidpoint::enabled.load() && !midpoint_title_logo && replay_world && succeeded &&
       MM3D::Widescreen::supported_layout.load() && !shadow_rendering && using_color_fb &&
       MM3D::Widescreen::IsTopViewport(wide_viewport.GetWidth(),wide_viewport.GetHeight()) &&
       pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
       pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL &&
       (!pipeline_info.state.depth_stencil.stencil_test_enable ||
        midpoint_stencil_frame==MM3D::guest_frames.load()) && !guest_regions.info[0] &&
       !use_pause && viewport.x==0 && viewport.y==0 && viewport.width>0 && viewport.height>0) {
        MM3D::InterpolationPerformance::Scope performance_replay{MM3D::InterpolationPerformance::Part::Replay};
        using namespace MM3D::CameraMidpoint;
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto cpu=MM3D::CameraLive::GetSnapshot();
        Matrix camera=Identity(),projection{};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
            projection[i][j]=pica.vs_setup.uniforms.f[i][j].ToFloat32();
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<4;++j)
            camera[i][j]=pica.vs_setup.uniforms.f[70+i][j].ToFloat32();
        bool matches=cpu.valid && now>=cpu.sampled_at_ms && now-cpu.sampled_at_ms<=100;
        double direction_length=0,direction_dot=0;
        for(unsigned i=0;i<3;++i) {
            matches &= std::abs(camera[i][3]-cpu.eye[i])<1.;
            const double direction=cpu.target[i]-cpu.eye[i];
            direction_length+=direction*direction;direction_dot-=direction*camera[i][2];
        }
        matches &= direction_length>1e-6 && direction_dot/std::sqrt(std::max(direction_length,1e-6))>.9999;
        const bool valid_transform=Rigid(camera) && WorldProjection(projection);
        const auto epoch=generation.load();
        if(epoch!=midpoint_generation || midpoint_width!=unsigned(viewport.width) || midpoint_height!=unsigned(viewport.height)) {
            midpoint_scene_clear.Reset();
            midpoint_history.Reset();forest_particle_history.Reset();midpoint_target_frame.reset();midpoint_world_frame.reset();midpoint_opaque_world_frame.reset();midpoint_stencil_frame.reset();midpoint_generation=epoch;
            link_history.Reset();song_history.Reset();song_overrides.reset();link_frame.reset();link_overrides={};
            midpoint_width=unsigned(viewport.width);midpoint_height=unsigned(viewport.height);
        }
        using CF=Pica::FramebufferRegs::CompareFunc;
        const auto compare=regs.framebuffer.output_merger.depth_test_func.Value();
        const bool valid_depth=!regs.framebuffer.output_merger.depth_test_enable ||
            compare==CF::Always || compare==CF::LessThan || compare==CF::LessThanOrEqual ||
            compare==CF::GreaterThan || compare==CF::GreaterThanOrEqual;
        if(valid_transform && valid_depth) {
            // Sample both accepted and rejected cameras together. Title/attract
            // rendering can mix cameras; rejection counters alone cannot tell
            // an auxiliary draw from a stale CPU snapshot. Bound trace volume.
            if(MM3D::InputLatency::Active()) {
                static std::uint64_t trace_frame=~std::uint64_t{};
                static unsigned trace_draws[2]{};
                const auto frame=MM3D::guest_frames.load();
                if(trace_frame!=frame) { trace_frame=frame;trace_draws[0]=trace_draws[1]=0; }
                if(frame%30<2 && trace_draws[matches?1:0]++<24) {
                    LOG_INFO(Render_Vulkan,
                        "Camera match trace frame={} vertices={} matches={} cpu_valid={} age={} base={:08x} eye={},{},{} target={},{},{} camera={},{},{},{};{},{},{},{};{},{},{},{} lens={},{},{},{}",
                        frame,regs.pipeline.num_vertices,matches,cpu.valid,now-cpu.sampled_at_ms,cpu.base,
                        cpu.eye[0],cpu.eye[1],cpu.eye[2],cpu.target[0],cpu.target[1],cpu.target[2],
                        camera[0][0],camera[0][1],camera[0][2],camera[0][3],
                        camera[1][0],camera[1][1],camera[1][2],camera[1][3],
                        camera[2][0],camera[2][1],camera[2][2],camera[2][3],
                        projection[0][0],projection[1][1],projection[2][2],projection[2][3]);
                }
            }
            bool fallback=false;
            const auto selected=midpoint_history.Preview(MM3D::guest_frames.load(),now,
                camera,projection,matches,native_preview.load(),fallback);
            {
                auto& layer=prompt_layers["@camera_midpoint"];
                if(!layer) {
                    layer=std::make_unique<MM3D::PromptLayer>(instance,scheduler,renderpass_cache,true);
                    layer->profile.id="@camera_midpoint";layer->profile.source_top=true;
                    layer->profile.scale=.42f;layer->profile.x=.26f;layer->profile.y=-.26f;
                }
                const auto frame=MM3D::guest_frames.load();
                if(midpoint_target_frame!=frame) {
                    layer->NextFrame();
                    midpoint_target_frame=frame;
                    midpoint_frame_interpolated=false;
                    if(midpoint_fade_frame!=frame) {
                        if(const auto fade=prompt_layers.find("@camera_midpoint_fade");fade!=prompt_layers.end())
                            fade->second->NextFrame();
                    }
                }
                const auto saved=pipeline_info;
                midpoint_projection=selected;
                if(MM3D::LinkPoseCapture::Enabled()) {
                    const auto link_epoch=MM3D::LinkPoseCapture::interpolation_generation.load();
                    if(link_epoch!=link_generation) {
                        link_history.Reset();song_history.Reset();song_overrides.reset();link_frame.reset();link_generation=link_epoch;
                    }
                    if(link_frame!=frame) {
                        link_poses=MM3D::LinkPoseCapture::ForFrame(frame);link_frame=frame;
                    }
                    MM3D::LinkInterpolation::Palette palette;
                    for(unsigned bone=0;bone<10;++bone) {
                        palette[bone]=Identity();
                        for(unsigned i=0;i<12;++i)
                            palette[bone][i/4][i%4]=pica.vs_setup.uniforms.f[20+bone*3+i/4][i%4].ToFloat32();
                    }
                    // Sample consecutive frames of the verified song effect during
                    // an explicit timing trace, before midpoint uniforms are applied.
                    if(MM3D::InputLatency::Active() && frame%10<3) {
                        bool song_effect=false;
                        for(const auto& pose:link_poses) {
                            if(pose.signature!=0x04e3fb346228fd20ull ||
                               pose.pose.bones.size()!=4 || pose.bind.size()!=4)continue;
                            std::vector<MM3D::LinkPoseMath::Matrix> bones;
                            for(const auto& bone:pose.pose.bones)
                                bones.push_back(MM3D::LinkPoseMath::Expand(bone));
                            if(MM3D::LinkInterpolation::ResolvePalette(palette[0],pose,bones,Identity()))
                                song_effect=true;
                        }
                        if(song_effect) {
                            const auto& blend=pipeline_info.state.blending;
                            LOG_INFO(Render_Vulkan,
                                "Song effect draw frame={} vertices={} shader={:016x} swizzle={:016x} textures={:08x},{:08x},{:08x} blend={} src={} dst={} equation={}",
                                frame,regs.pipeline.num_vertices,pica.vs_setup.GetProgramCodeHash(),
                                pica.vs_setup.GetSwizzleDataHash(),regs.texturing.texture0.GetPhysicalAddress(),
                                regs.texturing.texture1.GetPhysicalAddress(),regs.texturing.texture2.GetPhysicalAddress(),
                                unsigned(blend.blend_enable),unsigned(blend.src_color_blend_factor.Value()),
                                unsigned(blend.dst_color_blend_factor.Value()),unsigned(blend.color_blend_eq.Value()));
                            const auto& uniforms=pica.vs_setup.uniforms.f;
                            for(unsigned index=0;index<uniforms.size();++index) {
                                const auto& v=uniforms[index];
                                LOG_INFO(Render_Vulkan,"Song effect uniform frame={} vertices={} index={} value={},{},{},{}",
                                    frame,regs.pipeline.num_vertices,index,v.x.ToFloat32(),v.y.ToFloat32(),
                                    v.z.ToFloat32(),v.w.ToFloat32());
                            }
                        }
                    }
                    song_overrides.reset();
                    if(pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
                       pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL) {
                        MM3D::SongEffectInterpolation::Values values{};
                        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
                            values[r][c]=pica.vs_setup.uniforms.f[MM3D::SongEffectInterpolation::Rows[r]][c].ToFloat32();
                        const auto& blend=pipeline_info.state.blending;
                        using BF=Pica::FramebufferRegs::BlendFactor;
                        if(MM3D::SongEffectInterpolation::Recognized(regs.pipeline.num_vertices,values) &&
                           blend.blend_enable && blend.src_color_blend_factor==BF::SourceAlpha &&
                           (blend.dst_color_blend_factor==BF::One || blend.dst_color_blend_factor==BF::OneMinusSourceAlpha) &&
                           blend.color_blend_eq==Pica::FramebufferRegs::BlendEquation::Add &&
                           pipeline_info.state.depth_stencil.depth_test_enable &&
                           !pipeline_info.state.depth_stencil.stencil_test_enable && blend.color_write_mask==15) {
                            song_overrides=song_history.Apply(frame,{
                                regs.pipeline.vertex_attributes.GetPhysicalBaseAddress(),
                                regs.texturing.texture0.GetPhysicalAddress(),regs.texturing.texture1.GetPhysicalAddress(),
                                regs.texturing.texture2.GetPhysicalAddress(),unsigned(blend.dst_color_blend_factor.Value()),
                                regs.pipeline.num_vertices,is_indexed?u32(regs.pipeline.index_array.offset):0},
                                values,!fallback && !native_preview.load());
                        }
                    }
                    {
                        MM3D::InterpolationPerformance::Scope performance_skeleton{MM3D::InterpolationPerformance::Part::Skeleton};
                        link_overrides=link_history.Apply(frame,now,camera,palette,link_poses,
                            !fallback && !native_preview.load(),regs.pipeline.num_vertices);
                    }
                    if(MM3D::InputLatency::Active() && frame%10<3 &&
                       (regs.pipeline.num_vertices==4 || regs.pipeline.num_vertices==18 ||
                        regs.pipeline.num_vertices==24)) {
                        for(const auto& pose:link_poses) {
                            if(pose.signature!=0x09e2df0cf6765f8eull)continue;
                            const bool glow=MM3D::LinkInterpolation::FairyGlow(palette[0],pose,regs.pipeline.num_vertices);
                            if(!glow && !MM3D::LinkInterpolation::Identify(palette[0],pose))continue;
                            const auto output=link_overrides[0].value_or(palette[0]);
                            LOG_INFO(Render_Vulkan,
                                "Fairy alignment frame={} instance={:08x} vertices={} glow={} cpu_match={} fallback={} override={} camera={},{},{} native={},{},{} output={},{},{}",
                                frame,pose.pose.instance,regs.pipeline.num_vertices,glow,matches,fallback,
                                bool(link_overrides[0]),camera[0][3],camera[1][3],camera[2][3],
                                palette[0][0][3],palette[0][1][3],palette[0][2][3],
                                output[0][3],output[1][3],output[2][3]);
                        }
                    }
                    if(MM3D::InputLatency::Active() && frame%10==0 &&
                       regs.pipeline.num_vertices==1284 &&
                       std::any_of(link_poses.begin(),link_poses.end(),[](const auto& pose) {
                           return pose.signature==0x79937db4f6cb5c95ull;
                       })) {
                        const auto scale=[](const auto& m) {
                            return std::sqrt(m[0][0]*m[0][0]+m[1][0]*m[1][0]+m[2][0]*m[2][0]);
                        };
                        for(const auto& pose:link_poses)if(pose.signature==0x79937db4f6cb5c95ull) {
                            for(unsigned bone=0;bone<pose.pose.bones.size();++bone)
                                LOG_INFO(Render_Vulkan,"Deku pipe skeleton frame={} instance={} bone={} scale={}",
                                    frame,pose.pose.instance,bone,scale(MM3D::LinkPoseMath::Expand(pose.pose.bones[bone])));
                        }
                        for(unsigned slot=0;slot<6;++slot)
                            LOG_INFO(Render_Vulkan,
                                "Deku pipe interpolation frame={} slot={} fallback={} comparison={} override={} scale={} midpoint_scale={}",
                                frame,slot,fallback,native_preview.load(),bool(link_overrides[slot]),
                                scale(palette[slot]),link_overrides[slot]?scale(*link_overrides[slot]):-1.0);
                    }
                }
                if (cpu_interpolation) {
                    MM3D::InterpolationPerformance::Scope cpu_replay_timing{
                        MM3D::InterpolationPerformance::Part::CpuVertexReplay};
                    // Override only the verified forest sprite input layout. Work
                    // on a copy; the native draw and its replay cache stay intact.
                    std::vector<Pica::AttributeBuffer> particle_inputs;
                    const auto& blend = pipeline_info.state.blending;
                    using BF = Pica::FramebufferRegs::BlendFactor;
                    if (regs.pipeline.triangle_topology == Pica::PipelineRegs::TriangleTopology::Strip &&
                        regs.pipeline.num_vertices <= 144 &&
                        regs.vs.max_input_attribute_index == 2 &&
                        regs.vs.GetRegisterForAttribute(0) == 0 &&
                        regs.vs.GetRegisterForAttribute(1) == 3 &&
                        regs.vs.GetRegisterForAttribute(2) == 4 &&
                        blend.blend_enable && blend.src_color_blend_factor == BF::SourceAlpha &&
                        blend.dst_color_blend_factor == BF::One &&
                        blend.color_blend_eq == Pica::FramebufferRegs::BlendEquation::Add &&
                        blend.color_write_mask == 15 && pipeline_info.state.depth_stencil.depth_test_enable &&
                        !pipeline_info.state.depth_stencil.stencil_test_enable) {
                        const auto& replay = pica.GetCpuVertexReplay();
                        std::vector<MM3D::ForestParticleInterpolation::Vertex> inputs(replay.Inputs().size());
                        for (unsigned i = 0; i < inputs.size(); ++i)
                            for (unsigned a = 0; a < 3; ++a)
                                for (unsigned c = 0; c < 4; ++c)
                                    inputs[i][a][c] = replay.Inputs()[i][a][c].ToFloat32();
                        if (const auto quads = MM3D::ForestParticleInterpolation::Decode(inputs, replay.Order())) {
                            const auto half = forest_particle_history.Apply(frame,
                                regs.texturing.texture0.GetPhysicalAddress(), epoch, *quads);
                            if (half && !fallback && !native_preview.load()) {
                                particle_inputs = replay.Inputs();
                                for (const auto& q : *half)
                                    for (unsigned v = 0; v < 4; ++v)
                                        for (unsigned a = 0; a < 3; ++a)
                                            for (unsigned c = 0; c < 4; ++c)
                                                particle_inputs[q.indices[v]][a][c] =
                                                    Pica::f24::FromFloat32(q.vertices[v][a][c]);
                            }
                        }
                    }
                    auto uniforms = pica.vs_setup.uniforms;
                    for (unsigned row = 0; row < 4; ++row)
                        for (unsigned col = 0; col < 4; ++col)
                            uniforms.f[row][col] = Pica::f24::FromFloat32(float((*midpoint_projection)[row][col]));
                    for (unsigned bone = 0; bone < link_overrides.size(); ++bone) {
                        if (!link_overrides[bone]) continue;
                        for (unsigned i = 0; i < 12; ++i)
                            uniforms.f[20 + bone * 3 + i / 4][i % 4] = Pica::f24::FromFloat32(
                                float((*link_overrides[bone])[i / 4][i % 4]));
                    }
                    if (song_overrides)
                        for (unsigned row = 0; row < 4; ++row)
                            uniforms.f[MM3D::SongEffectInterpolation::Rows[row]][3] =
                                Pica::f24::FromFloat32((*song_overrides)[row][3]);
                    auto& vertices = midpoint_cpu_vertices;
                    vertices.clear();
                    vertices.reserve(vertex_batch.size());
                    pica.ReplayCpuVertices(uniforms, [&](const auto& a, const auto& b, const auto& c) {
                        AppendTriangle(vertices, a, b, c);
                    }, particle_inputs);
                    midpoint_cpu_draw = upload_cpu_draw(vertices);
                    use_midpoint_cpu_draw = true;
                }
                UploadUniforms(accelerate);
                const bool reverse=compare==CF::GreaterThan || compare==CF::GreaterThanOrEqual;
                {
                RenderManager::ReplayScope batching{renderpass_cache,
                    layer->CanBatch(std::max<unsigned>(draw_rect.right, viewport.width),
                                    std::max<unsigned>(draw_rect.top, viewport.height)),
                    layer->Attachments()};
                if(layer->Begin(std::max<unsigned>(draw_rect.right,viewport.width),
                                std::max<unsigned>(draw_rect.top,viewport.height),reverse?0.f:1.f,
                                midpoint_world_frame!=frame)) {
                    midpoint_world_frame=frame;
                    // Sky is perspective geometry too, but an early screen
                    // darkener belongs behind opaque actors/scenery drawn next.
                    const auto& native_blend=pipeline_info.state.blending;
                    using BF=Pica::FramebufferRegs::BlendFactor;
                    Matrix world_model=Identity();
                    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<4;++col)
                        world_model[row][col]=pica.vs_setup.uniforms.f[20+row][col].ToFloat32();
                    if(!OpaqueSkyDome(regs.pipeline.num_vertices,projection,world_model) &&
                       pipeline_info.state.depth_stencil.depth_test_enable &&
                       native_blend.color_write_mask==15 &&
                       (!native_blend.blend_enable ||
                        (native_blend.src_color_blend_factor==BF::One &&
                         native_blend.dst_color_blend_factor==BF::Zero &&
                         native_blend.color_blend_eq==Pica::FramebufferRegs::BlendEquation::Add)))
                        midpoint_opaque_world_frame=frame;
                    pipeline_info.state.attachments.color=VideoCore::PixelFormat::RGBA8;
                    pipeline_info.state.attachments.depth=VideoCore::PixelFormat::D24S8;
                    pipeline_info.dynamic_info.viewport={viewport.x,viewport.y,viewport.x+viewport.width,viewport.y+viewport.height};
                    pipeline_info.dynamic_info.scissor=draw_rect;
                    pipeline_cache.BindPipeline(pipeline_info,true);
                    const auto native_bindings=binding_offsets;
                    if(accelerate && ripple_offsets && !fallback && !native_preview.load()) {
                        binding_offsets[0]=(*ripple_offsets)[0];binding_offsets[1]=(*ripple_offsets)[1];
                    }
                    if(accelerate && splash_offsets && !fallback && !native_preview.load()) {
                        binding_offsets[0]=(*splash_offsets)[0];binding_offsets[1]=(*splash_offsets)[1];
                        if(splash_blend) {
                            binding_offsets[2]=(*splash_offsets)[2];draw_batch();
                            binding_offsets[2]=native_bindings[2];
                        }
                    }
                    draw_batch();
                    binding_offsets=native_bindings;
                    layer->x=viewport.x;layer->y=viewport.y;
                    layer->viewport_width=viewport.width;layer->viewport_height=viewport.height;
                    layer->End(false);++midpoint_copies;midpoint_last_draw_ms=now;
                    midpoint_scene_copies.Begin(frame,regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress());
                    if(fallback)++midpoint_fallback;
                    else midpoint_frame_interpolated=true;
                    const auto bones=std::count_if(link_overrides.begin(),link_overrides.end(),
                        [](const auto& value){return value.has_value();});
                    if(bones) {
                        ++MM3D::LinkPoseCapture::replaced_draws;
                        MM3D::LinkPoseCapture::replaced_bones.fetch_add(unsigned(bones));
                    }
                }
                }
                pipeline_info=saved;midpoint_projection.reset();link_overrides={};song_overrides.reset();
                use_midpoint_cpu_draw = false;
                pica.vs_setup.uniforms_dirty=true;UploadUniforms(accelerate);
            }
        } else ++midpoint_rejected;
    }
#endif

    // Replay verified UI draws using the same shader/uniforms/geometry. Guest target is untouched.
#ifdef ENABLE_MM3D_PRESENTATION
    for(const auto& match:mm3d_draw_profiles) {
        if(!match.atlas_selection || match.presentation) continue;
        MM3D::Atlas::Build(match,atlas_uniforms);
        fs_data_dirty=true; UploadUniforms(accelerate);
        pipeline_cache.UseFragmentShader(regs,user_config,true);
        // Uniform uploads can submit command buffers and end the current pass.
        renderpass_cache.BeginRendering(framebuffer,draw_rect);
        pipeline_info.dynamic_info.viewport={viewport.x,viewport.y,viewport.x+viewport.width,viewport.y+viewport.height};
        pipeline_info.dynamic_info.scissor=draw_rect;
        transform_viewport(match);
        pipeline_cache.BindPipeline(pipeline_info,true);
        draw_batch();
    }
    for (const auto& panel : MM3D::Profiles::Panels()) {
        const bool source_matches = viewport.x==0 && viewport.y==0 && viewport.width>0 && viewport.height>0 &&
            (panel.source_top ? s64(viewport.width)*5==s64(viewport.height)*3 :
                                s64(viewport.width)*4==s64(viewport.height)*3);
        if (!source_matches) continue;
        const bool excluded = std::any_of(panel.exclusions.begin(),panel.exclusions.end(),[&](const auto& rule) {
            return std::find(panel_draw_identities.begin(),panel_draw_identities.end(),rule)!=panel_draw_identities.end();
        });
        if (!excluded) mm3d_draw_profiles.push_back(panel);
    }
    // Manual panel experiment: replay lower-screen UI into one transparent layer.
    // Full viewport only; other render targets and partial viewports are not inferred.
    if (MM3D::Notebook::detail_preview.load() && MM3D::Notebook::transparent_copy.load() &&
        viewport.x == 0 && viewport.y == 0 && viewport.width > 0 && viewport.height > 0 &&
        s64(viewport.width)*4 == s64(viewport.height)*3) {
        if (panel_excluded) ++MM3D::Notebook::excluded_draws;
        else {
            MM3D::Profiles::Profile panel;
            panel.id = "@notebook_panel";
            panel.presentation = true;
            mm3d_draw_profiles.push_back(panel);
        }
    }
    // Keep the map's lower-screen geometry and radar sprites in a transparent
    // layer. LayoutShader controls are excluded; presentation also requires the
    // exact radar texture to have appeared in this same frame.
    const auto map_viewport=regs.rasterizer.GetViewportRect();
    if(MM3D::Minimap::LegacyCopy && MM3D::Minimap::enabled.load() && !shadow_rendering &&
       map_viewport.GetWidth()==240 && map_viewport.GetHeight()==320 &&
       (MM3D::Minimap::marker_draw || pica.vs_setup.GetProgramCodeHash()!=0xed1d199d94013c51ULL)) {
        MM3D::Profiles::Profile map;
        map.id="@minimap"; map.presentation=true;
        mm3d_draw_profiles.push_back(map);
    }
    // Reuse the verified native screen cover, with native timing/color, in a
    // separate transparent layer. Never apply a camera transform to this quad.
    if(MM3D::CameraMidpoint::enabled.load() && replay_geometry && succeeded &&
       !shadow_rendering && using_color_fb && !write_depth_fb && !use_pause &&
       !pipeline_info.state.depth_stencil.stencil_test_enable && !guest_regions.info[0]) {
        const auto& textures=regs.texturing.main_config;
        const auto& merger=regs.framebuffer.output_merger;
        MM3D::CameraMidpoint::Matrix projection{};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
            projection[i][j]=pica.vs_setup.uniforms.f[i][j].ToFloat32();
        const MM3D::WidescreenProbe::Draw candidate{
            pica.vs_setup.GetProgramCodeHash(),pica.vs_setup.GetSwizzleDataHash(),
            u32(regs.pipeline.num_vertices),unsigned(wide_viewport.GetWidth()),
            unsigned(wide_viewport.GetHeight()),
            unsigned(textures.texture0_enable)|(unsigned(textures.texture1_enable)<<1)|
                (unsigned(textures.texture2_enable)<<2),
            unsigned(merger.alpha_blending.factor_source_rgb.Value())|
                (unsigned(merger.alpha_blending.factor_dest_rgb.Value())<<4)|
                (unsigned(merger.alpha_blending.blend_equation_rgb.Value())<<8)|
                (unsigned(merger.alphablend_enable)<<16),u32(merger.depth_color_mask)};
        if(MM3D::CameraMidpoint::TransitionFade(candidate,projection)) {
            const auto frame=MM3D::guest_frames.load();
            if(midpoint_fade_frame!=frame) {
                if(const auto it=prompt_layers.find("@camera_midpoint_fade");it!=prompt_layers.end())
                    it->second->NextFrame();
                midpoint_fade_frame=frame;
            }
            MM3D::Profiles::Profile fade;
            fade.id="@camera_midpoint_fade";fade.presentation=true;fade.source_top=true;
            mm3d_draw_profiles.push_back(fade);
            // A fade-only frame can still cover the last captured scene.
            midpoint_last_draw_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }
    // Capture the reason, not just the aggregate fallback, during an explicit
    // timing trace. Limit each rejection category to one record per second.
    // Reasons: draw failed, stencil, unknown shader, layer limit, allocation, replay state.
    const auto reject_midpoint_hud = [&](unsigned reason) {
        midpoint_hud_failed=true;
        if(!MM3D::InputLatency::Active() &&
           !(MM3D::ScreenShrink::native_normalized.load() &&
             MM3D::ScreenShrink::presentation_scale.load()<1.f))return;
        const auto now=MM3D::InputLatency::Now();
        if(now-midpoint_rejection_logged[reason]<1000000)return;
        midpoint_rejection_logged[reason]=now;
        const auto& blend=pipeline_info.state.blending;
        LOG_INFO(Render_Vulkan,
            "Interpolation HUD rejection: reason={} shader={:016x} swizzle={:016x} "
            "vertices={} viewport={}x{} origin={},{} accelerated={} succeeded={} "
            "stencil={} blend={} src={} dst={} equation={} mask={} "
            "texture0={:08x} framebuffer={:08x} shrink={} normalized={} "
            "textures={} frame={} source_frame={} sources={:08x},{:08x},{:08x},{:08x}",
            reason,pica.vs_setup.GetProgramCodeHash(),pica.vs_setup.GetSwizzleDataHash(),
            u32(regs.pipeline.num_vertices),viewport.width,viewport.height,viewport.x,viewport.y,
            accelerate,succeeded,unsigned(pipeline_info.state.depth_stencil.stencil_test_enable),
            unsigned(blend.blend_enable),unsigned(blend.src_color_blend_factor.Value()),
            unsigned(blend.dst_color_blend_factor.Value()),unsigned(blend.color_blend_eq.Value()),
            unsigned(blend.color_write_mask),regs.texturing.texture0.GetPhysicalAddress(),
            regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress(),
            MM3D::ScreenShrink::presentation_scale.load(),MM3D::ScreenShrink::native_normalized.load(),
            unsigned(regs.texturing.main_config.texture0_enable)|
                (unsigned(regs.texturing.main_config.texture1_enable)<<1)|
                (unsigned(regs.texturing.main_config.texture2_enable)<<2),
            MM3D::guest_frames.load(),midpoint_scene_copies.frame,
            midpoint_scene_copies.addresses[0],midpoint_scene_copies.addresses[1],
            midpoint_scene_copies.addresses[2],midpoint_scene_copies.addresses[3]);
    };
    if(MM3D::CameraMidpoint::present_60.load() &&
       !shadow_rendering &&
       using_color_fb && !use_pause &&
       MM3D::Widescreen::IsTopViewport(wide_viewport.GetWidth(),wide_viewport.GetHeight())) {
        const auto frame=MM3D::guest_frames.load();
        if(midpoint_hud_frame!=frame) {
            midpoint_hud_frame=frame;midpoint_hud_failed=false;
            if(const auto it=prompt_layers.find("@camera_midpoint_hud");it!=prompt_layers.end())
                it->second->NextFrame();
        }
        if(!replay_geometry || !succeeded)reject_midpoint_hud(0);
        const auto shader=pica.vs_setup.GetProgramCodeHash();
        const auto swizzle=pica.vs_setup.GetSwizzleDataHash();
        const bool world=shader==0x89b1ab98da3d9241ULL && swizzle==0x6f0b398b1fc186a9ULL;
        const bool ortho=world && pica.vs_setup.uniforms.f[3].x.ToFloat32()==0 &&
            pica.vs_setup.uniforms.f[3].y.ToFloat32()==0 &&
            pica.vs_setup.uniforms.f[3].z.ToFloat32()==0 &&
            pica.vs_setup.uniforms.f[3].w.ToFloat32()==1;
        const bool layout=shader==0x9b343fea686d98e0ULL && swizzle==0xac22b79e29209c01ULL;
        if(cpu_interpolation && world && !ortho && !midpoint_title_logo && !replay_world)
            reject_midpoint_hud(0); // Never silently omit unsupported CPU world geometry.
        const bool fade=std::any_of(mm3d_draw_profiles.begin(),mm3d_draw_profiles.end(),
            [](const auto& p){return p.id=="@camera_midpoint_fade";});
        const auto& blend=pipeline_info.state.blending;
        const auto& textures=regs.texturing.main_config;
        using BF=Pica::FramebufferRegs::BlendFactor;
        using BE=Pica::FramebufferRegs::BlendEquation;
        const unsigned texture_mask=unsigned(textures.texture0_enable)|
            (unsigned(textures.texture1_enable)<<1)|(unsigned(textures.texture2_enable)<<2);
        const bool opaque=blend.blend_enable && blend.src_color_blend_factor==BF::One &&
            blend.dst_color_blend_factor==BF::Zero && blend.color_blend_eq==BE::Add &&
            blend.color_write_mask==15;
        const bool scene_copy=MM3D::CameraMidpoint::NormalizedSceneCopy(
            MM3D::ScreenShrink::native_normalized.load(),
            midpoint_scene_copies.Contains(frame,regs.texturing.texture0.GetPhysicalAddress()),
            ortho,u32(regs.pipeline.num_vertices),
            texture_mask,opaque,
            pipeline_info.state.depth_stencil.stencil_test_enable);
        const auto color_address=regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress();
        const bool scene_clear=MM3D::CameraMidpoint::NormalizedSceneClear(
            MM3D::ScreenShrink::native_normalized.load(),
            midpoint_scene_copies.Contains(frame,color_address),
            ortho,u32(regs.pipeline.num_vertices),texture_mask,opaque,
            pipeline_info.state.depth_stencil.stencil_test_enable);
        if(midpoint_scene_clear.Observe(frame,color_address,scene_clear,scene_copy && succeeded))
            reject_midpoint_hud(5);
        // Accepted screen-space effects can cover an interpolated scene with a
        // native scene copy without producing a HUD rejection. Sample those
        // draws during an explicitly requested timing trace as well.
        if(MM3D::InputLatency::Active() && frame%10==0 && ortho &&
           regs.pipeline.num_vertices==4) {
            const auto& u=pica.vs_setup.uniforms.f;
            LOG_INFO(Render_Vulkan,
                "Interpolation screen quad: frame={} texture={:08x} framebuffer={:08x} "
                "scene_source={} blend={} src={} dst={} equation={} mask={} "
                "textures={} model={},{},{},{};{},{},{},{};{},{},{},{} color={},{},{},{}",
                frame,regs.texturing.texture0.GetPhysicalAddress(),
                regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress(),
                midpoint_scene_copies.Contains(frame,regs.texturing.texture0.GetPhysicalAddress()),
                unsigned(blend.blend_enable),unsigned(blend.src_color_blend_factor.Value()),
                unsigned(blend.dst_color_blend_factor.Value()),unsigned(blend.color_blend_eq.Value()),
                unsigned(blend.color_write_mask),
                unsigned(textures.texture0_enable)|(unsigned(textures.texture1_enable)<<1)|
                    (unsigned(textures.texture2_enable)<<2),
                u[20].x.ToFloat32(),u[20].y.ToFloat32(),u[20].z.ToFloat32(),u[20].w.ToFloat32(),
                u[21].x.ToFloat32(),u[21].y.ToFloat32(),u[21].z.ToFloat32(),u[21].w.ToFloat32(),
                u[22].x.ToFloat32(),u[22].y.ToFloat32(),u[22].z.ToFloat32(),u[22].w.ToFloat32(),
                u[8].x.ToFloat32(),u[8].y.ToFloat32(),u[8].z.ToFloat32(),u[8].w.ToFloat32());
        }
        // The compositor already fits the private world/HUD into the shrinking
        // rectangle. Copying the native scene here would cover that midpoint.
        // Preserve native screen-space draw order. Splitting bars/fades into
        // a final layer would place them above HUD drawn later by the game,
        // changing occlusion on every midpoint frame.
        // The lens mask can follow translucent sky draws. It still precedes
        // opaque scenery; sky alone must not disqualify the stencil pass.
        const bool lens_mask=ortho && regs.pipeline.num_vertices==18 &&
            pipeline_info.state.depth_stencil.stencil_test_enable &&
            midpoint_opaque_world_frame!=frame && blend.blend_enable &&
            blend.src_color_blend_factor==BF::One && blend.dst_color_blend_factor==BF::One &&
            blend.color_blend_eq==BE::Add && blend.color_write_mask==15 &&
            textures.texture0_enable && !textures.texture1_enable && !textures.texture2_enable;
        if(lens_mask && replay_geometry && succeeded)midpoint_stencil_frame=frame;
        if((ortho || layout || fade || midpoint_title_logo) && !scene_copy && !scene_clear) {
            if(!replay_geometry || (pipeline_info.state.depth_stencil.stencil_test_enable &&
                              midpoint_stencil_frame!=frame && !midpoint_title_logo)) {
                reject_midpoint_hud(1);
            } else {
                MM3D::Profiles::Profile hud;
                // Background layouts and the death darkener precede opaque
                // world geometry, sometimes after perspective sky draws. Keep
                // them in the scene so later geometry covers them in native order.
                hud.id=midpoint_title_logo || (midpoint_opaque_world_frame==frame && midpoint_stencil_frame!=frame)?
                    "@camera_midpoint_hud":"@camera_midpoint";
                hud.presentation=true;hud.source_top=true;
                mm3d_draw_profiles.push_back(hud);
            }
        } else if(!world && !layout) {
            // Unknown top-screen draws must not silently vanish on alternate frames.
            reject_midpoint_hud(2);
        }
    }
    const float native_clip_x=vs_data.clip_scale_x,native_clip_y=vs_data.clip_scale_y;
    for (const auto& match : mm3d_draw_profiles) {
        if (!match.presentation) continue;
        // Initial atlas presentation supports the full, rotated 320x240 lower-screen viewport.
        // Do not accidentally replay top-screen uses of the same HUD atlas.
        if(match.atlas_selection && (viewport.width<=0 || viewport.height<=0 ||
            s64(viewport.width)*4!=s64(viewport.height)*3)) {++MM3D::prompt_unsupported;continue;}
        using BF = Pica::FramebufferRegs::BlendFactor;
        using BE = Pica::FramebufferRegs::BlendEquation;
        const auto& blend = pipeline_info.state.blending;
        const bool top_ui_source = match.source_top;
        const bool minimap=match.id=="@minimap";
        const bool scene_background=match.id=="@camera_midpoint";
        const bool midpoint_ui=scene_background || match.id=="@camera_midpoint_hud";
        MM3D::InterpolationPerformance::Scope performance_overlay{MM3D::InterpolationPerformance::Part::Replay,midpoint_ui};
        const bool stencil_scene=scene_background && midpoint_stencil_frame==MM3D::guest_frames.load();
        // Sun glare uses source-alpha additive color. Keep it in the ordered
        // HUD replay, with emission but no additional coverage of the world.
        const auto& texture_units=regs.texturing.main_config;
        const auto& projection_row=pica.vs_setup.uniforms.f[3];
        const bool bloom_additive=blend.src_color_blend_factor==BF::One &&
            MM3D::BloomSampling::MidpointComposite(pica.vs_setup.GetProgramCodeHash(),
                pica.vs_setup.GetSwizzleDataHash(),regs.pipeline.num_vertices,
                unsigned(texture_units.texture0_enable)|(unsigned(texture_units.texture1_enable)<<1)|
                    (unsigned(texture_units.texture2_enable)<<2),
                projection_row.x.ToFloat32()==0 && projection_row.y.ToFloat32()==0 &&
                projection_row.z.ToFloat32()==0 && projection_row.w.ToFloat32()==1);
        const bool additive=midpoint_ui && blend.blend_enable &&
            (blend.src_color_blend_factor==BF::SourceAlpha || bloom_additive) &&
            blend.dst_color_blend_factor==BF::One && blend.color_blend_eq==BE::Add;
        // Bounded, read-only vertex capture for the verified glare candidate.
        // Matrix captures show fixed transforms; motion lives in these buffers.
        if(additive && accelerate && MM3D::InputLatency::Active() &&
           regs.pipeline.num_vertices==72 &&
           pica.vs_setup.GetProgramCodeHash()==0x89b1ab98da3d9241ULL &&
           pica.vs_setup.GetSwizzleDataHash()==0x6f0b398b1fc186a9ULL) {
            const auto session=MM3D::InputLatency::session.load();
            if(glare_trace_session!=session) {
                glare_trace_session=session;glare_trace_samples=0;glare_trace_frame=0;
            }
            const auto frame=MM3D::guest_frames.load();
            if(glare_trace_frame!=frame && glare_trace_samples<120) {
                glare_trace_frame=frame;++glare_trace_samples;
                const auto& attributes=regs.pipeline.vertex_attributes;
                std::string formats;
                for(unsigned i=0;i<12;++i)
                    formats+=fmt::format(" {}:{}/{}/{}",i,unsigned(attributes.GetFormat(i)),
                        attributes.GetNumElements(i),regs.vs.GetRegisterForAttribute(i));
                unsigned slot=0;
                for(const auto& loader:attributes.attribute_loaders) {
                    const auto current_slot=slot++;
                    if(!loader.component_count || !loader.byte_count)continue;
                    const u64 address=u64(attributes.GetPhysicalBaseAddress())+loader.data_offset+
                        u64(vertex_info.vs_input_index_min)*loader.byte_count;
                    const u64 size=u64(vertex_info.vs_input_index_max-vertex_info.vs_input_index_min+1)*loader.byte_count;
                    if(address>0xffffffff || size>4096 || !size)continue;
                    const auto source=memory.GetPhysicalRef(static_cast<PAddr>(address));
                    if(source.GetSize()<size)continue;
                    std::string bytes,components;
                    for(unsigned i=0;i<loader.component_count && i<12;++i)
                        components+=fmt::format(" {},",loader.GetComponent(i));
                    for(u64 i=0;i<size;++i)bytes+=fmt::format("{:02x}",source.GetPtr()[i]);
                    LOG_INFO(Render_Vulkan,
                        "Glare vertices: session={} frame={} loader={} first={} last={} stride={} components={} formats={} bytes={}",
                        session,frame,current_slot,vertex_info.vs_input_index_min,vertex_info.vs_input_index_max,
                        u32(loader.byte_count),components,formats,bytes);
                }
            }
        }
        const bool supported = !shadow_rendering &&
            (minimap || top_ui_source || (!pipeline_info.state.depth_stencil.stencil_test_enable &&
             (!pipeline_info.state.depth_stencil.depth_test_enable ||
              pipeline_info.state.depth_stencil.depth_compare_op == Pica::FramebufferRegs::CompareFunc::Always))) &&
            (stencil_scene || additive || (minimap && !blend.blend_enable) || (blend.blend_enable && blend.src_color_blend_factor == BF::SourceAlpha &&
            blend.dst_color_blend_factor == BF::OneMinusSourceAlpha && blend.color_blend_eq == BE::Add)) &&
            (stencil_scene || (blend.color_write_mask & 7) == 7) &&
            viewport.x >= 0 && viewport.y >= 0 && viewport.width > 0 && viewport.height > 0;
        if (supported) {
            if (!prompt_layers.contains(match.id) && prompt_layers.size() >= 16) {
                MM3D::prompt_unsupported.fetch_add(1);
                if(midpoint_ui)reject_midpoint_hud(3);
                continue;
            }
            auto& prompt_layer = prompt_layers[match.id];
            if (!prompt_layer) prompt_layer = std::make_unique<MM3D::PromptLayer>(instance, scheduler, renderpass_cache,midpoint_ui);
            if(scene_background && midpoint_target_frame!=MM3D::guest_frames.load()) {
                prompt_layer->NextFrame();midpoint_target_frame=MM3D::guest_frames.load();
                midpoint_frame_interpolated=false;
            }
            if(scene_background) {
                prompt_layer->profile.id="@camera_midpoint";
                prompt_layer->profile.source_top=true;
                prompt_layer->profile.scale=.42f;
                prompt_layer->profile.x=.26f;prompt_layer->profile.y=-.26f;
            } else {
                prompt_layer->profile = match;
            }
            // Replay copies are placed with their own preserved source aspect.
            // Applying the scene correction here too would squeeze them twice.
            const float copy_clip_x=midpoint_ui?native_clip_x:1.f;
            const float copy_clip_y=midpoint_ui?native_clip_y:1.f;
            if(vs_data.clip_scale_x!=copy_clip_x || vs_data.clip_scale_y!=copy_clip_y) {
                vs_data.clip_scale_x=copy_clip_x; vs_data.clip_scale_y=copy_clip_y; vs_data_dirty=true;
            }
            atlas_uniforms={};
            if(match.atlas_selection) MM3D::Atlas::Build(match,atlas_uniforms);
            fs_data_dirty=true; UploadUniforms(accelerate);
            pipeline_cache.UseFragmentShader(regs,user_config,match.atlas_selection);
            const unsigned w = std::max<unsigned>(draw_rect.right, viewport.x + viewport.width);
            const unsigned h = std::max<unsigned>(draw_rect.top, viewport.y + viewport.height);
            const bool title_depth=midpoint_ui && midpoint_title_logo;
            const auto depth_compare=regs.framebuffer.output_merger.depth_test_func.Value();
            using CF=Pica::FramebufferRegs::CompareFunc;
            const bool reverse_depth=depth_compare==CF::GreaterThan || depth_compare==CF::GreaterThanOrEqual;
            const bool reset_title_depth=title_depth && midpoint_title_depth_frame!=MM3D::guest_frames.load();
            if (prompt_layer->Begin(w, h,reverse_depth?0.f:1.f,reset_title_depth)) {
                if(title_depth)midpoint_title_depth_frame=MM3D::guest_frames.load();
                const auto saved = pipeline_info;
                pipeline_info.dynamic_info.viewport = {viewport.x,viewport.y,
                    viewport.x+viewport.width,viewport.y+viewport.height};
                pipeline_info.dynamic_info.scissor = draw_rect;
                pipeline_info.state.attachments.color = VideoCore::PixelFormat::RGBA8;
                pipeline_info.state.attachments.depth = midpoint_ui?VideoCore::PixelFormat::D24S8:VideoCore::PixelFormat::Invalid;
                if(!stencil_scene && !title_depth)pipeline_info.state.depth_stencil.value = 0;
                auto& copy_blend = pipeline_info.state.blending;
                if(!stencil_scene)copy_blend.color_write_mask = 15;
                // Alpha stores accumulated coverage, not brightness. An additive
                // flare contributes RGB but preserves prior coverage. Later HUD
                // or bars still attenuate it in the game's submission order.
                if(!stencil_scene) {
                    copy_blend.src_alpha_blend_factor.Assign(additive?BF::Zero:BF::One);
                    copy_blend.dst_alpha_blend_factor.Assign(additive?BF::One:BF::OneMinusSourceAlpha);
                    copy_blend.alpha_blend_eq.Assign(BE::Add);
                }
                // Block on this small diagnostic pipeline rather than publishing a skipped draw.
                pipeline_cache.BindPipeline(pipeline_info, true);
                const auto native_position=binding_offsets[0];
                if(additive && glare_vertex_offset && midpoint_frame_interpolated &&
                   !MM3D::CameraMidpoint::native_preview.load())binding_offsets[0]=*glare_vertex_offset;
                draw_batch();
                binding_offsets[0]=native_position;
                prompt_layer->x = viewport.x; prompt_layer->y = viewport.y;
                prompt_layer->viewport_width = viewport.width;
                prompt_layer->viewport_height = viewport.height;
                prompt_layer->End(!midpoint_ui);
                pipeline_info = saved;
                if(minimap) ++MM3D::Minimap::draws;
                MM3D::prompt_replays.fetch_add(1);
            } else {
                MM3D::prompt_unsupported.fetch_add(1);
                if(midpoint_ui)reject_midpoint_hud(4);
            }
        } else {
            MM3D::prompt_unsupported.fetch_add(1);
            if(midpoint_ui)reject_midpoint_hud(5);
        }
    }
#endif
    vertex_batch.clear();
    return succeeded;
}

void RasterizerVulkan::SyncTextureUnits(const Framebuffer* framebuffer) {
    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
#ifdef ENABLE_MM3D_PRESENTATION
    pause_draw={};
#endif
    const auto pica_textures = regs.texturing.GetTextures();
    const bool use_cube_heap =
        pica_textures[0].enabled && pica_textures[0].config.type == TextureType::ShadowCube;
    const auto texture_set = pipeline_cache.Acquire(use_cube_heap ? DescriptorHeapType::Texture
                                                                  : DescriptorHeapType::Texture);

    for (u32 texture_index = 0; texture_index < pica_textures.size(); ++texture_index) {
        const auto& texture = pica_textures[texture_index];

        // If the texture unit is disabled bind a null surface to it
        if (!texture.enabled) {
            switch (texture.config.type.Value()) {
            case TextureType::TextureCube:
            case TextureType::ShadowCube: {
                Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_CUBE_ID);
                const Sampler& null_sampler = res_cache.GetSampler(VideoCore::NULL_SURFACE_CUBE_ID);
                update_queue.AddImageSampler(texture_set, texture_index, 0,
                                             null_surface.ImageView(), null_sampler.Handle());
                break;
            }
            default: {
                Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
                const Sampler& null_sampler = res_cache.GetSampler(VideoCore::NULL_SURFACE_ID);
                update_queue.AddImageSampler(texture_set, texture_index, 0,
                                             null_surface.ImageView(), null_sampler.Handle());
                break;
            }
            }
            continue;
        }

        // Handle special tex0 configurations
        if (texture_index == 0) {
            switch (texture.config.type.Value()) {
            case TextureType::Shadow2D: {
                Surface& surface = res_cache.GetTextureSurface(texture);
                Sampler& sampler = res_cache.GetSampler(texture.config);
                surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
                update_queue.AddImageSampler(texture_set, texture_index, 0, surface.StorageView(),
                                             sampler.Handle());
                continue;
            }
            case TextureType::ShadowCube: {
                BindShadowCube(texture, texture_set);
                continue;
            }
            case TextureType::TextureCube: {
                BindTextureCube(texture, texture_set);
                continue;
            }
            default:
                break;
            }
        }

        // Bind the texture provided by the rasterizer cache
        Surface& surface = res_cache.GetTextureSurface(texture);
        Sampler& sampler = res_cache.GetSampler(texture.config);
        const vk::ImageView color_view = framebuffer->ImageView(SurfaceType::Color);
        const bool is_feedback_loop = color_view == surface.FramebufferView();
        vk::ImageView texture_view =
            is_feedback_loop ? surface.CopyImageView() : surface.ImageView();
#ifdef ENABLE_MM3D_PRESENTATION
        if (MM3D::BloomSampling::enabled.load() && surface.GetResScale() > 1 &&
            !surface.IsCustom() && texture.config.type == TextureType::Texture2D &&
            MM3D::BloomSampling::Matches({pica.vs_setup.GetProgramCodeHash(),
                pica.vs_setup.GetSwizzleDataHash(), unsigned(regs.pipeline.num_vertices),
                texture.config.width, texture.config.height, unsigned(texture.format),
                u32(regs.rasterizer.viewport_size_x), u32(regs.rasterizer.viewport_size_y),
                regs.framebuffer.output_merger.alphablend_enable != 0, is_feedback_loop})) {
            const auto native_view = surface.NativeSampleView();
            if (native_view != texture_view) {
                texture_view = native_view;
                MM3D::BloomSampling::bindings.fetch_add(1);
            }
        }
#endif
#ifdef ENABLE_MM3D_PRESENTATION
        if(MM3D::PauseCapture::enabled.load()&&!is_feedback_loop&&texture.config.width==512&&
           texture.config.height==256&&texture.format==Pica::TexturingRegs::TextureFormat::RGB565&&
           texture.config.type==TextureType::Texture2D) {
            for(unsigned eye=0;eye<2;++eye) {
                if(!pause_textures[eye]||pause_textures[eye]!=texture.config.GetPhysicalAddress())continue;
                const auto& image=pause_images[eye]?pause_images[eye]:pause_images[0];
                if(image){texture_view=image->ImageView();pause_draw[texture_index]=int(pause_orientation[eye]);}
                break;
            }
        }
#endif
#ifdef ENABLE_MM3D_PRESENTATION
        const auto texture_key = MM3D::Textures::Key(texture.config.GetPhysicalAddress(),
            texture.config.width, texture.config.height, unsigned(texture.format));
#ifndef ENABLE_MM3D_GAME_FOLDER
        // Unlike content identities, this session-only probe follows animated buffers.
        // Include feedback reads; skip their consumer draw without modifying the image.
        if (MM3D::Textures::hide_by_address.load() && MM3D::Textures::Matches(texture_key))
            MM3D::Textures::hide_current_draw = true;
#endif
        const u32 match_width = texture.config.width;
        const u32 match_height = texture.config.height;
        if (!is_feedback_loop && !surface.IsCustom() &&
            (MM3D::Minimap::Candidate(match_width,match_height) || MM3D::Textures::WantsDetail(texture_key) || MM3D::Profiles::Candidate(match_width, match_height, unsigned(texture.format)) ||
             MM3D::Notebook::ExclusionCandidate(match_width, match_height, unsigned(texture.format)))) {
            const u64 size = u64(match_width) * match_height * surface.GetFormatBpp() / 8;
            if (match_width % 8 == 0 && match_height % 8 == 0 && size > 0 && size <= 4 * 1024 * 1024) {
                if (surface.mm3d_hash_tick != surface.ModificationTick() ||
                    surface.mm3d_hash_key != texture_key) {
                    renderpass_cache.EndRendering();
                    MM3D::InterpolationPerformance::ReadbackScope readback{
                        "presentation texture identity",texture.config.GetPhysicalAddress(),size};
                    res_cache.FlushRegion(texture.config.GetPhysicalAddress(), static_cast<u32>(size));
                    scheduler.Finish();
                    const auto ref = memory.GetPhysicalRef(texture.config.GetPhysicalAddress());
                    surface.mm3d_hash = ref && ref.GetSize() >= size
                        ? MM3D::Textures::ContentHash(ref.GetReadBytes<u8>(size)) : std::string{};
                    surface.mm3d_hash_tick = surface.ModificationTick();
                    surface.mm3d_hash_key = texture_key;
                }
                const auto map_viewport=regs.rasterizer.GetViewportRect();
                if(MM3D::Minimap::Candidate(match_width,match_height) &&
                   map_viewport.GetWidth()==240 && map_viewport.GetHeight()==320 &&
                   surface.mm3d_hash==MM3D::Minimap::RadarHash) {
                    MM3D::Minimap::marker_draw=true;
                    MM3D::Minimap::marker_seen=true;
                }
                const auto context = fmt::format(
                    "unit={} shader={:016x} swizzle={:016x} vertices={} blend={} viewport_raw={},{}",
                    texture_index, pica.vs_setup.GetProgramCodeHash(), pica.vs_setup.GetSwizzleDataHash(),
                    regs.pipeline.num_vertices, unsigned(regs.framebuffer.output_merger.alphablend_enable),
                    u32(regs.rasterizer.viewport_size_x), u32(regs.rasterizer.viewport_size_y));
                const MM3D::Profiles::Rule identity{match_width,match_height,unsigned(texture.format),surface.mm3d_hash,context};
                MM3D::Profiles::Observe(texture_key,identity);
                std::array<float,7> fade_alpha{};
                const auto fade_stages=regs.texturing.GetTevStages();
                for(size_t stage=0;stage<fade_stages.size();++stage)
                    fade_alpha[stage]=float(fade_stages[stage].const_a.Value())/255.f;
                fade_alpha[6]=float((regs.framebuffer.output_merger.blend_const.raw>>24)&255)/255.f;
                MM3D::Profiles::ObserveFade(identity,fade_alpha);
                panel_excluded |= MM3D::Notebook::Excluded(identity);
                panel_draw_identities.push_back(identity);
                if(!surface.mm3d_hash.empty() && MM3D::Textures::WantsDetail(texture_key,surface.mm3d_hash)) {
                    renderpass_cache.EndRendering();
                    MM3D::Textures::Capture(instance,scheduler,surface.Image(Type::Base),
                        instance.GetTraits(surface.pixel_format).native,surface.GetWidth(),surface.GetHeight(),
                        texture_key,vk::ImageLayout::eGeneral,true);
                }
                MM3D::Textures::hide_current_draw |= MM3D::Profiles::Hidden(identity);
                for (const auto& match : MM3D::Profiles::Match(identity))
                    if (std::none_of(mm3d_draw_profiles.begin(),mm3d_draw_profiles.end(),
                        [&](const auto& p){return p.id==match.id;})) {
                        mm3d_draw_profiles.push_back(match);
                        if (match.presentation) MM3D::prompt_matches.fetch_add(1);
                    }
            }
        }
        if (MM3D::Textures::RecordingKey(texture_key)) {
            std::span<const u8> bytes;
            const u32 width = texture.config.width;
            const u32 height = texture.config.height;
            const u64 size = u64(width) * height * surface.GetFormatBpp() / 8;
            if (MM3D::Textures::NeedsRecordingHash(texture_key) && !is_feedback_loop &&
                width % 8 == 0 && height % 8 == 0 && size > 0 && size <= 4 * 1024 * 1024) {
                MM3D::InterpolationPerformance::ReadbackScope readback{
                    "diagnostic texture recording",texture.config.GetPhysicalAddress(),size};
                // Read current guest encoded bytes, not a scaled/replaced GPU thumbnail.
                renderpass_cache.EndRendering();
                res_cache.FlushRegion(texture.config.GetPhysicalAddress(), static_cast<u32>(size));
                scheduler.Finish();
                const auto ref = memory.GetPhysicalRef(texture.config.GetPhysicalAddress());
                if (ref && ref.GetSize() >= size) bytes = ref.GetReadBytes<u8>(size);
            }
            const auto& fb = regs.framebuffer.framebuffer;
            MM3D::Textures::RecordBinding(texture_key, bytes, fmt::format(
                "size={}x{} format={} unit={} shader={:016x} swizzle={:016x} vertices={} "
                "target={:08x} depth={:08x} blend={} viewport_raw={},{} feedback={} "
                "surface={}x{} scale={} address_hide={}",
                width, height, unsigned(texture.format), texture_index,
                pica.vs_setup.GetProgramCodeHash(), pica.vs_setup.GetSwizzleDataHash(),
                regs.pipeline.num_vertices, fb.GetColorBufferPhysicalAddress(), fb.GetDepthBufferPhysicalAddress(),
                unsigned(regs.framebuffer.output_merger.alphablend_enable),
                u32(regs.rasterizer.viewport_size_x), u32(regs.rasterizer.viewport_size_y), is_feedback_loop,
                surface.GetWidth(), surface.GetHeight(), surface.GetResScale(),
                MM3D::Textures::hide_by_address.load() && MM3D::Textures::Matches(texture_key)));
        }
        if (!is_feedback_loop && surface.Aspect() == vk::ImageAspectFlagBits::eColor &&
            MM3D::Textures::Wants(texture_key)) {
            renderpass_cache.EndRendering();
            MM3D::Textures::Capture(instance, scheduler, surface.Image(Type::Base),
                instance.GetTraits(surface.pixel_format).native,
                surface.GetWidth(), surface.GetHeight(), texture_key);
        }
#endif
        update_queue.AddImageSampler(texture_set, texture_index, 0, texture_view, sampler.Handle());
    }
}

void RasterizerVulkan::SyncUtilityTextures(const Framebuffer* framebuffer) {
    const bool shadow_writing = regs.framebuffer.IsShadowRendering();
    const bool shadow_reading = regs.lighting.config0.enable_shadow;
    const auto utility_set = pipeline_cache.Acquire(DescriptorHeapType::Utility);

    // Reading and writing are mutually exclusive
    assert(!(shadow_writing && shadow_reading));

    if (shadow_writing) {
        update_queue.AddStorageImage(utility_set, 0, framebuffer->ImageView(SurfaceType::Color));
    } else if (shadow_reading) {
        const u32 shadow_texture_unit = regs.lighting.config0.shadow_selector.Value();
        const auto shadow_texture = regs.texturing.GetTextures()[shadow_texture_unit];
        Surface& shadow_surface = res_cache.GetTextureSurface(shadow_texture);
        update_queue.AddStorageImage(utility_set, 0, shadow_surface.StorageView());
    } else {
        Surface& null_surface = res_cache.GetSurface(VideoCore::NULL_SURFACE_ID);
        update_queue.AddStorageImage(utility_set, 0, null_surface.StorageView());
    }
}

void RasterizerVulkan::BindShadowCube(const Pica::TexturingRegs::FullTextureConfig& texture,
                                      vk::DescriptorSet texture_set) {
    using CubeFace = Pica::TexturingRegs::CubeFace;
    auto info = Pica::Texture::TextureInfo::FromPicaRegister(texture.config, texture.format);
    constexpr std::array faces = {
        CubeFace::PositiveX, CubeFace::NegativeX, CubeFace::PositiveY,
        CubeFace::NegativeY, CubeFace::PositiveZ, CubeFace::NegativeZ,
    };

    Sampler& sampler = res_cache.GetSampler(texture.config);

    for (CubeFace face : faces) {
        const u32 binding = static_cast<u32>(face);
        info.physical_address = regs.texturing.GetCubePhysicalAddress(face);

        const VideoCore::SurfaceId surface_id = res_cache.GetTextureSurface(info);
        Surface& surface = res_cache.GetSurface(surface_id);
        surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
        update_queue.AddImageSampler(texture_set, 0, binding, surface.StorageView(),
                                     sampler.Handle());
    }
}

void RasterizerVulkan::BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture,
                                       vk::DescriptorSet texture_set) {
    using CubeFace = Pica::TexturingRegs::CubeFace;
    const VideoCore::TextureCubeConfig config = {
        .px = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX),
        .nx = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX),
        .py = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY),
        .ny = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY),
        .pz = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ),
        .nz = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ),
        .width = texture.config.width,
        .levels = texture.config.lod.max_level + 1,
        .format = texture.format,
    };

    Surface& surface = res_cache.GetTextureCube(config);
    Sampler& sampler = res_cache.GetSampler(texture.config);
    update_queue.AddImageSampler(texture_set, 0, 0, surface.ImageView(), sampler.Handle());
}

#ifdef ENABLE_MM3D_PRESENTATION
void RasterizerVulkan::EndGuestFrame() {
    if (!visibility_depth_refresh_enabled) return;
    if (!visibility_depth || !visibility_depth->SourceAddress()) return;
    if (!visibility_depth_enabled || !MM3D::CameraMidpoint::enabled.load()) {
        ResetVisibilityDepth();
        return;
    }
    MM3D::InterpolationPerformance::Scope timing{
        MM3D::InterpolationPerformance::Part::VisibilityRefresh};
    struct GuestMemory {
        Memory::MemorySystem& memory;
        std::optional<u32> Read(u32 at) { return memory.Read32OrNullopt(at); }
    } guest{memory};
    const auto context = MM3D::VisibilityDepth::LiveContext(
        guest, MM3D::guest_frames.load(), MM3D::CameraMidpoint::generation.load());
    if (!context || !MM3D::VisibilityDepth::CodeMatches(guest)) {
        ResetVisibilityDepth();
        return;
    }
    // Inspect only the existing GPU-owned source. Never load/create a surface,
    // clean guest memory, or retain a raw surface pointer between guest frames.
    auto* surface = res_cache.GetDirtySurface(*visibility_depth->SourceAddress(), 4);
    if (!surface) return;
    visibility_depth->Refresh(runtime, *surface, *context, MM3D::InterpolationPerformance::Now());
}

std::optional<u32> RasterizerVulkan::ReadVisibilityDepth(u32 address, const MM3D::VisibilityDepth::Context& context) {
    if (!visibility_depth_enabled || !MM3D::CameraMidpoint::enabled.load()) {
        ResetVisibilityDepth();
        return {};
    }
    auto* surface = res_cache.GetDirtySurface(address, 4);
    if (!surface) { // Clean/CPU-owned memory uses the exact ordinary read.
        MM3D::InterpolationPerformance::RecordVisibilityEvent(
            MM3D::InterpolationPerformance::VisibilityEvent::CleanMemory);
        return {};
    }
    if (!visibility_depth) visibility_depth = std::make_unique<VisibilityDepthCache>(instance, scheduler);
    return visibility_depth->Read(runtime, *surface, address, context, MM3D::InterpolationPerformance::Now());
}
void RasterizerVulkan::ResetVisibilityDepth() {
    if (visibility_depth) visibility_depth->Reset();
}
void RasterizerVulkan::ResetPause() {
    pause_textures={};pause_orientation={};pause_draw={};
    MM3D::PauseCapture::captured_scale=0;MM3D::PauseCapture::bound_textures=0;
}
void RasterizerVulkan::CapturePause(u32 address,unsigned eye) {
    if(eye>1)return;
    if(eye==0) {
        ResetPause();
        // Retire the previous frozen images only after all queued draws finish.
        runtime.Finish();pause_images={};
    }
    VideoCore::SurfaceParams params;
    params.addr=address;params.width=params.stride=240;params.height=400;
    params.pixel_format=VideoCore::PixelFormat::RGB8;params.is_tiled=false;params.UpdateParams();
    const auto [id,rect]=res_cache.GetSurfaceSubRect(params,VideoCore::ScaleMatch::Ignore,true);
    if(!id)return;
    Surface& source=res_cache.GetSurface(id);
    const auto scale=source.GetResScale();
    if(scale<=1||source.IsCustom()||rect.GetWidth()!=240*scale||rect.GetHeight()!=400*scale)return;
    params.addr=0;params.res_scale=scale;params.UpdateParams();
    auto image=std::make_unique<Surface>(runtime,params);
    const VideoCore::TextureCopy copy{
        .src_level=source.LevelOf(address),.dst_level=0,
        .src_offset={rect.left,rect.bottom},.dst_offset={0,0},
        .extent={240*scale,400*scale},
    };
    if(!runtime.CopyTextures(source,*image,copy)){runtime.Finish();return;}
    pause_images[eye]=std::move(image);MM3D::PauseCapture::captured_scale=scale;
}
void RasterizerVulkan::BindPause(u32 texture,unsigned eye,const MM3D::PauseCapture::Probes& probes) {
    if(eye>1||!(pause_images[eye]||pause_images[0]))return;
    const auto data=memory.GetPhysicalRef(texture);
    if(!data||data.GetSize()<0x40000)return;
    const auto bytes=data.GetReadBytes<u8>(0x40000);
    const auto orientation=MM3D::PauseCapture::Orientation([&](u32 off)->std::optional<u32>{
        return u32(bytes[off])|(u32(bytes[off+1])<<8);
    },probes);
    if(!orientation)return;
    pause_textures[eye]=texture;pause_orientation[eye]=orientation;
    MM3D::PauseCapture::bound_textures=unsigned(pause_textures[0]!=0)+unsigned(pause_textures[1]!=0);
}
#endif

void RasterizerVulkan::FlushAll() {
#ifdef ENABLE_MM3D_PRESENTATION
    ResetVisibilityDepth();
    MM3D::InterpolationPerformance::ReadbackScope readback{"flush all"};
#endif
    res_cache.FlushAll();
}

void RasterizerVulkan::FlushRegion(PAddr addr, u32 size) {
#ifdef ENABLE_MM3D_PRESENTATION
    MM3D::InterpolationPerformance::ReadbackScope readback{"external memory read",addr,size};
#endif
    res_cache.FlushRegion(addr, size);
}

void RasterizerVulkan::InvalidateRegion(PAddr addr, u32 size) {
#ifdef ENABLE_MM3D_PRESENTATION
    if (visibility_depth) visibility_depth->Invalidate(addr, size);
    MM3D::InterpolationPerformance::ReadbackScope readback{"memory invalidation",addr,size};
#endif
    res_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::FlushAndInvalidateRegion(PAddr addr, u32 size) {
#ifdef ENABLE_MM3D_PRESENTATION
    if (visibility_depth) visibility_depth->Invalidate(addr, size);
    MM3D::InterpolationPerformance::ReadbackScope readback{"external memory read/write",addr,size};
#endif
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::ClearAll(bool flush) {
#ifdef ENABLE_MM3D_PRESENTATION
    ResetVisibilityDepth();
    ResetPause();
    midpoint_history.Reset();forest_particle_history.Reset();link_history.Reset();song_history.Reset();song_overrides.reset();link_frame.reset();link_overrides={};
    midpoint_scene_clear.Reset();
    midpoint_target_frame.reset();midpoint_world_frame.reset();midpoint_opaque_world_frame.reset();midpoint_stencil_frame.reset();midpoint_fade_frame.reset();midpoint_title_depth_frame.reset();
    for(const auto id : {"@camera_midpoint","@camera_midpoint_fade","@camera_midpoint_hud"})
        if(const auto it=prompt_layers.find(id);it!=prompt_layers.end())it->second->NextFrame();
#endif
    res_cache.ClearAll(flush);
}

bool RasterizerVulkan::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    const bool result=res_cache.AccelerateDisplayTransfer(config);
#ifdef ENABLE_MM3D_PRESENTATION
    if(MM3D::ScreenShrink::native_normalized.load() &&
       MM3D::ScreenShrink::presentation_scale.load()<1.f) {
        const auto now=MM3D::InputLatency::Now();
        auto& stamp=midpoint_transfer_logged[0];
        if(now-stamp>=1000000) {
            stamp=now;
            LOG_INFO(Render_Vulkan,"Interpolation shrink DisplayTransfer: success={} input={:08x} output={:08x} frame={} source_frame={} source={:08x}",
                result,config.GetPhysicalInputAddress(),config.GetPhysicalOutputAddress(),
                MM3D::guest_frames.load(),midpoint_scene_copies.frame,midpoint_scene_copies.addresses[0]);
        }
    }
    midpoint_scene_copies.Transfer(MM3D::guest_frames.load(),
        result?config.GetPhysicalInputAddress():0,config.GetPhysicalOutputAddress());
#endif
    return result;
}

bool RasterizerVulkan::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    const bool result=res_cache.AccelerateTextureCopy(config);
#ifdef ENABLE_MM3D_PRESENTATION
    if(MM3D::ScreenShrink::native_normalized.load() &&
       MM3D::ScreenShrink::presentation_scale.load()<1.f) {
        const auto now=MM3D::InputLatency::Now();
        auto& stamp=midpoint_transfer_logged[1];
        if(now-stamp>=1000000) {
            stamp=now;
            LOG_INFO(Render_Vulkan,"Interpolation shrink TextureCopy: success={} input={:08x} output={:08x} frame={} source_frame={} source={:08x}",
                result,config.GetPhysicalInputAddress(),config.GetPhysicalOutputAddress(),
                MM3D::guest_frames.load(),midpoint_scene_copies.frame,midpoint_scene_copies.addresses[0]);
        }
    }
    midpoint_scene_copies.Transfer(MM3D::guest_frames.load(),
        result?config.GetPhysicalInputAddress():0,config.GetPhysicalOutputAddress());
#endif
    return result;
}

bool RasterizerVulkan::AccelerateFill(const Pica::MemoryFillConfig& config) {
    return res_cache.AccelerateFill(config);
}

bool RasterizerVulkan::AccelerateDisplay(const Pica::FramebufferConfig& config,
                                         PAddr framebuffer_addr, u32 pixel_stride,
                                         ScreenInfo& screen_info) {
    if (framebuffer_addr == 0) [[unlikely]] {
        return false;
    }

    VideoCore::SurfaceParams src_params;
    src_params.addr = framebuffer_addr;
    src_params.width = std::min(config.width.Value(), pixel_stride);
    src_params.height = config.height;
    src_params.stride = pixel_stride;
    src_params.is_tiled = false;
    src_params.pixel_format = VideoCore::PixelFormatFromGPUPixelFormat(config.color_format);
    src_params.UpdateParams();

    const auto [src_surface_id, src_rect] =
        res_cache.GetSurfaceSubRect(src_params, VideoCore::ScaleMatch::Ignore, true);

    if (!src_surface_id) {
        return false;
    }

    Surface& src_surface = res_cache.GetSurface(src_surface_id);
    const u32 scaled_width = src_surface.GetScaledWidth();
    const u32 scaled_height = src_surface.GetScaledHeight();

    screen_info.texcoords = Common::Rectangle<f32>(
        (float)src_rect.bottom / (float)scaled_height, (float)src_rect.left / (float)scaled_width,
        (float)src_rect.top / (float)scaled_height, (float)src_rect.right / (float)scaled_width);

    screen_info.image_view = src_surface.ImageView();

    return true;
}

void RasterizerVulkan::MakeSoftwareVertexLayout() {
    constexpr std::array sizes = {4, 4, 2, 2, 2, 1, 4, 3};

    software_layout = VertexLayout{
        .binding_count = 1,
        .attribute_count = 8,
    };

    for (u32 i = 0; i < software_layout.binding_count; i++) {
        VertexBinding& binding = software_layout.bindings[i];
        binding.binding.Assign(i);
        binding.fixed.Assign(0);
        binding.byte_count.Assign(sizeof(HardwareVertex));
    }

    u32 offset = 0;
    for (u32 i = 0; i < 8; i++) {
        VertexAttribute& attribute = software_layout.attributes[i];
        attribute.binding.Assign(0);
        attribute.location.Assign(i);
        attribute.offset.Assign(offset);
        attribute.type.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
        attribute.size.Assign(sizes[i]);
        offset += sizes[i] * sizeof(float);
    }
}

void RasterizerVulkan::SyncAndUploadLUTsLF() {
    constexpr std::size_t max_size =
        sizeof(Common::Vec2f) * 256 * Pica::LightingRegs::NumLightingSampler +
        sizeof(Common::Vec2f) * 128; // fog

    if (!pica.lighting.lut_dirty && !pica.fog.lut_dirty) {
        return;
    }

    std::size_t bytes_used = 0;
    auto [buffer, offset, invalidate] = texture_lf_buffer.Map(max_size, sizeof(Common::Vec4f));

    if (invalidate) {
        pica.lighting.lut_dirty = pica.lighting.LutAllDirty;
        pica.fog.lut_dirty = true;
    }

    // Sync the lighting luts
    while (pica.lighting.lut_dirty) {
        u32 index = std::countr_zero(pica.lighting.lut_dirty);
        pica.lighting.lut_dirty &= ~(1 << index);

        Common::Vec2f* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
        const auto& source_lut = pica.lighting.luts[index];
        for (u32 i = 0; i < source_lut.size(); i++) {
            new_data[i] = {source_lut[i].ToFloat(), source_lut[i].DiffToFloat()};
        }
        fs_data.lighting_lut_offset[index / 4][index % 4] =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
        fs_data_dirty = true;
        bytes_used += source_lut.size() * sizeof(Common::Vec2f);
    }

    // Sync the fog lut
    if (pica.fog.lut_dirty) {
        Common::Vec2f* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
        for (u32 i = 0; i < pica.fog.lut.size(); i++) {
            new_data[i] = {pica.fog.lut[i].ToFloat(), pica.fog.lut[i].DiffToFloat()};
        }
        fs_data.fog_lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
        fs_data_dirty = true;
        bytes_used += pica.fog.lut.size() * sizeof(Common::Vec2f);
        pica.fog.lut_dirty = false;
    }

    texture_lf_buffer.Commit(static_cast<u32>(bytes_used));
}

void RasterizerVulkan::SyncAndUploadLUTs() {
    const auto& proctex = pica.proctex;
    constexpr std::size_t max_size =
        sizeof(Common::Vec2f) * 128 * 3 + // proctex: noise + color + alpha
        sizeof(Common::Vec4f) * 256 +     // proctex
        sizeof(Common::Vec4f) * 256;      // proctex diff

    if (!pica.proctex.lut_dirty) {
        return;
    }

    std::size_t bytes_used = 0;
    auto [buffer, offset, invalidate] = texture_buffer.Map(max_size, sizeof(Common::Vec4f));

    if (invalidate) {
        pica.proctex.table_dirty = pica.proctex.TableAllDirty;
    }

    // helper function for SyncProcTexNoiseLUT/ColorMap/AlphaMap
    const auto sync_proctex_value_lut =
        [&](const std::array<Pica::PicaCore::ProcTex::ValueEntry, 128>& lut, int& lut_offset) {
            Common::Vec2f* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
            for (u32 i = 0; i < lut.size(); i++) {
                new_data[i] = {lut[i].ToFloat(), lut[i].DiffToFloat()};
            }
            lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
            fs_data_dirty = true;
            bytes_used += lut.size() * sizeof(Common::Vec2f);
        };

    // Sync the proctex noise lut
    if (pica.proctex.noise_lut_dirty) {
        sync_proctex_value_lut(proctex.noise_table, fs_data.proctex_noise_lut_offset);
    }

    // Sync the proctex color map
    if (pica.proctex.color_map_dirty) {
        sync_proctex_value_lut(proctex.color_map_table, fs_data.proctex_color_map_offset);
    }

    // Sync the proctex alpha map
    if (pica.proctex.alpha_map_dirty) {
        sync_proctex_value_lut(proctex.alpha_map_table, fs_data.proctex_alpha_map_offset);
    }

    // Sync the proctex lut
    if (pica.proctex.lut_dirty) {
        Common::Vec4f* new_data = reinterpret_cast<Common::Vec4f*>(buffer + bytes_used);
        for (u32 i = 0; i < proctex.color_table.size(); i++) {
            new_data[i] = proctex.color_table[i].ToVector() / 255.0f;
        }
        fs_data.proctex_lut_offset =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
        fs_data_dirty = true;
        bytes_used += proctex.color_table.size() * sizeof(Common::Vec4f);
    }

    // Sync the proctex difference lut
    if (pica.proctex.diff_lut_dirty) {
        Common::Vec4f* new_data = reinterpret_cast<Common::Vec4f*>(buffer + bytes_used);
        for (u32 i = 0; i < proctex.color_diff_table.size(); i++) {
            new_data[i] = proctex.color_diff_table[i].ToVector() / 255.0f;
        }
        fs_data.proctex_diff_lut_offset =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
        fs_data_dirty = true;
        bytes_used += proctex.color_diff_table.size() * sizeof(Common::Vec4f);
    }

    pica.proctex.table_dirty = 0;

    texture_buffer.Commit(static_cast<u32>(bytes_used));
}

void RasterizerVulkan::UploadUniforms(bool accelerate_draw) {
    const bool sync_vs_pica = accelerate_draw && (pica.vs_setup.uniforms_dirty
#ifdef ENABLE_MM3D_PRESENTATION
        || midpoint_projection.has_value()
#endif
    );
    if (!sync_vs_pica && !vs_data_dirty && !fs_data_dirty) {
        return;
    }

    const u32 uniform_size =
        uniform_size_aligned_vs_pica + uniform_size_aligned_vs + uniform_size_aligned_fs;
    auto [uniforms, offset, invalidate] =
        uniform_buffer.Map(uniform_size, uniform_buffer_alignment);

    u32 used_bytes = 0;

    if (vs_data_dirty || invalidate) {
        std::memcpy(uniforms + used_bytes, &vs_data, sizeof(vs_data));
        pipeline_cache.UpdateRange(1, offset + used_bytes);
        vs_data_dirty = false;
        used_bytes += uniform_size_aligned_vs;
    }

    if (fs_data_dirty || invalidate) {
        std::memcpy(uniforms + used_bytes, &fs_data, sizeof(fs_data));
#ifdef ENABLE_MM3D_PRESENTATION
        std::memcpy(uniforms + used_bytes + sizeof(fs_data), &atlas_uniforms, sizeof(atlas_uniforms));
#endif
        pipeline_cache.UpdateRange(2, offset + used_bytes);
        fs_data_dirty = false;
        used_bytes += uniform_size_aligned_fs;
    }

    if (sync_vs_pica || invalidate) {
        VSPicaUniformData vs_uniforms;
        vs_uniforms.SetFromRegs(pica.vs_setup);
#ifdef ENABLE_MM3D_PRESENTATION
        if(midpoint_projection)for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
            vs_uniforms.f[i][j]=float((*midpoint_projection)[i][j]);
        if(midpoint_projection && song_overrides)for(unsigned row=0;row<4;++row)
            vs_uniforms.f[MM3D::SongEffectInterpolation::Rows[row]][3]=(*song_overrides)[row][3];
        if(midpoint_projection)for(unsigned bone=0;bone<10;++bone)if(link_overrides[bone])
            for(unsigned i=0;i<12;++i)
                vs_uniforms.f[20+bone*3+i/4][i%4]=float((*link_overrides[bone])[i/4][i%4]);
#endif
        std::memcpy(uniforms + used_bytes, &vs_uniforms, sizeof(vs_uniforms));
        pipeline_cache.UpdateRange(0, offset + used_bytes);
        pica.vs_setup.uniforms_dirty = false;
        used_bytes += uniform_size_aligned_vs_pica;
    }

    uniform_buffer.Commit(used_bytes);
}

void RasterizerVulkan::SwitchDiskResources(u64 title_id) {
#ifdef ENABLE_MM3D_PRESENTATION
    ResetPause();
    midpoint_history.Reset();forest_particle_history.Reset();link_history.Reset();song_history.Reset();song_overrides.reset();link_frame.reset();link_overrides={};
    midpoint_target_frame.reset();midpoint_world_frame.reset();midpoint_opaque_world_frame.reset();midpoint_stencil_frame.reset();midpoint_fade_frame.reset();midpoint_title_depth_frame.reset();
    for(const auto id : {"@camera_midpoint","@camera_midpoint_fade","@camera_midpoint_hud"})
        if(const auto it=prompt_layers.find(id);it!=prompt_layers.end())it->second->NextFrame();
#endif
    std::atomic_bool stop_loading = false;

    if (switch_disk_resources_callback) {
        switch_disk_resources_callback(VideoCore::LoadCallbackStage::Prepare, 0, 0, "");
    }

    pipeline_cache.SetAccurateMul(accurate_mul);
    pipeline_cache.SwitchCache(title_id, stop_loading, switch_disk_resources_callback);

    if (switch_disk_resources_callback) {
        switch_disk_resources_callback(VideoCore::LoadCallbackStage::Complete, 0, 0, "");
    }
}

} // namespace Vulkan
