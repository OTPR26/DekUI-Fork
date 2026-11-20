#include "fast/renderer3ds/point_shadow_map.h"

#include <algorithm>
#include <cmath>

namespace Fast::Renderer3ds {
namespace {

float Dot(const ShadowVector& left, const ShadowVector& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

ShadowVector Subtract(const ShadowVector& left, const ShadowVector& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

ShadowVector Combine(const ShadowVector& forward, float scale, const ShadowVector& side, float sign) noexcept {
    return {forward[0] * scale + side[0] * sign, forward[1] * scale + side[1] * sign,
            forward[2] * scale + side[2] * sign};
}

// The bounds corner farthest along `normal`.
ShadowVector SupportCorner(const ShadowBounds& bounds, const ShadowVector& normal) noexcept {
    return {normal[0] >= 0.0F ? bounds.Maximum[0] : bounds.Minimum[0],
            normal[1] >= 0.0F ? bounds.Maximum[1] : bounds.Minimum[1],
            normal[2] >= 0.0F ? bounds.Maximum[2] : bounds.Minimum[2]};
}

float DistanceToBounds(const ShadowVector& point, const ShadowBounds& bounds) noexcept {
    float squared = 0.0F;
    for (size_t axis = 0; axis < 3U; ++axis) {
        const float below = bounds.Minimum[axis] - point[axis];
        const float above = point[axis] - bounds.Maximum[axis];
        const float outside = std::max({below, above, 0.0F});
        squared += outside * outside;
    }
    return std::sqrt(squared);
}

bool Finite(const ShadowVector& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

} // namespace

const std::array<PointShadowFace, kPointShadowFaceCount>& PointShadowFaces() noexcept {
    static constexpr std::array<PointShadowFace, kPointShadowFaceCount> kFaces{{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}},
        {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}},
        {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
        {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
    }};
    return kFaces;
}

PointShadowLayout BuildPointShadowLayout(uint32_t faceTexels, uint32_t slotCount) noexcept {
    PointShadowLayout layout;
    if (faceTexels == 0U || slotCount == 0U || slotCount > kMaximumPointShadowLights) return layout;
    layout.FaceTexels = faceTexels;
    layout.TileTexels = faceTexels + 2U * kPointShadowBorderTexels;
    layout.SlotCount = slotCount;
    layout.Width = layout.TileTexels * kPointShadowFaceCount;
    layout.Height = layout.TileTexels * slotCount;
    return layout;
}

PointShadowSlot BuildPointShadowSlot(const PointShadowLight& light, float range) noexcept {
    PointShadowSlot slot;
    if (light.SourceIdentity == 0U || !Finite(light.PositionWorld) || !std::isfinite(light.Radius) ||
        light.Radius <= 0.0F || !std::isfinite(range) || range <= 0.0F)
        return slot;
    slot.SourceIdentity = light.SourceIdentity;
    slot.PositionWorld = light.PositionWorld;
    slot.Radius = light.Radius;
    slot.FarPlane = light.Radius * range;
    slot.NearPlane = slot.FarPlane * kPointShadowNearFraction;
    return slot;
}

PointShadowSlots SelectPointShadowSlots(
    std::span<const PointShadowLight> candidates, const ShadowVector& camera, float range,
    uint32_t capacity, const PointShadowSlots& previous) noexcept {
    PointShadowSlots result{};
    capacity = std::min(capacity, kMaximumPointShadowLights);
    struct Ranked {
        PointShadowSlot Slot;
        float Distance = 0.0F;
        float Luminance = 0.0F;
    };
    std::array<Ranked, 64> ranked{};
    size_t count = 0;
    for (const auto& light : candidates) {
        const auto slot = BuildPointShadowSlot(light, range);
        if (!slot.Valid() || count == ranked.size() ||
            std::any_of(ranked.begin(), ranked.begin() + count, [&](const Ranked& entry) {
                return entry.Slot.SourceIdentity == slot.SourceIdentity;
            }))
            continue;
        const auto offset = Subtract(slot.PositionWorld, camera);
        ranked[count++] = {slot, std::max(0.0F, std::sqrt(Dot(offset, offset)) - slot.FarPlane),
                           light.Luminance};
    }
    std::sort(ranked.begin(), ranked.begin() + count, [](const Ranked& left, const Ranked& right) {
        if (left.Distance != right.Distance) return left.Distance < right.Distance;
        if (left.Luminance != right.Luminance) return left.Luminance > right.Luminance;
        return left.Slot.SourceIdentity < right.Slot.SourceIdentity;
    });
    count = std::min<size_t>(count, capacity);
    std::array<bool, 64> placed{};
    // Lights that keep their place keep their row.
    for (uint32_t row = 0; row < capacity; ++row) {
        for (size_t index = 0; index < count; ++index) {
            if (!placed[index] && previous[row].SourceIdentity != 0U &&
                previous[row].SourceIdentity == ranked[index].Slot.SourceIdentity) {
                result[row] = ranked[index].Slot;
                placed[index] = true;
                break;
            }
        }
    }
    uint32_t row = 0;
    for (size_t index = 0; index < count; ++index) {
        if (placed[index]) continue;
        while (row < capacity && result[row].SourceIdentity != 0U) ++row;
        if (row == capacity) break;
        result[row] = ranked[index].Slot;
        placed[index] = true;
    }
    return result;
}

ShadowMatrix BuildPointShadowFaceMatrix(
    const PointShadowSlot& slot, uint32_t face, const PointShadowLayout& layout) noexcept {
    ShadowMatrix matrix{};
    if (face >= kPointShadowFaceCount || !slot.Valid() || !layout.Valid()) return matrix;
    const auto& basis = PointShadowFaces()[face];
    const float extent = layout.FaceExtent();
    const float depthScale = slot.FarPlane / (slot.FarPlane - slot.NearPlane);
    const float depthOffset = slot.FarPlane * slot.NearPlane / (slot.FarPlane - slot.NearPlane);
    const auto& light = slot.PositionWorld;
    const auto row = [&](size_t index, const ShadowVector& axis, float scale, float translation) {
        for (size_t column = 0; column < 3U; ++column) matrix[column * 4U + index] = axis[column] * scale;
        matrix[12U + index] = translation;
    };
    row(0U, basis.Right, 1.0F / extent, -Dot(basis.Right, light) / extent);
    row(1U, basis.Up, 1.0F / extent, -Dot(basis.Up, light) / extent);
    row(2U, basis.Forward, depthScale, -depthScale * Dot(basis.Forward, light) - depthOffset);
    row(3U, basis.Forward, 1.0F, -Dot(basis.Forward, light));
    return matrix;
}

bool PointShadowFaceIntersects(
    const PointShadowSlot& slot, uint32_t face, const PointShadowLayout& layout,
    const ShadowBounds& bounds) noexcept {
    if (face >= kPointShadowFaceCount || !slot.Valid() || !layout.Valid()) return false;
    if (!bounds.Available) return true;
    if (DistanceToBounds(slot.PositionWorld, bounds) > slot.FarPlane) return false;
    const auto& basis = PointShadowFaces()[face];
    const float extent = layout.FaceExtent();
    const std::array<ShadowVector, 5> planes{
        Combine(basis.Forward, extent, basis.Right, 1.0F), Combine(basis.Forward, extent, basis.Right, -1.0F),
        Combine(basis.Forward, extent, basis.Up, 1.0F), Combine(basis.Forward, extent, basis.Up, -1.0F),
        basis.Forward};
    for (size_t index = 0; index < planes.size(); ++index) {
        const auto corner = Subtract(SupportCorner(bounds, planes[index]), slot.PositionWorld);
        const float limit = index == 4U ? slot.NearPlane : 0.0F;
        if (Dot(corner, planes[index]) < limit) return false;
    }
    return true;
}

std::string_view PointShadowReceiverShader() noexcept {
    static_assert(kPointShadowFaceCount == 6U);
    return R"glsl(
layout(set=0,binding=27) uniform sampler2D pica_point_shadow_atlas;
const vec3 pica_point_face_forward[6] = vec3[6](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
const vec3 pica_point_face_right[6] = vec3[6](vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0),
    vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0));
const vec3 pica_point_face_up[6] = vec3[6](vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0));
float pica_point_axis_weight(int offset, float fraction) {
    return offset == -1 ? 1.0 - fraction : (offset == 2 ? fraction : 1.0);
}
// Visibility of a point light (1 = lit) for the light-to-fragment world vector.
// Rows are light slots, columns cube faces; a face tile includes its border.
float pica_point_shadow(float slot, vec3 light_to_fragment, float radius) {
    vec4 shadow = pica_surface.point_shadow;      // range, strength, face texels, border texels
    vec4 atlas = pica_surface.point_shadow_atlas; // 1/width, 1/height, near fraction, enabled
    if (slot < 0.0 || atlas.w == 0.0 || radius <= 0.0) return 1.0;
    vec3 a = abs(light_to_fragment);
    int face = a.x >= a.y && a.x >= a.z ? (light_to_fragment.x >= 0.0 ? 0 : 1)
             : (a.y >= a.z ? (light_to_fragment.y >= 0.0 ? 2 : 3) : (light_to_fragment.z >= 0.0 ? 4 : 5));
    float far_plane = radius * shadow.x;
    float near_plane = far_plane * atlas.z;
    float m = dot(light_to_fragment, pica_point_face_forward[face]);
    if (m <= near_plane || m >= far_plane) return 1.0;
    float tile = shadow.z + 2.0 * shadow.w;
    float extent = tile / shadow.z;
    vec2 tangent = vec2(dot(light_to_fragment, pica_point_face_right[face]),
                        dot(light_to_fragment, pica_point_face_up[face])) / m;
    vec2 position = (tangent / extent * 0.5 + 0.5) * tile + vec2(float(face), slot) * tile - vec2(0.5);
    vec2 base = floor(position);
    vec2 fraction = position - base;
    float c = far_plane * near_plane / (far_plane - near_plane);
    // Receiver offset toward the light: one and a half texels at this distance.
    float texel_world = 2.0 * extent * m / tile;
    float depth = far_plane / (far_plane - near_plane) - c / m - c / (m * m) * 1.5 * texel_world;
    float visibility = 0.0;
    for (int iy = 0; iy < 2; ++iy) {
        int y = -1 + iy * 2;
        vec2 wy = vec2(pica_point_axis_weight(y, fraction.y), pica_point_axis_weight(y + 1, fraction.y));
        for (int ix = 0; ix < 2; ++ix) {
            int x = -1 + ix * 2;
            vec2 wx = vec2(pica_point_axis_weight(x, fraction.x), pica_point_axis_weight(x + 1, fraction.x));
            vec4 weights = vec4(wx.x * wy.y, wx.y * wy.y, wx.y * wy.x, wx.x * wy.x);
            vec2 uv = (base + vec2(x + 1, y + 1)) * atlas.xy;
            visibility += dot(step(vec4(depth), textureGather(pica_point_shadow_atlas, uv, 0)), weights);
        }
    }
    visibility /= 9.0;
    // The shadow ends with the light's reach.
    float fade = smoothstep(0.9 * far_plane, far_plane, m);
    return 1.0 - (1.0 - visibility) * clamp(shadow.y, 0.0, 1.0) * (1.0 - fade);
}
)glsl";
}

} // namespace Fast::Renderer3ds
