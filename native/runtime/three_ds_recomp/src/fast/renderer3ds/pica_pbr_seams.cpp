#include "fast/renderer3ds/pica_pbr_seams.h"
#include "fast/renderer3ds/pica_texture_coordinates.h"
#include <bit>
#include <map>
#include <numeric>
#include <stdexcept>

namespace Fast::Renderer3ds {
namespace {
using namespace VertexGeometry;
using V3 = std::array<float, 3>;
V3 Sub(const V3& a, const V3& b) {
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}
V3 Cross(const V3& a, const V3& b) {
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
float Dot(const V3& a, const V3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 Xyz(const Vec4& v) {
    return { v[0], v[1], v[2] };
}
float Bits(uint32_t value) {
    return std::bit_cast<float>(value);
}
void Hash(uint64_t& hash, const void* data, size_t size) {
    for (auto b : std::span(static_cast<const uint8_t*>(data), size))
        hash = (hash ^ b) * 1099511628211ULL;
}
struct Vertex {
    Vec4 Position{}, Normal{}, Uv{}, Indices{}, Weights{};
    uint32_t Weld = 0;
};
struct Triangle {
    std::array<uint32_t, 3> Vertices{}, Neighbors{}, FanOffsets{}, FanCounts{};
    V3 FaceNormal{};
};
struct Edge {
    uint32_t Triangle, Opposite, A, B;
};
bool Smooth(const Vertex& a, const Vertex& b) {
    const auto x = Xyz(a.Normal), y = Xyz(b.Normal);
    const float xx = Dot(x, x), yy = Dot(y, y);
    return xx > 1e-20F && yy > 1e-20F && Dot(x, y) > 0.9999F * std::sqrt(xx * yy);
}
} // namespace
struct PicaPbrSeamCache::Impl {
    uint64_t GeometryVersion = 0, PoseVersion = 0;
    int32_t BaseVertex = 0;
    bool Strip = false, Skinned = false, Multiple = false;
    uint8_t UvInput = 0xff;
    PicaVertexShaderHookLayout Hooks;
    std::vector<Vertex> Vertices;
    std::vector<Triangle> Triangles;
    std::vector<uint32_t> Fans;
    std::array<bool, 96> UsedUniforms{};
    std::vector<V3> Positions;
    std::vector<std::array<float, 2>> Coordinates;
    std::array<float, 6> UvRows{};
    bool UvValid = false;
    PicaPbrSeamData Data;

    bool Build(const PicaShadowGeometryView& geometry, const PicaVertexShaderHookLayout& hooks, const PicaAffineUv& uv,
               const Pose& pose, bool strip) {
        Vertices.clear();
        Triangles.clear();
        Fans.clear();
        Data.Words.clear();
        Data.TriangleCount = Data.SeamCount = 0;
        UvValid = false;
        const auto* transform = hooks.TransformProgram();
        if (!transform || geometry.VertexOrIndexCount < 3)
            return false;
        const auto positions = ResolveAttribute(geometry, transform->PositionInputRegister);
        const auto normals = ResolveAttribute(geometry, transform->NormalInputRegister);
        const auto coords = ResolveAttribute(geometry, uv.InputRegister);
        const auto* skeleton = hooks.SkeletonProgram();
        const auto indices =
            pose.Skinned ? ResolveAttribute(geometry, skeleton->BoneIndexInputRegister) : AttributeStream{};
        const auto weights =
            pose.Multiple ? ResolveAttribute(geometry, skeleton->BoneWeightInputRegister) : AttributeStream{};
        std::map<uint32_t, uint32_t> remap;
        std::map<std::array<float, 12>, uint32_t> welds;
        std::vector<uint32_t> corners;
        corners.reserve(geometry.VertexOrIndexCount);
        for (uint32_t item = 0; item < geometry.VertexOrIndexCount; ++item) {
            uint32_t index = item;
            if (geometry.Indexed) {
                const uint64_t offset = geometry.IndexOffset + uint64_t(item) * 2;
                if (offset > geometry.Payload.size() || 2 > geometry.Payload.size() - offset)
                    return false;
                uint16_t value;
                std::memcpy(&value, geometry.Payload.data() + offset, 2);
                index = value;
            }
            const int64_t adjusted = int64_t(index) + geometry.BaseVertex;
            if (adjusted < 0 || adjusted > UINT32_MAX)
                return false;
            auto [entry, inserted] = remap.try_emplace(uint32_t(adjusted), uint32_t(Vertices.size()));
            if (inserted) {
                Vertex vertex;
                if (!positions.Read(entry->first, vertex.Position) || !normals.Read(entry->first, vertex.Normal) ||
                    !coords.Read(entry->first, vertex.Uv) ||
                    (pose.Skinned && !indices.Read(entry->first, vertex.Indices)) ||
                    (pose.Multiple && !weights.Read(entry->first, vertex.Weights)))
                    return false;
                std::array<float, 12> key{};
                std::copy(vertex.Position.begin(), vertex.Position.end(), key.begin());
                std::copy(vertex.Indices.begin(), vertex.Indices.end(), key.begin() + 4);
                std::copy(vertex.Weights.begin(), vertex.Weights.end(), key.begin() + 8);
                vertex.Weld = welds.try_emplace(key, uint32_t(welds.size())).first->second;
                Vertices.push_back(vertex);
            }
            corners.push_back(entry->second);
        }
        const size_t count = strip ? corners.size() - 2 : corners.size() / 3;
        Triangles.resize(count);
        std::map<std::pair<uint32_t, uint32_t>, std::vector<Edge>> edges;
        for (uint32_t t = 0; t < count; ++t) {
            auto& triangle = Triangles[t];
            for (uint32_t i = 0; i < 3; ++i)
                triangle.Vertices[i] = corners[(strip ? t : t * 3) + i];
            if (strip && (t & 1))
                std::swap(triangle.Vertices[0], triangle.Vertices[1]);
            const auto& v = triangle.Vertices;
            const V3 n = Cross(Sub(Xyz(Vertices[v[1]].Position), Xyz(Vertices[v[0]].Position)),
                               Sub(Xyz(Vertices[v[2]].Position), Xyz(Vertices[v[0]].Position)));
            if (Dot(n, n) <= 1e-20F)
                continue;
            triangle.FaceNormal = n;
            for (uint32_t opposite = 0; opposite < 3; ++opposite) {
                const auto a = Vertices[v[(opposite + 1) % 3]].Weld, b = Vertices[v[(opposite + 2) % 3]].Weld;
                if (a != b)
                    edges[std::minmax(a, b)].push_back({ t, opposite, a, b });
            }
        }
        std::vector<uint32_t> parents(count * 3);
        std::iota(parents.begin(), parents.end(), 0);
        auto root = [&](uint32_t x) {
            while (parents[x] != x) {
                parents[x] = parents[parents[x]];
                x = parents[x];
            }
            return x;
        };
        auto join = [&](uint32_t a, uint32_t b) { parents[root(a)] = root(b); };
        for (const auto& [key, pair] : edges) {
            if (pair.size() != 2)
                continue; // Open/non-manifold/overlapping ownership is not inferred.
            const auto& a = pair[0];
            const auto& b = pair[1];
            if (a.A != b.B || a.B != b.A || a.Triangle == b.Triangle)
                continue;
            auto& ta = Triangles[a.Triangle];
            auto& tb = Triangles[b.Triangle];
            // Opposing coplanar faces are overlapping layers, not a UV seam.
            const auto& na = ta.FaceNormal;
            const auto& nb = tb.FaceNormal;
            if (Dot(na, nb) <= -0.9999F * std::sqrt(Dot(na, na) * Dot(nb, nb)))
                continue;
            const uint32_t a0 = (a.Opposite + 1) % 3, a1 = (a.Opposite + 2) % 3, b0 = (b.Opposite + 2) % 3,
                           b1 = (b.Opposite + 1) % 3;
            if (!Smooth(Vertices[ta.Vertices[a0]], Vertices[tb.Vertices[b0]]) ||
                !Smooth(Vertices[ta.Vertices[a1]], Vertices[tb.Vertices[b1]]))
                continue;
            join(a.Triangle * 3 + a0, b.Triangle * 3 + b0);
            join(a.Triangle * 3 + a1, b.Triangle * 3 + b1);
            const auto different = [&](uint32_t x, uint32_t y) {
                return Vertices[x].Uv[0] != Vertices[y].Uv[0] || Vertices[x].Uv[1] != Vertices[y].Uv[1];
            };
            if (different(ta.Vertices[a0], tb.Vertices[b0]) || different(ta.Vertices[a1], tb.Vertices[b1])) {
                ta.Neighbors[a.Opposite] = b.Triangle * 3 + b.Opposite + 1;
                tb.Neighbors[b.Opposite] = a.Triangle * 3 + a.Opposite + 1;
                ++Data.SeamCount;
            }
        }
        if (!Data.SeamCount) {
            Data.Status = "No continuous UV seams in this native geometry";
            return true;
        }
        std::map<uint32_t, std::vector<uint32_t>> groups;
        for (uint32_t corner = 0; corner < parents.size(); ++corner)
            groups[root(corner)].push_back(corner);
        std::map<uint32_t, uint32_t> offsets;
        for (auto& triangle : Triangles)
            for (uint32_t edge = 0; edge < 3; ++edge) {
                if (!triangle.Neighbors[edge])
                    continue;
                const uint32_t t = uint32_t(&triangle - Triangles.data());
                for (uint32_t side = 1; side < 3; ++side) {
                    const uint32_t c = (edge + side) % 3, r = root(t * 3 + c);
                    auto [entry, inserted] = offsets.try_emplace(r, uint32_t(Fans.size()));
                    if (inserted) {
                        const auto& group = groups.at(r);
                        Fans.insert(Fans.end(), group.begin(), group.end());
                    }
                    triangle.FanOffsets[c] = entry->second;
                    triangle.FanCounts[c] = uint32_t(groups.at(r).size());
                }
            }
        Data.Status = "UV seams ready";
        return true;
    }
};
PicaPbrSeamCache::PicaPbrSeamCache() = default;
PicaPbrSeamCache::~PicaPbrSeamCache() = default;
PicaPbrSeamCache::PicaPbrSeamCache(PicaPbrSeamCache&&) noexcept = default;
PicaPbrSeamCache& PicaPbrSeamCache::operator=(PicaPbrSeamCache&&) noexcept = default;
const PicaPbrSeamData& PicaPbrSeamCache::Resolve(const PicaShadowGeometryView& geometry,
                                                 const PicaVertexShaderHookLayout& hooks, uint32_t booleans,
                                                 std::span<const uint8_t> uniforms, uint8_t coordinate, bool strip,
                                                 uint64_t version) {
    if (!State)
        State = std::make_unique<Impl>();
    auto& s = *State;
    auto fail = [&](const char* message) -> const PicaPbrSeamData& {
        s.Data.Words.clear();
        s.Data.TriangleCount = 0;
        s.Data.Status = message;
        s.PoseVersion = 0;
        return s.Data;
    };
    const auto uv = DecodePicaAffineUv(hooks.TextureCoordinate(coordinate), booleans, uniforms);
    if (uv.Status != PicaAffineUvStatus::Applied)
        return fail("UV seam softening requires typed affine texture coordinates");
    Pose pose(hooks, booleans, uniforms);
    if (!pose.Valid)
        return fail("UV seam softening requires valid native transform/skinning data");
    const bool rebuild = !version || s.GeometryVersion != version || s.Hooks != hooks ||
                         s.UvInput != uv.InputRegister || s.Strip != strip || s.BaseVertex != geometry.BaseVertex ||
                         s.Skinned != pose.Skinned || s.Multiple != pose.Multiple;
    if (rebuild) {
        s.GeometryVersion = 0;
        s.PoseVersion = 0;
        if (!s.Build(geometry, hooks, uv, pose, strip))
            return fail("UV seam geometry inputs are incomplete");
        s.GeometryVersion = version;
        s.Hooks = hooks;
        s.UvInput = uv.InputRegister;
        s.Strip = strip;
        s.BaseVertex = geometry.BaseVertex;
        s.Skinned = pose.Skinned;
        s.Multiple = pose.Multiple;
        s.UsedUniforms.fill(false);
        const auto* transform = hooks.TransformProgram();
        for (uint32_t i = 0; i < 3; ++i) {
            s.UsedUniforms[transform->ViewFirstUniform + i] = true;
            s.UsedUniforms[transform->ModelFirstUniform + i] = true;
        }
        if (pose.Skinned) {
            const auto* surface = hooks.SurfaceProgram();
            const auto* skeleton = hooks.SkeletonProgram();
            for (auto component : { surface->SkinnedPositionScale, surface->BoneIndexScale, surface->BoneWeightScale,
                                    surface->InfluenceCount, surface->AdditionalInfluenceThresholds[0],
                                    surface->AdditionalInfluenceThresholds[1] })
                if (component.Available())
                    s.UsedUniforms[component.Uniform] = true;
            // Palette addressing itself is animated native state; include its declared range.
            for (uint32_t i = skeleton->PaletteFirstUniform; i < 96; ++i)
                s.UsedUniforms[i] = true;
        }
    }
    if (!s.Data.SeamCount)
        return s.Data;
    uint64_t signature = 1469598103934665603ULL;
    Hash(signature, uv.Rows.data(), sizeof(uv.Rows));
    for (uint32_t i = 0; i < 96; ++i)
        if (s.UsedUniforms[i])
            Hash(signature, uniforms.data() + i * 16, 16);
    if (signature == s.PoseVersion)
        return s.Data;
    s.Positions.resize(s.Vertices.size());
    s.Coordinates.resize(s.Vertices.size());
    const bool updateUv = !s.UvValid || s.UvRows != uv.Rows || s.Data.Words.empty();
    for (size_t i = 0; i < s.Vertices.size(); ++i) {
        const auto& v = s.Vertices[i];
        if (!pose.Position(v.Position, v.Indices, v.Weights, s.Positions[i]))
            return fail("UV seam pose evaluation failed");
        if (updateUv)
            s.Coordinates[i] = uv.Apply(v.Uv);
    }
    const uint32_t triangleWords = uint32_t(s.Triangles.size()) * kPicaPbrSeamTriangleWords;
    if (updateUv)
        s.Data.Words.assign(triangleWords + (s.Fans.size() + 3) / 4, {});
    for (uint32_t t = 0; t < s.Triangles.size(); ++t) {
        const auto& triangle = s.Triangles[t];
        const auto& v = triangle.Vertices;
        auto* words = s.Data.Words.data() + t * kPicaPbrSeamTriangleWords;
        if (updateUv) {
            const auto& coordinates = s.Coordinates;
            words[0] = { coordinates[v[0]][0], coordinates[v[0]][1], coordinates[v[1]][0], coordinates[v[1]][1] };
            words[1] = { coordinates[v[2]][0], coordinates[v[2]][1], 0, 0 };
        }
        for (uint32_t c = 0; c < 3; ++c) {
            const auto& p = s.Positions[v[c]];
            words[2 + c] = { p[0], p[1], p[2], float(triangle.FanCounts[c]) };
            if (updateUv) {
                words[5][c] = Bits(triangle.Neighbors[c]);
                words[6][c] = Bits(triangle.FanOffsets[c]);
            }
        }
    }
    if (updateUv) {
        for (uint32_t i = 0; i < s.Fans.size(); ++i)
            s.Data.Words[triangleWords + i / 4][i % 4] = Bits(s.Fans[i]);
        s.UvRows = uv.Rows;
        s.UvValid = true;
    }
    s.Data.TriangleCount = uint32_t(s.Triangles.size());
    s.Data.Status = "UV seams ready";
    s.PoseVersion = signature;
    ++s.Data.Revision;
    return s.Data;
}
} // namespace Fast::Renderer3ds
