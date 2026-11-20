#include "fast/renderer3ds/pica_gi_geometry.h"
#include "fast/renderer3ds/pica_vertex_geometry.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace Fast::Renderer3ds {
namespace {
using namespace VertexGeometry;

uint64_t Hash(uint64_t hash, uint64_t value) noexcept {
    for (size_t byte = 0; byte < sizeof(value); ++byte) {
        hash = (hash ^ uint8_t(value)) * 1099511628211ULL;
        value >>= 8;
    }
    return hash;
}

// Vertex referenced by the item-th element of the draw (index or sequence).
bool ReadVertex(const PicaShadowGeometryView& geometry, uint32_t item, uint32_t& vertex) {
    uint32_t raw = item;
    if (geometry.Indexed) {
        const uint64_t offset = geometry.IndexOffset + uint64_t(item) * sizeof(uint16_t);
        if (offset > geometry.Payload.size() || sizeof(uint16_t) > geometry.Payload.size() - offset)
            return false;
        uint16_t index;
        std::memcpy(&index, geometry.Payload.data() + offset, sizeof(index));
        raw = index;
    }
    const int64_t adjusted = int64_t(raw) + geometry.BaseVertex;
    if (adjusted < 0 || adjusted > std::numeric_limits<uint32_t>::max())
        return false;
    vertex = static_cast<uint32_t>(adjusted);
    return true;
}

void Extend(ShadowBounds& bounds, const ShadowVector& point) {
    if (!bounds.Available) {
        bounds.Minimum = bounds.Maximum = point;
        bounds.Available = true;
        return;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        bounds.Minimum[axis] = std::min(bounds.Minimum[axis], point[axis]);
        bounds.Maximum[axis] = std::max(bounds.Maximum[axis], point[axis]);
    }
}

// Rigid placement signature: geometry version plus the quantized world
// transform, with the same bins as the shadow-caster reuse rule.
uint64_t PlacementSignature(uint64_t geometryVersion, const ShadowMatrix& world) {
    uint64_t signature = Hash(1469598103934665603ULL, geometryVersion);
    for (size_t index = 0; index < world.size(); ++index) {
        const double scale = index >= 12 ? 64.0 : 65536.0;
        const double value = double(world[index]) * scale;
        if (!std::isfinite(value) || std::abs(value) > 9.0e15)
            return 0;
        signature = Hash(signature, static_cast<uint64_t>(static_cast<int64_t>(std::llround(value))));
    }
    return signature == 0 ? 1 : signature;
}

// Builds the mesh from per-vertex positions supplied by `position(vertex, out)`.
template <typename PositionOf>
std::shared_ptr<PicaGiTriangleMesh> BuildMesh(const PicaShadowGeometryView& geometry, PicaSceneTopology topology,
                                              PositionOf&& position) {
    auto mesh = std::make_shared<PicaGiTriangleMesh>();
    std::unordered_map<uint32_t, uint32_t> compact;
    compact.reserve(geometry.VertexOrIndexCount);
    std::vector<uint32_t> items;
    items.reserve(geometry.VertexOrIndexCount);
    for (uint32_t item = 0; item < geometry.VertexOrIndexCount; ++item) {
        uint32_t vertex;
        if (!ReadVertex(geometry, item, vertex))
            return nullptr;
        const auto [found, inserted] = compact.try_emplace(vertex, static_cast<uint32_t>(mesh->Positions.size()));
        if (inserted) {
            ShadowVector point;
            if (!position(vertex, point))
                return nullptr;
            mesh->Positions.push_back(point);
            Extend(mesh->LocalBounds, point);
        }
        items.push_back(found->second);
    }
    const auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
        // Degenerate strip joints carry no surface.
        if (a == b || b == c || a == c)
            return;
        mesh->Indices.insert(mesh->Indices.end(), { a, b, c });
    };
    if (topology == PicaSceneTopology::TriangleStrip) {
        for (size_t i = 2; i < items.size(); ++i)
            emit(items[i - 2], items[i - 1], items[i]);
    } else {
        for (size_t i = 2; i < items.size(); i += 3)
            emit(items[i - 2], items[i - 1], items[i]);
    }
    return mesh;
}

} // namespace

PicaGiInstance BuildPicaGiInstance(const PicaShadowGeometryView& geometry, PicaSceneTopology topology,
                                   const PicaVertexShaderHookLayout& hooks, uint32_t booleanUniforms,
                                   std::span<const uint8_t> floatUniforms, const ShadowMatrix& viewToWorld,
                                   uint64_t geometryVersion, PicaGiGeometryCache& cache) {
    const auto* transform = hooks.TransformProgram();
    if (!transform || geometry.VertexOrIndexCount < 3)
        return {};
    Pose pose(hooks, booleanUniforms, floatUniforms, viewToWorld);
    if (!pose.Valid)
        return {};
    const auto positions = ResolveAttribute(geometry, transform->PositionInputRegister);

    PicaGiInstance instance;
    if (!pose.Skinned && geometryVersion != 0) {
        ShadowMatrix view, model;
        if (!UniformMatrix(floatUniforms, transform->ViewFirstUniform, view) ||
            !UniformMatrix(floatUniforms, transform->ModelFirstUniform, model))
            return {};
        const auto world = Multiply(Multiply(viewToWorld, view), model);
        std::shared_ptr<const PicaGiTriangleMesh> mesh =
            cache.Version == geometryVersion ? cache.Mesh : nullptr;
        bool homogeneous = true;
        if (mesh == nullptr) {
            auto built = BuildMesh(geometry, topology, [&](uint32_t vertex, ShadowVector& point) {
                Vec4 value;
                if (!positions.Read(vertex, value))
                    return false;
                homogeneous &= value[3] == 1.0F;
                point = { value[0], value[1], value[2] };
                return true;
            });
            if (built != nullptr && homogeneous) {
                mesh = std::move(built);
                cache = { geometryVersion, mesh };
            }
        }
        if (mesh != nullptr) {
            instance.Mesh = mesh;
            instance.WorldFromLocal = world;
            instance.RigidSignature = PlacementSignature(geometryVersion, world);
            const auto& local = mesh->LocalBounds;
            if (local.Available) {
                for (uint32_t corner = 0; corner < 8; ++corner) {
                    const Vec4 point{ corner & 1 ? local.Maximum[0] : local.Minimum[0],
                                      corner & 2 ? local.Maximum[1] : local.Minimum[1],
                                      corner & 4 ? local.Maximum[2] : local.Minimum[2], 1.0F };
                    Extend(instance.WorldBounds, Transform(world, point));
                }
            }
            return instance;
        }
        // Non-homogeneous positions: evaluate through the full pose below.
    }

    const auto* skeleton = hooks.SkeletonProgram();
    const auto boneIndices = pose.Skinned ? ResolveAttribute(geometry, skeleton->BoneIndexInputRegister)
                                          : AttributeStream{};
    const auto boneWeights = pose.Multiple ? ResolveAttribute(geometry, skeleton->BoneWeightInputRegister)
                                           : AttributeStream{};
    auto mesh = BuildMesh(geometry, topology, [&](uint32_t vertex, ShadowVector& point) {
        Vec4 value, indices{}, weights{};
        if (!positions.Read(vertex, value))
            return false;
        if (pose.Skinned && (!boneIndices.Read(vertex, indices) ||
                             (pose.Multiple && !boneWeights.Read(vertex, weights))))
            return false;
        return pose.Position(value, indices, weights, point);
    });
    if (mesh == nullptr)
        return {};
    instance.WorldBounds = mesh->LocalBounds;
    instance.Mesh = std::move(mesh);
    instance.WorldFromLocal = Identity();
    return instance;
}

} // namespace Fast::Renderer3ds
