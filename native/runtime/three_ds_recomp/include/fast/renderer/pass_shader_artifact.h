#pragma once
#include "fast/renderer/shaderc_compiler.h"
#include "fast/renderer/spirv_pack.h"
#include <span>

namespace Fast::Renderer {
// Explicit Vulkan target/main artifacts. Source, stage and ordered defines identify
// the compiled program; pass names and scene identities are not lookup keys.
struct PassShaderArtifact {
    SpirvStage Stage;
    std::string_view Source;
    std::span<const std::pair<std::string_view, std::string_view>> Defines;
    std::span<const uint32_t> Spirv;
    uint32_t VulkanMinor = 2;
};
inline std::span<const uint32_t> FindPassShaderArtifact(
    std::span<const PassShaderArtifact> artifacts, std::string_view source,
    SpirvStage stage, const ShaderDefines& defines, uint32_t vulkanMinor = 2) {
    for (const auto& artifact : artifacts) {
        if (artifact.VulkanMinor != vulkanMinor || artifact.Stage != stage || artifact.Source != source ||
            artifact.Defines.size() != defines.size()) continue;
        bool same = true;
        for (size_t i = 0; i < defines.size(); ++i)
            same &= artifact.Defines[i].first == defines[i].first &&
                    artifact.Defines[i].second == defines[i].second;
        if (same && artifact.Spirv.size() >= 5 && artifact.Spirv[0] == 0x07230203U)
            return artifact.Spirv;
    }
    return {};
}
// The pass programs of a pack, viewing the pack's modules and the entries'
// define lists (the entries must outlive the artifacts).
inline std::vector<PassShaderArtifact> PassShaderArtifactsFromPack(std::span<const SpirvPackEntry> entries) {
    std::vector<PassShaderArtifact> artifacts;
    for (const auto& entry : entries)
        if (entry.Kind == SpirvPackKind::Pass)
            artifacts.push_back({static_cast<SpirvStage>(entry.Stage), entry.Source, entry.Defines,
                                 entry.Spirv, entry.VulkanMinor});
    return artifacts;
}
}
