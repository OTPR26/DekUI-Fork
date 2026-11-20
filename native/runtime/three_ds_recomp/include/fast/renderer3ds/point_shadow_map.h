#pragma once

#include "fast/renderer3ds/virtual_shadow_map.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Fast::Renderer3ds {

// Cube shadow maps of point lights, packed in one 2D depth atlas: one row of
// six face tiles per light slot. Each tile holds its face plus a border, so
// filtering near a face edge stays inside the tile.
inline constexpr uint32_t kPointShadowFaceCount = 6U;
inline constexpr uint32_t kPointShadowBorderTexels = 2U;
inline constexpr uint32_t kMaximumPointShadowLights = 8U;
// Near plane as a fraction of the far plane: geometry closer to the light
// than this never occludes it.
inline constexpr float kPointShadowNearFraction = 1.0F / 64.0F;

// World-axis face basis, shared with the receiver program.
struct PointShadowFace {
    ShadowVector Forward{};
    ShadowVector Right{};
    ShadowVector Up{};
};

[[nodiscard]] const std::array<PointShadowFace, kPointShadowFaceCount>& PointShadowFaces() noexcept;

struct PointShadowLayout {
    uint32_t FaceTexels = 0U;
    uint32_t TileTexels = 0U;
    uint32_t SlotCount = 0U;
    uint32_t Width = 0U;
    uint32_t Height = 0U;

    [[nodiscard]] bool Valid() const noexcept {
        return FaceTexels != 0U && SlotCount != 0U && SlotCount <= kMaximumPointShadowLights &&
               TileTexels == FaceTexels + 2U * kPointShadowBorderTexels &&
               Width == TileTexels * kPointShadowFaceCount && Height == TileTexels * SlotCount;
    }
    // Tangent half-extent of a rendered face: the face plus its border.
    [[nodiscard]] float FaceExtent() const noexcept {
        return static_cast<float>(TileTexels) / static_cast<float>(FaceTexels);
    }
};

[[nodiscard]] PointShadowLayout BuildPointShadowLayout(uint32_t faceTexels, uint32_t slotCount) noexcept;

// A light that may cast: its world position and the radius its lighting reaches.
struct PointShadowLight {
    uint64_t SourceIdentity = 0U;
    ShadowVector PositionWorld{};
    float Radius = 0.0F;
    float Luminance = 0.0F;
};

// One atlas row. The far plane is the light radius scaled by the shadow
// range, so a light shadows only within its own reach.
struct PointShadowSlot {
    uint64_t SourceIdentity = 0U;
    ShadowVector PositionWorld{};
    float Radius = 0.0F;
    float FarPlane = 0.0F;
    float NearPlane = 0.0F;

    [[nodiscard]] bool Valid() const noexcept {
        return SourceIdentity != 0U && FarPlane > NearPlane && NearPlane > 0.0F;
    }
};

using PointShadowSlots = std::array<PointShadowSlot, kMaximumPointShadowLights>;

[[nodiscard]] PointShadowSlot BuildPointShadowSlot(const PointShadowLight& light, float range) noexcept;

// Selects at most `capacity` lights, nearest first by the distance from the
// camera to the light's reach, then brighter first. A light that keeps a
// place keeps its previous row.
[[nodiscard]] PointShadowSlots SelectPointShadowSlots(
    std::span<const PointShadowLight> candidates, const ShadowVector& camera, float range,
    uint32_t capacity, const PointShadowSlots& previous) noexcept;

// Column-major world -> face clip transform (Vulkan depth 0..1):
// x = Right.(p - L) / extent, y = Up.(p - L) / extent, w = Forward.(p - L),
// z = A w - C with A = far / (far - near) and C = far near / (far - near).
[[nodiscard]] ShadowMatrix BuildPointShadowFaceMatrix(
    const PointShadowSlot& slot, uint32_t face, const PointShadowLayout& layout) noexcept;

// Conservative: may the world bounds hold geometry inside the face frustum
// between the near and far planes?
[[nodiscard]] bool PointShadowFaceIntersects(
    const PointShadowSlot& slot, uint32_t face, const PointShadowLayout& layout,
    const ShadowBounds& bounds) noexcept;

// Receiver program: `float pica_point_shadow(float slot, vec3 light_to_fragment,
// float radius)` over the atlas at binding 27. It reads the surface-lighting
// block (`pica_surface.point_shadow`, `pica_surface.point_shadow_atlas`), so it
// follows the surface-lighting declarations.
[[nodiscard]] std::string_view PointShadowReceiverShader() noexcept;

} // namespace Fast::Renderer3ds
