// Generated from maintained renderer pass sources; no game assets or captures.
// Explicit Vulkan target per artifact, main, performance. Preserve source donor notices.
// Regenerate builtin_pass.spvpack with tools/renderer/tev_program/build_pass_artifacts.
#include "fast/renderer/builtin_pass_binaries.h"
#include "fast/renderer/spirv_pack.h"

namespace Fast::Renderer {
namespace {
TRIAEVUM_SPIRV_PACK_EMBED_BEGIN
alignas(16) const unsigned char kPack[] = {
#if TRIAEVUM_SPIRV_PACK_EMBED
#embed "builtin_pass.spvpack"
#else
#include "builtin_pass.spvpack.inc"
#endif
};
TRIAEVUM_SPIRV_PACK_EMBED_END
} // namespace

std::span<const PassShaderArtifact> BuiltinPassArtifacts() {
    static const auto entries = Renderer::ReadSpirvPack(kPack);
    static const auto artifacts = PassShaderArtifactsFromPack(entries);
    return artifacts;
}
} // namespace Fast::Renderer
