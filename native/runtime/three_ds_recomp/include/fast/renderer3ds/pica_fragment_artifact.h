#pragma once

#include "pica_shader_source_identity.h"
#include "fast/renderer/spirv_pack.h"
#include <span>

namespace Fast::Renderer3ds {

struct PicaFragmentArtifact {
    PicaShaderSourceIdentity Source;
    bool SeparateSamplers;
    std::span<const uint32_t> Spirv;
};

// The fragment programs of a pack, viewing the pack's modules.
inline std::vector<PicaFragmentArtifact> PicaFragmentArtifactsFromPack(
    std::span<const Renderer::SpirvPackEntry> entries) {
    std::vector<PicaFragmentArtifact> artifacts;
    for (const auto& entry : entries)
        if (entry.Kind == Renderer::SpirvPackKind::PicaFragment)
            artifacts.push_back({{entry.Identity[0], entry.Identity[1], entry.Identity[2]},
                                 (entry.Flags & Renderer::kSpirvPackSeparateSamplers) != 0U, entry.Spirv});
    return artifacts;
}

inline std::span<const uint32_t> FindPicaFragmentArtifact(
    std::span<const PicaFragmentArtifact> artifacts, std::string_view source,
    bool separateSamplers) {
    const auto identity = IdentifyPicaShaderSource(source);
    if (!identity.Available()) return {};
    for (const auto& artifact : artifacts) {
        if (artifact.SeparateSamplers == separateSamplers && artifact.Source == identity &&
            artifact.Spirv.size() >= 5 && artifact.Spirv[0] == 0x07230203U)
            return artifact.Spirv;
    }
    return {};
}

} // namespace Fast::Renderer3ds
