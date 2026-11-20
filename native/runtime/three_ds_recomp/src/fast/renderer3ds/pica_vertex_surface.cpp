#include "fast/renderer3ds/pica_vertex_surface.h"

#include <sstream>

namespace Fast::Renderer3ds {
namespace {

std::string Scalar(PicaVertexUniformComponent value) {
    return "uniforms.f[" + std::to_string(value.Uniform) + "]." + "xyzw"[value.Component];
}

std::string Enabled(uint8_t boolean) {
    if (boolean >= 16U) return "false";
    return "((uniforms.b & " + std::to_string(1U << boolean) + "u) != 0u)";
}

std::string Matrix(uint8_t first, bool palette) {
    std::string result = "transpose(mat3(";
    for (uint8_t row = 0; row < 3U; ++row) {
        if (row != 0U) result += ", ";
        result += palette ? "get_offset_register(" : "uniforms.f[";
        result += std::to_string(first + row);
        result += palette ? ", pica_bone_offset).xyz" : "].xyz";
    }
    return result + "))";
}

} // namespace

PicaVertexSurfaceShader BuildPicaVertexSurfaceShader(const PicaVertexShaderHookLayout& hooks) {
    PicaVertexSurfaceShader result;
    const auto* surface = hooks.SurfaceProgram();
    const auto* transform = hooks.TransformProgram();
    const auto* skeleton = hooks.SkeletonProgram();
    if (surface == nullptr || transform == nullptr ||
        (skeleton != nullptr && !surface->SkinnedNormalsAvailable())) return result;
    result.Declarations = R"glsl(
layout(location=9) out vec3 pica_surface_normal;
layout(location=10) out vec4 pica_surface_tint;
vec3 pica_surface_transform_normal(mat3 linear, vec3 normal) {
    mat3 cofactor = mat3(cross(linear[1], linear[2]),
                         cross(linear[2], linear[0]),
                         cross(linear[0], linear[1]));
    float determinant = dot(linear[0], cofactor[0]);
    return abs(determinant) > 1e-20
        ? (cofactor * normal) / determinant : linear * normal;
}
)glsl";
    std::ostringstream source;
    source << "    {\n"
           << "        vec3 pica_source_normal = pica_input" << unsigned(transform->NormalInputRegister) << ".xyz;\n"
           << "        vec3 pica_corrected_normal = pica_surface_transform_normal("
           << Matrix(transform->ModelFirstUniform, false) << ", pica_source_normal);\n";
    if (skeleton != nullptr) {
        source << "        if (" << Enabled(skeleton->EnableBooleanUniform) << ") {\n"
               << "            pica_source_normal *= " << Scalar(surface->SkinnedNormalScale) << ";\n"
               << "            pica_corrected_normal = vec3(0.0);\n"
               << "            bool pica_multiple_bones = " << Enabled(skeleton->MultipleInfluenceBooleanUniform) << ";\n";
        for (uint8_t influence = 0; influence < skeleton->MaximumInfluences; ++influence) {
            source << "            if (";
            if (influence == 0U) source << "true";
            else source << "pica_multiple_bones";
            if (influence >= 2U)
                source << " && " << Scalar(surface->AdditionalInfluenceThresholds[influence - 2U])
                       << " <= " << Scalar(surface->InfluenceCount);
            source << ") {\n"
                   << "                int pica_bone_offset = int(" << Scalar(surface->BoneIndexScale)
                   << " * pica_input" << unsigned(skeleton->BoneIndexInputRegister) << "." << "xyzw"[influence] << ");\n"
                   << "                float pica_bone_weight = pica_multiple_bones ? "
                   << Scalar(surface->BoneWeightScale) << " * pica_input" << unsigned(skeleton->BoneWeightInputRegister)
                   << "." << "xyzw"[influence] << " : 1.0;\n"
                   << "                pica_corrected_normal += pica_surface_transform_normal("
                   << Matrix(skeleton->PaletteFirstUniform, true)
                   << ", pica_source_normal) * pica_bone_weight;\n"
                   << "            }\n";
        }
        source << "        }\n";
    }
    // Blend transformed normals, not inverse blended matrices: two valid
    // bones can have a singular matrix average at a joint. Keep the authored
    // normals/weights (including hard edges) and normalize only after blending.
    source << "        pica_corrected_normal = pica_surface_transform_normal("
           << Matrix(transform->ViewFirstUniform, false) << ", pica_corrected_normal);\n"
           << "        float pica_normal_length2 = dot(pica_corrected_normal, pica_corrected_normal);\n"
           << "        pica_surface_normal = pica_normal_length2 > 1e-20\n"
           << "            ? pica_corrected_normal * inversesqrt(pica_normal_length2) : vec3(0.0);\n"
           << "        pica_surface_tint = " << Enabled(surface->TintBooleanUniform) << " ? pica_input"
           << unsigned(surface->TintInputRegister) << " * " << Scalar(surface->TintScale) << " : vec4(1.0);\n"
           << "    }\n";
    result.MainBody = source.str();
    result.Available = true;
    return result;
}

} // namespace Fast::Renderer3ds
