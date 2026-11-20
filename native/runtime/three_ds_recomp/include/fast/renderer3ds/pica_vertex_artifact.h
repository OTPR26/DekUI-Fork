#pragma once
#include "pica_shader_source_identity.h"
#include "fast/renderer/spirv_pack.h"
#include <span>

namespace Fast::Renderer3ds {
// Immutable binary published by a title adapter. The owner must outlive every
// submitted view. Source identity prevents reuse after vertex instrumentation.
struct PicaVertexArtifact {
    PicaShaderSourceIdentity Source;
    std::span<const uint32_t> Spirv;
    bool Matches(std::string_view source) const {
        return Source.Available() && Spirv.size() >= 5 && Spirv[0] == 0x07230203U &&
               Source == IdentifyPicaShaderSource(source);
    }
};
inline PicaVertexArtifact FindPicaVertexArtifact(std::span<const PicaVertexArtifact> artifacts,
                                                std::string_view source) {
    const auto id = IdentifyPicaShaderSource(source);
    for (const auto& artifact : artifacts)
        if (artifact.Source.Available() && artifact.Source == id && artifact.Spirv.size() >= 5 &&
            artifact.Spirv[0] == 0x07230203U) return artifact;
    return {};
}
// The vertex programs of a pack, viewing the pack's modules.
inline std::vector<PicaVertexArtifact> PicaVertexArtifactsFromPack(
    std::span<const Renderer::SpirvPackEntry> entries) {
    std::vector<PicaVertexArtifact> artifacts;
    for (const auto& entry : entries)
        if (entry.Kind == Renderer::SpirvPackKind::PicaVertex)
            artifacts.push_back({{entry.Identity[0], entry.Identity[1], entry.Identity[2]}, entry.Spirv});
    return artifacts;
}
} // namespace Fast::Renderer3ds
