#pragma once

#include "fast/renderer3ds/pica_shadow_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Fast::Renderer3ds::VertexGeometry {
using Vec4 = std::array<float, 4>;
inline ShadowMatrix Identity() {
    return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}
inline ShadowMatrix Multiply(const ShadowMatrix& a, const ShadowMatrix& b) {
    ShadowMatrix result{};
    for (size_t row = 0; row < 4; ++row)
        for (size_t col = 0; col < 4; ++col) {
            double sum = 0;
            for (size_t k = 0; k < 4; ++k)
                sum += double(a[k * 4 + row]) * b[col * 4 + k];
            result[col * 4 + row] = static_cast<float>(sum);
        }
    return result;
}
inline ShadowVector Transform(const ShadowMatrix& matrix, const Vec4& point) {
    ShadowVector result{};
    for (size_t row = 0; row < 3; ++row) {
        double sum = 0;
        for (size_t col = 0; col < 4; ++col)
            sum += double(matrix[col * 4 + row]) * point[col];
        result[row] = static_cast<float>(sum);
    }
    return result;
}
inline bool UniformMatrix(std::span<const uint8_t> bytes, uint32_t first, ShadowMatrix& result) {
    if (first > 93 || bytes.size() < (first + 3) * sizeof(Vec4))
        return false;
    result = Identity();
    for (size_t row = 0; row < 3; ++row) {
        Vec4 values;
        std::memcpy(values.data(), bytes.data() + (first + row) * sizeof(Vec4), sizeof(values));
        for (size_t col = 0; col < 4; ++col) {
            if (!std::isfinite(values[col]))
                return false;
            result[col * 4 + row] = values[col];
        }
    }
    return true;
}
inline float Scalar(std::span<const uint8_t> bytes, PicaVertexUniformComponent component) {
    if (!component.Available() || bytes.size() < (component.Uniform + 1U) * sizeof(Vec4))
        return std::numeric_limits<float>::quiet_NaN();
    float value;
    std::memcpy(&value, bytes.data() + component.Uniform * sizeof(Vec4) + component.Component * sizeof(float),
                sizeof(value));
    return value;
}
struct AttributeStream {
    std::span<const uint8_t> Bytes;
    uint32_t Stride = 0;
    bool PerInstance = false;
    bool Read(uint32_t vertex, Vec4& result) const {
        const uint64_t offset = PerInstance ? 0 : uint64_t(vertex) * Stride;
        if (Stride == 0 || offset > Bytes.size() || sizeof(result) > Bytes.size() - offset)
            return false;
        std::memcpy(result.data(), Bytes.data() + offset, sizeof(result));
        return std::all_of(result.begin(), result.end(), [](float value) { return std::isfinite(value); });
    }
};
inline AttributeStream ResolveAttribute(const PicaShadowGeometryView& geometry, uint8_t location) {
    const auto a = std::find_if(geometry.Attributes.begin(), geometry.Attributes.end(),
                                [=](const auto& v) { return v.Location == location; });
    if (a == geometry.Attributes.end())
        return {};
    const auto b = std::find_if(geometry.Bindings.begin(), geometry.Bindings.end(),
                                [&](const auto& v) { return v.Binding == a->Binding; });
    if (b == geometry.Bindings.end() || b->Offset < geometry.BufferBaseOffset || b->Stride == 0 ||
        a->ByteOffset > b->Size)
        return {};
    const auto input = std::find_if(geometry.InputBindings.begin(), geometry.InputBindings.end(),
                                    [&](const auto& v) { return v.Binding == a->Binding; });
    const uint64_t base = b->Offset - geometry.BufferBaseOffset;
    if (input == geometry.InputBindings.end() || base > geometry.Payload.size() ||
        a->ByteOffset > geometry.Payload.size() - base)
        return {};
    const uint64_t offset = base + a->ByteOffset;
    return { geometry.Payload.subspan(offset,
                                      std::min<uint64_t>(b->Size - a->ByteOffset, geometry.Payload.size() - offset)),
             b->Stride, input->PerInstance };
}

// Evaluates exactly the typed native rigid/palette position contract. Both
// shadow bounds and UV-seam frames consume this; neither guesses title registers.
class Pose {
  public:
    Pose(const PicaVertexShaderHookLayout& hooks, uint32_t booleans, std::span<const uint8_t> uniforms,
         const ShadowMatrix& viewToWorld = Identity())
        : Uniforms(uniforms) {
        const auto* transform = hooks.TransformProgram();
        Skeleton = hooks.SkeletonProgram();
        const auto* surface = hooks.SurfaceProgram();
        ShadowMatrix view;
        if (!transform || !UniformMatrix(uniforms, transform->ViewFirstUniform, view) ||
            !UniformMatrix(uniforms, transform->ModelFirstUniform, Model))
            return;
        Post = Multiply(viewToWorld, view);
        Skinned = Skeleton && (booleans & (1U << Skeleton->EnableBooleanUniform));
        Multiple = Skinned && Skeleton->MultipleInfluenceBooleanUniform < 16 &&
                   (booleans & (1U << Skeleton->MultipleInfluenceBooleanUniform));
        if (Skinned) {
            if (!surface || !surface->SkinnedNormalsAvailable() || !surface->SkinnedPositionScale.Available())
                return;
            PositionScale = Scalar(uniforms, surface->SkinnedPositionScale);
            IndexScale = Scalar(uniforms, surface->BoneIndexScale);
            WeightScale = Scalar(uniforms, surface->BoneWeightScale);
            if (!std::isfinite(PositionScale) || !std::isfinite(IndexScale) || !std::isfinite(WeightScale))
                return;
            Influences = std::min<uint8_t>(Multiple ? 4 : 1, Skeleton->MaximumInfluences);
            Enabled[1] = Multiple;
            if (Multiple) {
                const float count = Scalar(uniforms, surface->InfluenceCount);
                if (!std::isfinite(count))
                    return;
                for (size_t i = 2; i < Influences; ++i) {
                    const float threshold = Scalar(uniforms, surface->AdditionalInfluenceThresholds[i - 2]);
                    if (!std::isfinite(threshold))
                        return;
                    Enabled[i] = threshold <= count;
                }
            }
        }
        Valid = true;
    }
    bool Position(Vec4 position, const Vec4& indices, const Vec4& weights, ShadowVector& result) {
        if (!Valid)
            return false;
        ShadowVector model{};
        if (Skinned) {
            for (auto& v : position)
                v *= PositionScale;
            for (uint8_t i = 0; i < Influences; ++i) {
                if (!Enabled[i])
                    continue;
                const float offset = indices[i] * IndexScale;
                if (!std::isfinite(offset) || offset < 0 || offset > 95)
                    return false;
                const uint32_t bone = static_cast<uint32_t>(offset);
                if (!Ready[bone]) {
                    ShadowMatrix matrix, linear = Post;
                    linear[12] = linear[13] = linear[14] = 0;
                    if (!UniformMatrix(Uniforms, Skeleton->PaletteFirstUniform + bone, matrix))
                        return false;
                    Bones[bone] = Multiply(linear, matrix);
                    Ready[bone] = true;
                }
                const auto point = Transform(Bones[bone], position);
                const float weight = Multiple ? weights[i] * WeightScale : 1;
                for (size_t axis = 0; axis < 3; ++axis)
                    model[axis] += point[axis] * weight;
            }
            result = { model[0] + Post[12], model[1] + Post[13], model[2] + Post[14] };
        } else {
            model = Transform(Model, position);
            result = Transform(Post, { model[0], model[1], model[2], 1 });
        }
        return std::all_of(result.begin(), result.end(), [](float v) { return std::isfinite(v); });
    }
    bool Valid = false, Skinned = false, Multiple = false;

  private:
    const PicaVertexSkeletonLayout* Skeleton = nullptr;
    std::span<const uint8_t> Uniforms;
    ShadowMatrix Post{}, Model{};
    std::array<ShadowMatrix, 96> Bones{};
    std::array<bool, 96> Ready{};
    std::array<bool, 4> Enabled{ true, false, false, false };
    uint8_t Influences = 1;
    float PositionScale = 1, IndexScale = 1, WeightScale = 1;
};
} // namespace Fast::Renderer3ds::VertexGeometry
