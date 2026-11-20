#pragma once

#include <array>
#include <cstdint>

namespace Fast::Renderer3ds {

enum class PicaPbrTextureRole : uint8_t { Normal, Orm, Emissive, Height, Count };
inline constexpr uint32_t kPicaPbrTextureCount = static_cast<uint32_t>(PicaPbrTextureRole::Count);

enum class PicaPbrDebugView : uint8_t { Shaded, SurfaceNormals, Roughness };


struct PicaPbrMaterialParameters {
    std::array<float, 4> BaseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 3> EmissiveFactor{};
    float NormalScale = 1.0F;
    float NormalYSign = 1.0F;
    float OcclusionStrength = 1.0F;
    float RoughnessFactor = 1.0F;
    float MetallicFactor = 0.0F;
    float GlossinessStrength = 1.0F;
    float SpecularLevel = 1.0F;
    float SpecularWeight = 1.0F;
    float ScreenSpaceReflectionWeight = 1.0F;
    bool SeamSoftening = false;
    float SeamStrength = 1.0F;
    // Fraction of the native seam edge length, stable under camera motion.
    float SeamWidth = 0.05F;
    // Normal, occlusion, roughness, metallic, emission, height.
    uint32_t SeamChannels = 47U;
    float MinimumRoughness = 0.045F;
    // Editor-only preview; never serialized in a material descriptor.
    PicaPbrDebugView DebugView = PicaPbrDebugView::Shaded;
    // UV-space relief depth; white height is the native surface, black recessed.
    float HeightScale = 0.02F;
    // No selector means the native TEV graph must identify one color texture.
    uint8_t TextureSlot = 0xffU;

    bool operator==(const PicaPbrMaterialParameters&) const = default;
};

} // namespace Fast::Renderer3ds
