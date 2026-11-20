#pragma once

#include "fast/renderer3ds/pica_scene_payloads.h"
#include "fast/renderer3ds/pica_shadow_geometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace Fast::Renderer3ds {

// Triangle geometry of one native draw for a world-space distance field
// (global illumination). Positions are float3; indices form a triangle list
// over them, independent of the native topology and index width.
struct PicaGiTriangleMesh {
    std::vector<std::array<float, 3>> Positions;
    std::vector<uint32_t> Indices;
    ShadowBounds LocalBounds;
};

struct PicaGiGeometryCache {
    uint64_t Version = 0;
    std::shared_ptr<const PicaGiTriangleMesh> Mesh;
};

// A rigid draw keeps its mesh in model space with the draw's model-to-world
// transform: the mesh is shared by every placement of that geometry version
// and survives camera motion. A skinned draw, or one whose reuse cannot be
// proved, carries world-space positions and an identity transform.
struct PicaGiInstance {
    std::shared_ptr<const PicaGiTriangleMesh> Mesh;
    ShadowMatrix WorldFromLocal{};
    ShadowBounds WorldBounds;
    // Geometry version and quantized placement; zero = not reusable.
    uint64_t RigidSignature = 0;

    [[nodiscard]] bool Valid() const noexcept {
        return Mesh != nullptr && !Mesh->Indices.empty() && WorldBounds.Available;
    }
};

// Uses the same typed transform/skeleton contract and packed inputs as the
// shadow casters (VertexGeometry::Pose). Unknown programs yield no instance:
// such surfaces are absent from the field, never guessed.
[[nodiscard]] PicaGiInstance
BuildPicaGiInstance(const PicaShadowGeometryView& geometry, PicaSceneTopology topology,
                    const PicaVertexShaderHookLayout& hooks, uint32_t booleanUniforms,
                    std::span<const uint8_t> floatUniforms, const ShadowMatrix& viewToWorld,
                    uint64_t geometryVersion, PicaGiGeometryCache& cache);

} // namespace Fast::Renderer3ds
