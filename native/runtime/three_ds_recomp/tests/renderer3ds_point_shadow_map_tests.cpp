#include "fast/renderer3ds/point_shadow_map.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace Fast::Renderer3ds;

float Dot(const ShadowVector& left, const ShadowVector& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<float, 4> Transform(const ShadowMatrix& matrix, const ShadowVector& point) {
    std::array<float, 4> result{};
    for (size_t row = 0; row < 4U; ++row)
        result[row] = matrix[row] * point[0] + matrix[4U + row] * point[1] + matrix[8U + row] * point[2] +
                      matrix[12U + row];
    return result;
}

PointShadowSlot Slot(uint64_t identity, ShadowVector position, float radius, float range = 1.0F) {
    return BuildPointShadowSlot({identity, position, radius, 1.0F}, range);
}

// The receiver's face choice (pica_point_shadow): the major axis of the
// light-to-fragment vector, positive faces on ties.
uint32_t ReceiverFace(const ShadowVector& v) {
    const ShadowVector a{std::abs(v[0]), std::abs(v[1]), std::abs(v[2])};
    if (a[0] >= a[1] && a[0] >= a[2]) return v[0] >= 0.0F ? 0U : 1U;
    if (a[1] >= a[2]) return v[1] >= 0.0F ? 2U : 3U;
    return v[2] >= 0.0F ? 4U : 5U;
}

TEST(PointShadowMap, LayoutPacksOneRowOfBorderedFacesPerLight) {
    const auto layout = BuildPointShadowLayout(512U, 4U);
    ASSERT_TRUE(layout.Valid());
    EXPECT_EQ(layout.TileTexels, 512U + 2U * kPointShadowBorderTexels);
    EXPECT_EQ(layout.Width, layout.TileTexels * 6U);
    EXPECT_EQ(layout.Height, layout.TileTexels * 4U);
    EXPECT_FLOAT_EQ(layout.FaceExtent(), 516.0F / 512.0F);
    EXPECT_FALSE(BuildPointShadowLayout(512U, 0U).Valid());
    EXPECT_FALSE(BuildPointShadowLayout(512U, kMaximumPointShadowLights + 1U).Valid());
    EXPECT_FALSE(BuildPointShadowLayout(0U, 4U).Valid());
}

TEST(PointShadowMap, FacesAreOrthonormalAndCoverEveryAxisDirection) {
    const auto& faces = PointShadowFaces();
    for (uint32_t face = 0; face < kPointShadowFaceCount; ++face) {
        const auto& basis = faces[face];
        EXPECT_FLOAT_EQ(Dot(basis.Forward, basis.Forward), 1.0F);
        EXPECT_FLOAT_EQ(Dot(basis.Right, basis.Right), 1.0F);
        EXPECT_FLOAT_EQ(Dot(basis.Up, basis.Up), 1.0F);
        EXPECT_FLOAT_EQ(Dot(basis.Forward, basis.Right), 0.0F);
        EXPECT_FLOAT_EQ(Dot(basis.Forward, basis.Up), 0.0F);
        EXPECT_FLOAT_EQ(Dot(basis.Right, basis.Up), 0.0F);
        // The receiver picks this face for its own forward axis.
        EXPECT_EQ(ReceiverFace(basis.Forward), face);
    }
}

TEST(PointShadowMap, SlotFarPlaneFollowsTheLightRadius) {
    const auto slot = Slot(7U, {1.0F, 2.0F, 3.0F}, 200.0F, 0.5F);
    ASSERT_TRUE(slot.Valid());
    EXPECT_FLOAT_EQ(slot.FarPlane, 100.0F);
    EXPECT_FLOAT_EQ(slot.NearPlane, 100.0F * kPointShadowNearFraction);
    EXPECT_FALSE(Slot(0U, {}, 200.0F).Valid());
    EXPECT_FALSE(Slot(7U, {}, 0.0F).Valid());
    EXPECT_FALSE(Slot(7U, {}, std::nanf("")).Valid());
}

TEST(PointShadowMap, FaceMatrixMatchesTheReceiverProjection) {
    const auto layout = BuildPointShadowLayout(256U, 2U);
    const auto slot = Slot(3U, {10.0F, -4.0F, 25.0F}, 300.0F);
    const float extent = layout.FaceExtent();
    const float a = slot.FarPlane / (slot.FarPlane - slot.NearPlane);
    const float c = slot.FarPlane * slot.NearPlane / (slot.FarPlane - slot.NearPlane);
    const ShadowVector offsets[] = {
        {120.0F, 30.0F, -20.0F}, {-90.0F, 10.0F, 40.0F}, {5.0F, 150.0F, -60.0F},
        {-30.0F, -200.0F, 70.0F}, {20.0F, -15.0F, 80.0F}, {-11.0F, 9.0F, -250.0F}};
    for (const auto& offset : offsets) {
        const uint32_t face = ReceiverFace(offset);
        const auto& basis = PointShadowFaces()[face];
        const ShadowVector world{slot.PositionWorld[0] + offset[0], slot.PositionWorld[1] + offset[1],
                                 slot.PositionWorld[2] + offset[2]};
        const auto clip = Transform(BuildPointShadowFaceMatrix(slot, face, layout), world);
        const float m = Dot(offset, basis.Forward);
        ASSERT_GT(m, 0.0F);
        EXPECT_NEAR(clip[3], m, 1.0e-3F);
        EXPECT_NEAR(clip[0] / clip[3], Dot(offset, basis.Right) / m / extent, 1.0e-5F);
        EXPECT_NEAR(clip[1] / clip[3], Dot(offset, basis.Up) / m / extent, 1.0e-5F);
        EXPECT_NEAR(clip[2] / clip[3], a - c / m, 1.0e-5F);
        // The major axis stays inside the face, the border outside it.
        EXPECT_LE(std::abs(clip[0] / clip[3]) * extent, 1.0F + 1.0e-5F);
        EXPECT_LE(std::abs(clip[1] / clip[3]) * extent, 1.0F + 1.0e-5F);
    }
    const ShadowVector atFar{slot.PositionWorld[0] + slot.FarPlane, slot.PositionWorld[1], slot.PositionWorld[2]};
    const auto far = Transform(BuildPointShadowFaceMatrix(slot, 0U, layout), atFar);
    EXPECT_NEAR(far[2] / far[3], 1.0F, 1.0e-5F);
    const ShadowVector atNear{slot.PositionWorld[0] + slot.NearPlane, slot.PositionWorld[1], slot.PositionWorld[2]};
    const auto near = Transform(BuildPointShadowFaceMatrix(slot, 0U, layout), atNear);
    EXPECT_NEAR(near[2] / near[3], 0.0F, 1.0e-5F);
}

TEST(PointShadowMap, FaceCullingKeepsOnlyBoundsInsideTheFrustumAndReach) {
    const auto layout = BuildPointShadowLayout(256U, 1U);
    const auto slot = Slot(1U, {0.0F, 0.0F, 0.0F}, 100.0F);
    const ShadowBounds ahead{{40.0F, -5.0F, -5.0F}, {50.0F, 5.0F, 5.0F}, true};
    EXPECT_TRUE(PointShadowFaceIntersects(slot, 0U, layout, ahead));
    EXPECT_FALSE(PointShadowFaceIntersects(slot, 1U, layout, ahead));
    EXPECT_FALSE(PointShadowFaceIntersects(slot, 2U, layout, ahead));
    const ShadowBounds beyond{{120.0F, -5.0F, -5.0F}, {130.0F, 5.0F, 5.0F}, true};
    for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
        EXPECT_FALSE(PointShadowFaceIntersects(slot, face, layout, beyond));
    // A box around the light reaches every face.
    const ShadowBounds around{{-10.0F, -10.0F, -10.0F}, {10.0F, 10.0F, 10.0F}, true};
    for (uint32_t face = 0; face < kPointShadowFaceCount; ++face)
        EXPECT_TRUE(PointShadowFaceIntersects(slot, face, layout, around));
    // Unknown bounds are never culled.
    EXPECT_TRUE(PointShadowFaceIntersects(slot, 3U, layout, ShadowBounds{}));
}

TEST(PointShadowMap, SelectionPrefersNearReachThenBrightnessAndKeepsRows) {
    const ShadowVector camera{0.0F, 0.0F, 0.0F};
    const std::array<PointShadowLight, 4> lights{{
        {11U, {500.0F, 0.0F, 0.0F}, 100.0F, 1.0F},  // reach 400 away
        {12U, {50.0F, 0.0F, 0.0F}, 100.0F, 0.5F},   // camera inside its reach
        {13U, {-60.0F, 0.0F, 0.0F}, 100.0F, 2.0F},  // inside, brighter
        {14U, {1000.0F, 0.0F, 0.0F}, 50.0F, 9.0F},  // far
    }};
    const auto first = SelectPointShadowSlots(lights, camera, 1.0F, 2U, {});
    EXPECT_EQ(first[0].SourceIdentity, 13U);
    EXPECT_EQ(first[1].SourceIdentity, 12U);
    EXPECT_EQ(first[2].SourceIdentity, 0U);
    // Light 12 leaves, light 11 enters: 13 keeps row 0, 11 takes the free row.
    const std::array<PointShadowLight, 3> next{{lights[0], lights[2], lights[3]}};
    const auto second = SelectPointShadowSlots(next, camera, 1.0F, 2U, first);
    EXPECT_EQ(second[0].SourceIdentity, 13U);
    EXPECT_EQ(second[1].SourceIdentity, 11U);
    // A kept light keeps its row even when it ranks differently.
    PointShadowSlots swapped{};
    swapped[0] = first[1];
    swapped[1] = first[0];
    const auto stable = SelectPointShadowSlots(lights, camera, 1.0F, 2U, swapped);
    EXPECT_EQ(stable[0].SourceIdentity, 12U);
    EXPECT_EQ(stable[1].SourceIdentity, 13U);
    // Duplicates and invalid lights never take a row.
    const std::array<PointShadowLight, 3> duplicates{{lights[1], lights[1], {15U, {}, 0.0F, 1.0F}}};
    const auto unique = SelectPointShadowSlots(duplicates, camera, 1.0F, 4U, {});
    EXPECT_EQ(unique[0].SourceIdentity, 12U);
    EXPECT_EQ(unique[1].SourceIdentity, 0U);
}

TEST(PointShadowMap, ReceiverProgramReadsTheDeclaredAtlasAndBlock) {
    const std::string source(PointShadowReceiverShader());
    EXPECT_NE(source.find("layout(set=0,binding=27) uniform sampler2D pica_point_shadow_atlas;"),
              std::string::npos);
    EXPECT_NE(source.find("float pica_point_shadow(float slot, vec3 light_to_fragment, float radius)"),
              std::string::npos);
    EXPECT_NE(source.find("pica_surface.point_shadow_atlas"), std::string::npos);
    // The GLSL face table is the C++ one, entry by entry.
    const auto component = [](float value) {
        return value == 0.0F ? std::string("0.0") : (value > 0.0F ? std::string("1.0") : std::string("-1.0"));
    };
    const auto table = [&](const char* name) {
        const std::string head = std::string("const vec3 ") + name + "[6] = vec3[6](";
        const size_t begin = source.find(head);
        if (begin == std::string::npos) return std::string();
        const size_t end = source.find(");", begin);
        std::string compact;
        for (const char c : source.substr(begin + head.size(), end - begin - head.size()))
            if (c != ' ' && c != '\n') compact += c;
        return compact;
    };
    const auto expected = [&](ShadowVector PointShadowFace::*axis) {
        std::string result;
        for (uint32_t face = 0; face < kPointShadowFaceCount; ++face) {
            const auto& v = PointShadowFaces()[face].*axis;
            result += (face == 0U ? "" : ",") + std::string("vec3(") + component(v[0]) + "," + component(v[1]) +
                      "," + component(v[2]) + ")";
        }
        return result;
    };
    EXPECT_EQ(table("pica_point_face_forward"), expected(&PointShadowFace::Forward));
    EXPECT_EQ(table("pica_point_face_right"), expected(&PointShadowFace::Right));
    EXPECT_EQ(table("pica_point_face_up"), expected(&PointShadowFace::Up));
}

} // namespace
