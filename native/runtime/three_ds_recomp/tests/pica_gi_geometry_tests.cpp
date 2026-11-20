#include "fast/renderer3ds/pica_gi_geometry.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace Fast::Renderer3ds;

int gFailures = 0;
void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "pica_gi_geometry_tests: %s\n", message);
        ++gFailures;
    }
}

struct Fixture {
    PicaVertexShaderHookLayout Hooks;
    std::array<std::array<float, 4>, 96> Uniforms{};
    std::vector<uint8_t> Payload;
    std::array<PicaSceneVertexBufferBinding, 1> Bindings{ { { 0, 0, 64, 16 } } };
    std::array<PicaNriPackedVertexAttribute, 1> Attributes{ { { 0, 0, 0 } } };
    std::array<PicaNriPackedVertexBinding, 1> Inputs{ { { 0, 16, false } } };
    ShadowMatrix ViewToWorld{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 0, 0, 1 };

    Fixture(std::array<float, 4> w = { 1, 1, 1, 1 }) {
        Hooks.Transform = { PicaVertexTransformOperation::ModelViewProjection3x4, 0, 4, 4, 3, 20, 3, 0, 1 };
        for (const int first : { 4, 20 })
            for (int axis = 0; axis < 3; ++axis) Uniforms[first + axis][axis] = 1;
        Uniforms[20][3] = 10; // model translation x
        const std::array<std::array<float, 4>, 4> vertices{ { { 0, 0, 0, w[0] },
                                                              { 1, 0, 0, w[1] },
                                                              { 0, 1, 0, w[2] },
                                                              { 1, 1, 0, w[3] } } };
        Payload.resize(sizeof(vertices));
        std::memcpy(Payload.data(), vertices.data(), sizeof(vertices));
    }
    void SetIndices(std::initializer_list<uint16_t> indices) {
        Payload.resize(64);
        for (const uint16_t index : indices) {
            Payload.push_back(uint8_t(index));
            Payload.push_back(uint8_t(index >> 8));
        }
    }
    PicaShadowGeometryView View(uint32_t count, bool indexed = true) const {
        return { Payload, Bindings, Attributes, Inputs, 0, 64, count, 0, indexed };
    }
    std::span<const uint8_t> Bytes() const {
        return { reinterpret_cast<const uint8_t*>(Uniforms.data()), sizeof(Uniforms) };
    }
    PicaGiInstance Build(PicaSceneTopology topology, uint32_t count, PicaGiGeometryCache& cache,
                         uint64_t version = 7) const {
        return BuildPicaGiInstance(View(count), topology, Hooks, 0, Bytes(), ViewToWorld, version, cache);
    }
};

void TestRigidStripIsModelSpaceWithPlacement() {
    Fixture fixture;
    fixture.SetIndices({ 0, 1, 2, 3 });
    PicaGiGeometryCache cache;
    const auto first = fixture.Build(PicaSceneTopology::TriangleStrip, 4, cache);
    Require(first.Valid(), "rigid strip produced no instance");
    Require(first.Mesh->Positions.size() == 4U && first.Mesh->Indices.size() == 6U,
            "strip did not become two list triangles");
    Require(first.WorldFromLocal[12] == 110.0F, "model-to-world translation is not view-to-world * model");
    Require(first.WorldBounds.Minimum[0] == 110.0F && first.WorldBounds.Maximum[0] == 111.0F,
            "world bounds do not follow the placement");
    Require(first.RigidSignature != 0U, "rigid placement has no signature");

    fixture.Uniforms[20][3] = 20;
    const auto moved = fixture.Build(PicaSceneTopology::TriangleStrip, 4, cache);
    Require(moved.Mesh == first.Mesh, "a moved rigid draw rebuilt its model-space mesh");
    Require(moved.RigidSignature != first.RigidSignature, "a moved placement kept its signature");
    Require(moved.WorldFromLocal[12] == 120.0F, "moved placement transform is wrong");

    const auto other = fixture.Build(PicaSceneTopology::TriangleStrip, 4, cache, 8);
    Require(other.Mesh != first.Mesh, "a new geometry version reused the old mesh");
}

void TestDegenerateStripAndList() {
    Fixture fixture;
    fixture.SetIndices({ 0, 1, 1, 2 });
    PicaGiGeometryCache cache;
    Require(!fixture.Build(PicaSceneTopology::TriangleStrip, 4, cache).Valid(),
            "a degenerate strip produced surface triangles");
    fixture.SetIndices({ 0, 1, 2, 2, 1, 3 });
    PicaGiGeometryCache listCache;
    const auto list = fixture.Build(PicaSceneTopology::TriangleList, 6, listCache);
    Require(list.Valid() && list.Mesh->Indices.size() == 6U && list.Mesh->Positions.size() == 4U,
            "triangle list lost triangles or duplicated vertices");
}

void TestUnprovenReuseIsWorldSpace() {
    Fixture fixture({ 1, 1, 1, 0.5F });
    fixture.SetIndices({ 0, 1, 2, 3 });
    PicaGiGeometryCache cache;
    const auto instance = fixture.Build(PicaSceneTopology::TriangleStrip, 4, cache);
    Require(instance.Valid(), "non-homogeneous rigid draw produced no instance");
    Require(instance.RigidSignature == 0U && instance.WorldFromLocal[12] == 0.0F,
            "non-homogeneous positions were treated as reusable model space");
    Require(instance.Mesh->Positions[1][0] > 100.0F, "world-space positions were not evaluated");
    Require(cache.Mesh == nullptr, "an unprovable mesh entered the reuse cache");

    PicaGiGeometryCache unversioned;
    Fixture rigid;
    rigid.SetIndices({ 0, 1, 2, 3 });
    const auto transient = rigid.Build(PicaSceneTopology::TriangleStrip, 4, unversioned, 0);
    Require(transient.Valid() && transient.RigidSignature == 0U, "an unversioned draw claimed reuse");
}

void TestUnknownProgramIsAbsent() {
    Fixture fixture;
    fixture.SetIndices({ 0, 1, 2, 3 });
    fixture.Hooks = {};
    PicaGiGeometryCache cache;
    Require(!fixture.Build(PicaSceneTopology::TriangleStrip, 4, cache).Valid(),
            "a program without a typed transform produced geometry");
}

} // namespace

int main() {
    TestRigidStripIsModelSpaceWithPlacement();
    TestDegenerateStripAndList();
    TestUnprovenReuseIsWorldSpace();
    TestUnknownProgramIsAbsent();
    if (gFailures != 0)
        return 1;
    std::puts("pica_gi_geometry_tests: ok");
    return 0;
}
