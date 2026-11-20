#pragma once

#ifdef ENABLE_RENDERER3DS_VULKAN

#include "fast/renderer3ds/nri_pica_pipeline_bridge.h"
#include "fast/renderer3ds/pica_fragment_artifact.h"
#include "fast/renderer3ds/pica_vertex_artifact.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Fast::Renderer3ds {

// The startup preparation's program parts: every shader-bearing library part
// an always-available draw links. The renderer creates them at startup and
// Forge's device preparation creates the same parts into the device pipeline
// cache. Both derive them here: one differing parameter is a cache miss.
struct PicaStartupProgramDevice {
    VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;
    // Fragment primitive IDs (PBR seams) exist only with geometry shaders.
    bool GeometryShaders = false;
};

struct PicaStartupProgramPart {
    std::span<const uint32_t> Vertex;
    std::span<const uint32_t> Fragment;
    bool AlphaToCoverage = false;
};

// SPIR-V declares its capabilities first, right after the five-word header.
inline bool DeclaresSpirvCapability(std::span<const uint32_t> spirv, uint32_t capability) {
    constexpr uint32_t kOpCapability = 17U;
    for (size_t at = 5U; at + 1U < spirv.size();) {
        const uint32_t words = spirv[at] >> 16U;
        if (words == 0U || (spirv[at] & 0xffffU) != kOpCapability) break;
        if (spirv[at + 1U] == capability) return true;
        at += words;
    }
    return false;
}

inline constexpr uint32_t kSpirvStorageInput = 1U;
inline constexpr uint32_t kSpirvStorageOutput = 3U;

// The locations of a module's Input or Output variables, as a mask (built-ins
// have none). Every PICA stage interface variable takes one location.
inline uint64_t SpirvInterfaceLocations(std::span<const uint32_t> spirv, uint32_t storageClass) {
    constexpr uint32_t kOpDecorate = 71U, kOpVariable = 59U, kDecorationLocation = 30U;
    std::vector<std::pair<uint32_t, uint32_t>> locations;
    std::vector<uint32_t> variables;
    for (size_t at = 5U; at < spirv.size();) {
        const uint32_t words = spirv[at] >> 16U;
        if (words == 0U || words > spirv.size() - at) break;
        const uint32_t opcode = spirv[at] & 0xffffU;
        if (opcode == kOpDecorate && words >= 4U && spirv[at + 2U] == kDecorationLocation)
            locations.emplace_back(spirv[at + 1U], spirv[at + 3U]);
        else if (opcode == kOpVariable && words >= 4U && spirv[at + 3U] == storageClass)
            variables.push_back(spirv[at + 2U]);
        at += words;
    }
    uint64_t mask = 0;
    for (const auto& [id, location] : locations)
        if (location < 64U && std::find(variables.begin(), variables.end(), id) != variables.end())
            mask |= uint64_t{1} << location;
    return mask;
}

// A link is valid only when the vertex program writes every location the
// fragment program reads.
inline bool PicaStageInterfaceLinks(std::span<const uint32_t> vertex, std::span<const uint32_t> fragment) {
    const uint64_t inputs = SpirvInterfaceLocations(fragment, kSpirvStorageInput);
    return (inputs & ~SpirvInterfaceLocations(vertex, kSpirvStorageOutput)) == 0U;
}

// The title vertex family, the superset effect programs (empty when draws do
// not use them) and the canonical bases, each once, not the vertex/fragment
// product. Vertex parts first (cheap, every link needs one), then the superset
// programs: they take longest and are what a first draw waits for. Each item
// is a valid link: its fragment program takes the startup layout's separate
// samplers and reads only locations its vertex program writes (the temporal
// and surface programs read the outputs of the matching vertex variants).
inline std::vector<PicaStartupProgramPart> PicaStartupProgramParts(
    std::span<const PicaVertexArtifact> vertices,
    std::span<const PicaFragmentArtifact> superset,
    std::span<const PicaFragmentArtifact> canonical,
    const PicaStartupProgramDevice& device) {
    if (vertices.empty()) throw std::runtime_error("native program preparation has no vertex family");
    if (canonical.empty()) throw std::runtime_error("native program preparation has no fragment family");
    const auto usable = [&](const PicaFragmentArtifact& artifact) {
        constexpr uint32_t kCapabilityGeometry = 2U;
        return artifact.SeparateSamplers &&
               (device.GeometryShaders || !DeclaresSpirvCapability(artifact.Spirv, kCapabilityGeometry));
    };
    bool firstVertexLinked = false;
    const auto fragmentFamily = [&](std::span<const PicaFragmentArtifact> family) {
        std::vector<PicaStartupProgramPart> items;
        for (const auto& artifact : family) {
            if (!usable(artifact)) continue;
            const auto vertex = std::find_if(vertices.begin(), vertices.end(), [&](const PicaVertexArtifact& v) {
                return PicaStageInterfaceLinks(v.Spirv, artifact.Spirv);
            });
            // No vertex program of the family feeds it, so no draw links it.
            if (vertex == vertices.end()) continue;
            firstVertexLinked |= vertex == vertices.begin();
            items.push_back({vertex->Spirv, artifact.Spirv, false});
            // Alpha-tested draws cover by alpha once samples exceed one.
            if (device.Samples != VK_SAMPLE_COUNT_1_BIT)
                items.push_back({vertex->Spirv, artifact.Spirv, true});
        }
        return items;
    };
    const auto supersetItems = fragmentFamily(superset);
    const auto canonicalItems = fragmentFamily(canonical);
    if (canonicalItems.empty())
        throw std::runtime_error("native program preparation has no fragment family");
    std::vector<PicaStartupProgramPart> parts;
    for (size_t index = firstVertexLinked ? 1U : 0U; index < vertices.size(); ++index) {
        const auto& vertex = vertices[index];
        const auto base = std::find_if(canonical.begin(), canonical.end(), [&](const PicaFragmentArtifact& f) {
            return usable(f) && PicaStageInterfaceLinks(vertex.Spirv, f.Spirv);
        });
        if (base == canonical.end())
            throw std::runtime_error("native program preparation has no canonical base for a vertex program");
        parts.push_back({vertex.Spirv, base->Spirv, false});
    }
    parts.insert(parts.end(), supersetItems.begin(), supersetItems.end());
    parts.insert(parts.end(), canonicalItems.begin(), canonicalItems.end());
    return parts;
}

// Creates the library parts of one item through the bridge's live factory. A
// preparation interface, never geometry submitted to a draw.
template <typename Bridge>
bool PreparePicaStartupProgramPart(Bridge& bridge, const PicaStartupProgramPart& part,
                                   const PicaStartupProgramDevice& device) {
    VkVertexInputBindingDescription binding{0, 16U * 16U, VK_VERTEX_INPUT_RATE_VERTEX};
    std::array<VkVertexInputAttributeDescription, 16> attributes{};
    for (uint32_t i = 0; i < attributes.size(); ++i)
        attributes[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16U};
    NriPicaGraphicsPipelineDesc desc;
    desc.VertexBindings = {&binding, 1};
    desc.VertexAttributes = attributes;
    desc.VertexSpirv = part.Vertex;
    desc.FragmentSpirv = part.Fragment;
    desc.ColorAttachmentCount = 1;
    desc.ColorFormats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    desc.Colors[0].colorWriteMask = 15;
    desc.DepthStencilFormat = device.DepthFormat;
    desc.Samples = device.Samples;
    desc.AlphaToCoverage = part.AlphaToCoverage;
    return bridge.PreparePipeline(desc, NriPicaPipelineFactory::Libraries);
}

} // namespace Fast::Renderer3ds

#endif
