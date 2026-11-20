#include "fast/renderer3ds/pica_pbr_seams.h"
#include "fast/renderer3ds/pica_pbr_shading.h"
#include "fast/renderer3ds/pica_surface_lighting.h"
#include "vulkan_test_device.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>

namespace {
using namespace Fast::Renderer3ds;
using V4 = std::array<float, 4>;
struct Vertex {
  V4 Position, Normal, Uv;
};
struct Mesh {
  std::vector<Vertex> Vertices;
  std::vector<uint8_t> Payload;
  std::array<V4, 96> Uniforms{};
  PicaVertexShaderHookLayout Hooks;
  std::array<PicaSceneVertexBufferBinding, 1> Bindings{};
  const std::array<PicaNriPackedVertexAttribute, 3> Attributes{
      {{0, 0, 0}, {1, 0, 16}, {3, 0, 32}}};
  const std::array<PicaNriPackedVertexBinding, 1> Inputs{{{0, 48, false}}};
  PicaPbrSeamCache Cache;
  bool Strip;
  uint32_t Count = 0;
  explicit Mesh(bool mirror = false, bool strip = false) : Strip(strip) {
    Hooks.Transform = {PicaVertexTransformOperation::ModelViewProjection3x4,
                       0,
                       4,
                       4,
                       3,
                       20,
                       3,
                       0,
                       1};
    auto &uv = Hooks.TextureCoordinates[0];
    uv.Operation = PicaVertexTextureCoordinateOperation::CmbAffine;
    uv.EnableBooleanUniform = 1;
    uv.SourceSelectorUniform = 89;
    uv.SourceSelectorComponent = 0;
    uv.SourceSelectorConstantUniform = 93;
    uv.SourceSelectorConstantComponents = {0, 1};
    uv.CoordinateModeUniform = 92;
    uv.CoordinateModeComponent = 0;
    uv.CoordinateModeConstantUniform = 95;
    uv.CoordinateModeConstantComponents = {0, 1};
    uv.MatrixRowUUniform = 10;
    uv.MatrixRowVUniform = 11;
    uv.HomogeneousUniform = 93;
    uv.HomogeneousComponent = 1;
    uv.Sources[0] = {3, 91, 0, 6};
    uv.SourceCount = 1;
    for (int first : {4, 20})
      for (int i = 0; i < 3; ++i)
        Uniforms[first + i][i] = 1;
    Uniforms[4][3] = -.7F;
    Uniforms[6][3] = -2;
    Uniforms[10][0] = Uniforms[11][1] = Uniforms[91][0] = Uniforms[92][0] =
        Uniforms[93][1] = 1;
    Uniforms[95] = {3, 4, 0, 0};
    auto vertex = [&](float x, float y, bool right) {
      const float u =
          right ? (mirror ? .75F - .1F * x : .75F + .1F * y) : .25F + .1F * x;
      const float v = right && !mirror ? .5F - .2F * x : .5F + .2F * y;
      return Vertex{{x, y, 0, 1}, {0, 0, 1, 0}, {u, 1 - v, 0, 1}};
    };
    if (strip) {
      Vertices.resize(3); // Nonzero indexed base vertex, plus four degenerate
                          // strip primitives.
      for (auto xy : {std::array{-1.F, -1.F},
                      {0.F, -1.F},
                      {-1.F, 1.F},
                      {0.F, 1.F},
                      {0.F, 1.F}})
        Vertices.push_back(vertex(xy[0], xy[1], false));
      for (auto xy : {std::array{0.F, -1.F},
                      {0.F, -1.F},
                      {1.F, -1.F},
                      {0.F, 1.F},
                      {1.F, 1.F}})
        Vertices.push_back(vertex(xy[0], xy[1], true));
    } else {
      for (auto xy : {std::array{-1.F, -1.F},
                      {0.F, -1.F},
                      {0.F, 1.F},
                      {-1.F, -1.F},
                      {0.F, 1.F},
                      {-1.F, 1.F}})
        Vertices.push_back(vertex(xy[0], xy[1], false));
      for (auto xy : {std::array{0.F, -1.F},
                      {1.F, -1.F},
                      {1.F, 1.F},
                      {0.F, -1.F},
                      {1.F, 1.F},
                      {0.F, 1.F}})
        Vertices.push_back(vertex(xy[0], xy[1], true));
    }
    Pack();
  }
  void Pack() {
    Count = uint32_t(Vertices.size()) - (Strip ? 3 : 0);
    Bindings[0] = {0, 0, Vertices.size() * sizeof(Vertex), sizeof(Vertex)};
    Payload.resize(Vertices.size() * sizeof(Vertex) + (Strip ? Count * 2 : 0));
    std::memcpy(Payload.data(), Vertices.data(),
                Vertices.size() * sizeof(Vertex));
    if (Strip)
      for (uint16_t i = 0; i < Count; ++i)
        std::memcpy(Payload.data() + Vertices.size() * sizeof(Vertex) + i * 2,
                    &i, 2);
  }
  void Junction() {
    Vertices.clear();
    const std::array<std::array<float, 2>, 3> ring{
        {{-.9F, -.7F}, {.9F, -.7F}, {0, .9F}}};
    for (size_t chart = 0; chart < 3; ++chart)
      for (auto p :
           {std::array{0.F, 0.F}, ring[chart], ring[(chart + 1) % 3]}) {
        const float u = chart ? .75F + .1F * p[1] + .03F * float(chart - 1)
                              : .25F + .1F * p[0];
        const float v = chart ? .5F - .2F * p[0] : .5F + .2F * p[1];
        Vertices.push_back(
            {{p[0], p[1], 0, 1}, {0, 0, 1, 0}, {u, 1 - v, 0, 1}});
      }
    Pack();
  }
  const PicaPbrSeamData &Resolve(uint64_t version = 0) {
    return Cache.Resolve(
        {Payload, Bindings, Attributes, Inputs, 0,
         Vertices.size() * sizeof(Vertex), Count, Strip ? 3 : 0, Strip},
        Hooks, 66,
        {reinterpret_cast<const uint8_t *>(Uniforms.data()), sizeof(Uniforms)},
        0, Strip, version);
  }
};
void TopologyBoundaries() {
  Mesh mesh;
  Require(mesh.Resolve().SeamCount == 1,
          "manifold chart seam was not discovered");
  for (size_t i = 6; i < mesh.Vertices.size(); ++i)
    mesh.Vertices[i].Normal = {0, 1, 0, 0};
  mesh.Pack();
  Require(mesh.Resolve().SeamCount == 0, "authored hard edge was joined");
  mesh.Vertices.resize(6);
  mesh.Pack();
  Require(mesh.Resolve().SeamCount == 0, "open boundary was joined");
  Mesh disconnected;
  for (size_t i = 6; i < disconnected.Vertices.size(); ++i)
    disconnected.Vertices[i].Position[0] += .0001F;
  disconnected.Pack();
  Require(disconnected.Resolve().SeamCount == 0,
          "disconnected surface was joined");
  Mesh ambiguous;
  const std::array extra{ambiguous.Vertices[9], ambiguous.Vertices[10],
                         ambiguous.Vertices[11]};
  ambiguous.Vertices.insert(ambiguous.Vertices.end(), extra.begin(),
                            extra.end());
  ambiguous.Pack();
  Require(ambiguous.Resolve().SeamCount == 0, "non-manifold edge was guessed");
  Mesh overlapping;
  overlapping.Vertices.resize(3);
  const std::array reverse{overlapping.Vertices[2], overlapping.Vertices[1],
                           overlapping.Vertices[0]};
  for (auto vertex : reverse) {
    vertex.Uv[0] += .5F;
    overlapping.Vertices.push_back(vertex);
  }
  overlapping.Pack();
  Require(overlapping.Resolve().SeamCount == 0,
          "coincident opposing faces were treated as adjacent charts");
  std::cout << "Topology: manifold seam, hard edge, open boundary, "
               "disconnected and non-manifold ownership passed\n";
}
struct Image {
  VkImage Handle{};
  VkDeviceMemory Memory{};
  VkImageView View{};
};
struct Fixture {
  static constexpr uint32_t Width = 512, Height = 128;
  Gpu G;
  Mesh Geometry;
  VkCommandPool Pool{};
  VkCommandBuffer Command{};
  VkRenderPass RenderPass{};
  VkFramebuffer Framebuffer{};
  VkDescriptorSetLayout SetLayout{};
  VkDescriptorPool DescriptorPool{};
  VkDescriptorSet Set{};
  VkPipelineLayout PipelineLayout{};
  std::array<VkPipeline, 2> Pipelines{};
  VkSampler Sampler{};
  std::array<Image, 4> Targets{}, Maps{};
  Gpu::Buffer Vertices{}, Seams{}, Material{}, Surface{}, Readback{}, Upload{};
  explicit Fixture(bool mirror, bool strip, bool junction = false)
      : G([] {
          VkPhysicalDeviceFeatures f{};
          f.geometryShader = VK_TRUE;
          return f;
        }()),
        Geometry(mirror, strip) {
    if (junction)
      Geometry.Junction();
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = G.Family;
    Check(vkCreateCommandPool(G.Device, &pool, nullptr, &Pool));
    VkCommandBufferAllocateInfo command{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = Pool;
    command.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(G.Device, &command, &Command));
    const auto &data = Geometry.Resolve(1);
    Require(data.SeamCount == (junction ? 3 : 1) &&
                data.TriangleCount == (junction ? 3
                                       : strip  ? 8
                                                : 4),
            "primitive-to-chart indexing failed");
    Vertices = G.MakeBuffer(Geometry.Payload.size(),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    std::memcpy(Vertices.Mapped, Geometry.Payload.data(),
                Geometry.Payload.size());
    Seams = G.MakeBuffer(data.Words.size() * 16,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(Seams.Mapped, data.Words.data(), data.Words.size() * 16);
    Material = G.MakeBuffer(sizeof(PicaPbrUniforms),
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    Surface = G.MakeBuffer(sizeof(PicaSurfaceLightingUniforms),
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::memset(Surface.Mapped, 0, sizeof(PicaSurfaceLightingUniforms));
    Readback = G.MakeBuffer(Width * Height * 16 * Targets.size(),
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Upload =
        G.MakeBuffer(Maps.size() * 2 * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const std::array<V4, 8> texels{
        {{.8F, .5F, .9F, 1},
         mirror ? V4{.2F, .5F, .9F, 1} : V4{.5F, .2F, .9F, 1},
         {.2F, .3F, .4F, .2F},
         {.8F, .7F, .6F, .8F},
         {.2F, .2F, .2F, 1},
         {.8F, .8F, .8F, 1},
         {.1F, 0, 0, 1},
         {.9F, 0, 0, 1}}};
    std::memcpy(Upload.Mapped, texels.data(), sizeof(texels));
    Begin();
    for (size_t i = 0; i < Maps.size(); ++i) {
      Maps[i] = MakeImage(
          2, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.image = Maps[i].Handle;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
      VkBufferImageCopy copy{};
      copy.bufferOffset = i * 32;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {2, 1, 1};
      vkCmdCopyBufferToImage(Command, Upload.Handle, Maps[i].Handle,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier.oldLayout = barrier.newLayout;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);
    }
    Submit();
    std::array<VkAttachmentDescription, 4> attachments{};
    std::array<VkAttachmentReference, 4> refs{};
    std::array<VkImageView, 4> views{};
    for (uint32_t i = 0; i < 4; ++i) {
      Targets[i] = MakeImage(Width, Height,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
      views[i] = Targets[i].View;
      attachments[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
      attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
      attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachments[i].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      refs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 4;
    subpass.pColorAttachments = refs.data();
    VkSubpassDependency dependency{
        0,
        VK_SUBPASS_EXTERNAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        0};
    VkRenderPassCreateInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    render.attachmentCount = 4;
    render.pAttachments = attachments.data();
    render.subpassCount = 1;
    render.pSubpasses = &subpass;
    render.dependencyCount = 1;
    render.pDependencies = &dependency;
    Check(vkCreateRenderPass(G.Device, &render, nullptr, &RenderPass));
    VkFramebufferCreateInfo framebuffer{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = RenderPass;
    framebuffer.attachmentCount = 4;
    framebuffer.pAttachments = views.data();
    framebuffer.width = Width;
    framebuffer.height = Height;
    framebuffer.layers = 1;
    Check(vkCreateFramebuffer(G.Device, &framebuffer, nullptr, &Framebuffer));
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(G.Device, &sampler, nullptr, &Sampler));
    std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
    bindings[0] = {16, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {17, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (uint32_t i = 0; i < 4; ++i)
      bindings[2 + i] = {18 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[6] = {26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setLayout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayout.bindingCount = bindings.size();
    setLayout.pBindings = bindings.data();
    Check(
        vkCreateDescriptorSetLayout(G.Device, &setLayout, nullptr, &SetLayout));
    const std::array<VkDescriptorPoolSize, 3> sizes{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    VkDescriptorPoolCreateInfo descriptors{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptors.maxSets = 1;
    descriptors.poolSizeCount = sizes.size();
    descriptors.pPoolSizes = sizes.data();
    Check(vkCreateDescriptorPool(G.Device, &descriptors, nullptr,
                                 &DescriptorPool));
    VkDescriptorSetAllocateInfo allocate{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = DescriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &SetLayout;
    Check(vkAllocateDescriptorSets(G.Device, &allocate, &Set));
    const std::array<VkDescriptorBufferInfo, 3> buffers{
        {{Surface.Handle, 0, sizeof(PicaSurfaceLightingUniforms)},
         {Material.Handle, 0, sizeof(PicaPbrUniforms)},
         {Seams.Handle, 0, data.Words.size() * 16}}};
    std::array<VkDescriptorImageInfo, 4> images{};
    std::array<VkWriteDescriptorSet, 7> writes{};
    for (uint32_t i = 0; i < 7; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = Set;
      writes[i].dstBinding = bindings[i].binding;
      writes[i].descriptorType = bindings[i].descriptorType;
      writes[i].descriptorCount = 1;
      if (i < 2)
        writes[i].pBufferInfo = &buffers[i];
      else if (i == 6)
        writes[i].pBufferInfo = &buffers[2];
      else {
        images[i - 2] = {Sampler, Maps[i - 2].View,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i].pImageInfo = &images[i - 2];
      }
    }
    vkUpdateDescriptorSets(G.Device, writes.size(), writes.data(), 0, nullptr);
    VkPipelineLayoutCreateInfo layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &SetLayout;
    Check(vkCreatePipelineLayout(G.Device, &layout, nullptr, &PipelineLayout));
    for (uint32_t active = 0; active < 2; ++active)
      Pipelines[active] = Pipeline(active != 0);
  }
  Image MakeImage(uint32_t width, uint32_t height, VkImageUsageFlags usage) {
    Image result;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    image.extent = {width, height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.usage = usage;
    Check(vkCreateImage(G.Device, &image, nullptr, &result.Handle));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(G.Device, result.Handle, &requirements);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = G.Type(requirements.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(G.Device, &memory, nullptr, &result.Memory));
    Check(vkBindImageMemory(G.Device, result.Handle, result.Memory, 0));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = result.Handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(G.Device, &view, nullptr, &result.View));
    return result;
  }
  VkPipeline Pipeline(bool active) {
    const auto vs = G.Shader(R"glsl(#version 450
layout(location=0) in vec4 position;layout(location=1) in vec4 normal;layout(location=3) in vec4 uv;
layout(location=0) out vec2 pica_texcoord0;layout(location=1) out vec3 pica_view;
layout(location=9) out vec3 pica_surface_normal;layout(location=10) out vec4 pica_surface_tint;
void main(){gl_Position=vec4(position.xy,0,1);pica_texcoord0=uv.xy;
pica_view=-vec3(position.x-.7,position.y,-2);pica_surface_normal=normal.xyz;pica_surface_tint=vec4(1);})glsl",
                             shaderc_vertex_shader);
    PicaShaderHookLayout hooks;
    hooks.SampledTextureMask = 1;
    hooks.TextureSamples[0] = {0, PicaTextureCoordinateOperation::NativeVFlip};
    PicaPbrShaderOptions options;
    options.TextureSlot = 0;
    options.SeamSoftening = active;
    const auto material = BuildPicaPbrShader(hooks, options);
    Require(material.has_value(), "PBR fixture contract rejected");
    std::string source = R"glsl(#version 450
layout(location=0) in vec2 pica_texcoord0;layout(location=1) in vec3 pica_view;
layout(location=0) out vec4 native_result;layout(location=1) out vec4 normal_roughness;
layout(location=2) out vec4 auxiliary;layout(location=3) out vec4 relief;
vec3 byteround3(vec3 v){return round(v*255.0)/255.0;}
)glsl";
    source += PicaSurfaceLightingDeclarations();
    source += "\nvec3 pica_surface_shadow(int slot){return vec3(1);}\n"
              "float pica_point_shadow(float s,vec3 v,float r){return 1.0;}\n";
    source += material->Declarations;
    source += "\nvoid main(){vec4 rounded_primary_color=vec4(1);\n";
    source += material->UnlitInputs;
    source += "\nvec2 native_uv=vec2(pica_texcoord0.x,1-pica_texcoord0.y);vec4 "
              "combiner_output=texture(pbr_orm_map,native_uv);native_result="
              "vec4(native_uv,combiner_output.r,combiner_output.a);\n";
    source += material->ShadingBody;
    source += "\nnormal_roughness=vec4(pbr_normal,pbr_roughness);auxiliary="
              "vec4(pbr_ao,pbr_metallic,pbr_emission.r,1);\n";
    source += active
                  ? "relief=vec4(pbr_uv,pbr_seam.weight,pbr_seam.value.height);"
                  : "relief=vec4(pbr_uv,0,0);";
    source += "\n{\n" + material->NativeCoordinates +
              "\nnative_result.a=texture(pbr_orm_map,vec2(pica_texcoord0.x,1-"
              "pica_texcoord0.y)).a;}\n}";
    const auto fs = G.Shader(source, shaderc_fragment_shader);
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
         {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}}};
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex),
                                                  VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 3> attributes{
        {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
         {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32}}};
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 3;
    vertex.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = Geometry.Strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, float(Width), float(Height), 0, 1};
    const VkRect2D scissor{{0, 0}, {Width, Height}};
    VkPipelineViewportStateCreateInfo viewportInfo{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportInfo.viewportCount = viewportInfo.scissorCount = 1;
    viewportInfo.pViewports = &viewport;
    viewportInfo.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    std::array<VkPipelineColorBlendAttachmentState, 4> attachments{};
    for (auto &a : attachments)
      a.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 4;
    blend.pAttachments = attachments.data();
    VkGraphicsPipelineCreateInfo pipeline{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages.data();
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewportInfo;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples;
    pipeline.pColorBlendState = &blend;
    pipeline.layout = PipelineLayout;
    pipeline.renderPass = RenderPass;
    VkPipeline result;
    Check(vkCreateGraphicsPipelines(G.Device, {}, 1, &pipeline, nullptr,
                                    &result));
    vkDestroyShaderModule(G.Device, vs, nullptr);
    vkDestroyShaderModule(G.Device, fs, nullptr);
    return result;
  }
  void Begin() {
    Check(vkResetCommandBuffer(Command, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(Command, &begin));
  }
  void Submit() {
    Check(vkEndCommandBuffer(Command));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &Command;
    Check(vkQueueSubmit(G.Queue, 1, &submit, {}));
    Check(vkQueueWaitIdle(G.Queue));
  }
  std::vector<V4> Draw(bool active, float strength = 1,
                       uint32_t channels = 63) {
    PicaPbrMaterialParameters parameters;
    parameters.SeamSoftening = active;
    parameters.SeamStrength = strength;
    parameters.SeamWidth = .1F;
    parameters.SeamChannels = channels;
    parameters.HeightScale = .08F;
    parameters.MetallicFactor = 1;
    parameters.EmissiveFactor = {1, 1, 1};
    auto uniforms = BuildPicaPbrUniforms(parameters, 15, {});
    uniforms.SeamSettings[3] =
        std::bit_cast<float>(Geometry.Resolve(1).TriangleCount);
    std::memcpy(Material.Mapped, &uniforms, sizeof(uniforms));
    Begin();
    const std::array<VkClearValue, 4> clears{};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = RenderPass;
    pass.framebuffer = Framebuffer;
    pass.renderArea = {{0, 0}, {Width, Height}};
    pass.clearValueCount = 4;
    pass.pClearValues = clears.data();
    vkCmdBeginRenderPass(Command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(Command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      Pipelines[active]);
    vkCmdBindDescriptorSets(Command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            PipelineLayout, 0, 1, &Set, 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(Command, 0, 1, &Vertices.Handle, &offset);
    if (Geometry.Strip) {
      vkCmdBindIndexBuffer(Command, Vertices.Handle,
                           Geometry.Vertices.size() * sizeof(Vertex),
                           VK_INDEX_TYPE_UINT16);
      vkCmdDrawIndexed(Command, Geometry.Count, 1, 0, 3, 0);
    } else
      vkCmdDraw(Command, Geometry.Count, 1, 0, 0);
    vkCmdEndRenderPass(Command);
    for (uint32_t i = 0; i < 4; ++i) {
      VkBufferImageCopy copy{};
      copy.bufferOffset = i * Width * Height * 16;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {Width, Height, 1};
      vkCmdCopyImageToBuffer(Command, Targets[i].Handle,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             Readback.Handle, 1, &copy);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(Command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                         0, nullptr);
    Submit();
    const auto *p = static_cast<const V4 *>(Readback.Mapped);
    return {p, p + 4 * Width * Height};
  }
  void Run() {
    const auto off = Draw(false), zero = Draw(true, 0), on = Draw(true),
               roughnessOnly = Draw(true, 1, 4);
    const size_t plane = Width * Height;
    for (size_t i = 0; i < plane; ++i) {
      Require(off[i] == on[i] && off[i] == roughnessOnly[i],
              "base UV, base color or native alpha changed");
      for (size_t a = 0; a < 3; ++a)
        Require(off[a * plane + i] == zero[a * plane + i],
                "zero-strength seam path changed material output");
      Require(off[2 * plane + i] == roughnessOnly[2 * plane + i],
              "roughness-only blending changed AO, metallic or emission");
      Require(off[3 * plane + i][0] == roughnessOnly[3 * plane + i][0] &&
                  off[3 * plane + i][1] == roughnessOnly[3 * plane + i][1],
              "unselected height changed auxiliary relief");
    }
    const size_t left = Height / 2 * Width + Width / 2 - 1, right = left + 1,
                 interior = Height / 2 * Width + Width / 4;
    Require(std::abs(off[plane + left][3] - off[plane + right][3]) > .39F,
            "fixture lacks a hard native roughness seam");
    Require(std::abs(on[plane + left][3] - on[plane + right][3]) < .001F,
            "roughness seam remains discontinuous");
    for (size_t c = 0; c < 3; ++c)
      Require(std::abs(on[2 * plane + left][c] - on[2 * plane + right][c]) <
                  .001F,
              "scalar or linear emission seam remains discontinuous");
    Require(std::abs(on[plane + left][0] - .6F) < 1e-5F &&
                std::abs(on[plane + right][0] - .6F) < 1e-5F &&
                std::abs(on[plane + right][1]) < 1e-5F,
            "normal blending ignored rotated/mirrored tangent frames");
    const float linearMean = (std::pow((.2F + .055F) / 1.055F, 2.4F) +
                              std::pow((.8F + .055F) / 1.055F, 2.4F)) *
                             .5F;
    Require(std::abs(on[2 * plane + left][2] - linearMean) < .001F,
            "emission was blended in encoded sRGB");
    Require(std::abs(on[3 * plane + left][3] - .5F) < 1e-6F &&
                std::abs(on[3 * plane + right][3] - .5F) < 1e-6F,
            "shared edge height does not converge");
    Require(on[3 * plane + left][0] != off[3 * plane + left][0] ||
                on[3 * plane + left][1] != off[3 * plane + left][1],
            "height selection did not affect auxiliary relief");
    Require(off[plane + interior] == on[plane + interior] &&
                off[2 * plane + interior] == on[2 * plane + interior],
            "seam blending leaked outside its width");
    std::cout << "GPU: production PBR, base/alpha invariance, zero strength, "
                 "channel isolation, tangent frames, linear emission, height "
                 "relief and finite seam width passed"
              << (Geometry.Strip
                      ? " (indexed strip + degenerates + base vertex)"
                      : " (triangle list)")
              << '\n';
  }
  ~Fixture() {
    for (auto p : Pipelines)
      vkDestroyPipeline(G.Device, p, nullptr);
    vkDestroyPipelineLayout(G.Device, PipelineLayout, nullptr);
    vkDestroyDescriptorPool(G.Device, DescriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(G.Device, SetLayout, nullptr);
    vkDestroySampler(G.Device, Sampler, nullptr);
    vkDestroyFramebuffer(G.Device, Framebuffer, nullptr);
    vkDestroyRenderPass(G.Device, RenderPass, nullptr);
    for (auto images : {Targets, Maps})
      for (auto image : images) {
        vkDestroyImageView(G.Device, image.View, nullptr);
        vkDestroyImage(G.Device, image.Handle, nullptr);
        vkFreeMemory(G.Device, image.Memory, nullptr);
      }
    for (auto b : {Vertices, Seams, Material, Surface, Readback, Upload})
      G.Free(b);
    vkDestroyCommandPool(G.Device, Pool, nullptr);
  }
};
void JunctionConsensus() {
  Fixture fixture(false, false, true);
  const auto image = fixture.Draw(true);
  const size_t plane = Fixture::Width * Fixture::Height;
  for (uint32_t y : {Fixture::Height / 2 - 1, Fixture::Height / 2})
    for (uint32_t x : {Fixture::Width / 2 - 1, Fixture::Width / 2}) {
      const size_t p = y * Fixture::Width + x;
      Require(std::abs(image[plane + p][3] - (.3F + .7F + .7F) / 3) < .01F,
              "three-chart junction does not converge to a common roughness");
      Require(std::abs(image[2 * plane + p][0] - .6F) < .01F,
              "overlapping seam bands disagree at a three-chart vertex");
    }
  std::cout << "GPU: three-chart corner consensus with overlapping seam bands "
               "passed\n";
}
} // namespace
int main() try {
  TopologyBoundaries();
  Fixture(false, false).Run();
  Fixture(true, true).Run();
  JunctionConsensus();
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
