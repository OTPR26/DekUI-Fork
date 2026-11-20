#include "fast/renderer3ds/pica_vertex_surface.h"
#include "vulkan_compute_test.h"
#include <array>
#include <cmath>
#include <iostream>

int main() {
    try {
        using namespace Fast::Renderer3ds;
        PicaVertexShaderHookLayout hooks;
        hooks.Transform = { PicaVertexTransformOperation::ModelViewProjection3x4, 0, 4, 4, 3, 20, 3, 0, 1 };
        hooks.Skeleton = { PicaVertexSkeletonOperation::MatrixPalette3x4, 2, 3, 20, 3, 6, 7, 4 };
        auto& surface = hooks.Surface;
        surface.ViewNormalTemporary = 14;
        surface.LightingBooleanUniform = 9;
        surface.DiffuseUniform = 8;
        surface.AmbientUniform = 9;
        surface.TintBooleanUniform = 5;
        surface.TintInputRegister = 2;
        surface.TintScale = { 90, 2 };
        surface.LightEnabledValue = { 93, 1 };
        surface.LightDirectionUniforms = { 80, 83, 86 };
        surface.BoneIndexScale = { 93, 3 };
        surface.BoneWeightScale = { 94, 3 };
        surface.SkinnedNormalScale = { 90, 1 };
        surface.InfluenceCount = { 92, 3 };
        surface.AdditionalInfluenceThresholds = { PicaVertexUniformComponent{ 95, 0 },
                                                  PicaVertexUniformComponent{ 95, 1 } };
        const auto generated = BuildPicaVertexSurfaceShader(hooks);
        if (!generated.Available)
            throw std::runtime_error("Surface normal program unavailable");
        // Execute the production vertex arithmetic densely on the GPU. Only the
        // stage output qualifiers become invocation-private variables in this
        // compute consumer; the generated arithmetic is used verbatim.
        std::string shader = R"glsl(#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) readonly buffer Cases { vec4 cases[]; } inputs;
layout(set=0,binding=1,std430) writeonly buffer Results { vec4 values[]; } outputs;
struct Uniforms { vec4 f[96]; uint b; } uniforms;
vec4 pica_input0, pica_input1, pica_input2, pica_input6, pica_input7, reg_tmp14;
vec4 get_offset_register(int first, int offset) { return uniforms.f[first + offset]; }
#define layout(x)
#define out
)glsl";
        shader += generated.Declarations;
        shader += R"glsl(
#undef out
#undef layout
void main() {
    uint index = gl_GlobalInvocationID.x;
    vec4 sample_case = inputs.cases[index];
    for (int i=0; i<96; ++i) uniforms.f[i]=vec4(0.0);
    uniforms.b=12u;
    uniforms.f[4]=vec4(1,0,0,0);
    uniforms.f[5]=vec4(0,1,0,0);
    uniforms.f[6]=vec4(0,0,1,0);
    uniforms.f[20]=vec4(1,0,0,0);
    uniforms.f[21]=vec4(0,1,0,0);
    uniforms.f[22]=vec4(0,0,1,0);
    // Two valid rigid bones, with a 180 degree relative rotation about Y.
    uniforms.f[23]=vec4(-1,0,0,0);
    uniforms.f[24]=vec4(0,1,0,0);
    uniforms.f[25]=vec4(0,0,-1,0);
    uniforms.f[90]=vec4(1.0);
    uniforms.f[92].w=2.0;
    uniforms.f[93]=vec4(0,1,0,3);
    uniforms.f[94].w=1.0;
    uniforms.f[95]=vec4(3,4,0,0);
    pica_input0=vec4(0,0,0,1);
    pica_input1=vec4(1,1,0,0);
    pica_input2=vec4(1.0);
    pica_input6=vec4(0,1,0,0);
    pica_input7=vec4(1.0-sample_case.x,sample_case.x,0,0);
    reg_tmp14=vec4(normalize(vec3(1.0-2.0*sample_case.x,1,0)),0);
    if (sample_case.y != 0.0) {
        uniforms.b=0u;
        uniforms.f[20].x=sample_case.y*2.0;
        uniforms.f[22].z=0.5;
        pica_input1=vec4(1,1,1,0);
    }
)glsl";
        shader += generated.MainBody;
        shader += "outputs.values[index]=vec4(pica_surface_normal,1);\n}\n";
        using Vec4 = std::array<float, 4>;
        const std::array<Vec4, 9> cases{ { { 0, 0, 0, 0 },
                                           { 0.25F, 0, 0, 0 },
                                           { 0.49F, 0, 0, 0 },
                                           { 0.5F, 0, 0, 0 },
                                           { 0.51F, 0, 0, 0 },
                                           { 0.75F, 0, 0, 0 },
                                           { 1, 0, 0, 0 },
                                           { 0, 1, 0, 0 },
                                           { 0, -1, 0, 0 } } };
        std::array<Vec4, cases.size()> output{};
        RunComputeTest(shader, std::as_bytes(std::span(cases)), std::as_writable_bytes(std::span(output)),
                       cases.size());
        unsigned failures = 0;
        for (size_t i = 0; i < cases.size(); ++i) {
            const auto& sample = cases[i];
            Vec4 expected = sample[1] == 0 ? Vec4{ 1 - 2 * sample[0], 1, 0, 0 } : Vec4{ 0.5F / sample[1], 1, 2, 0 };
            const float length =
                std::sqrt(expected[0] * expected[0] + expected[1] * expected[1] + expected[2] * expected[2]);
            for (size_t channel = 0; channel < 3; ++channel) {
                expected[channel] /= length;
                if (!std::isfinite(output[i][channel]) || std::abs(output[i][channel] - expected[channel]) > 2.0e-5F) {
                    std::cerr << "case=" << i << " channel=" << channel << " actual=" << output[i][channel]
                              << " expected=" << expected[channel] << '\n';
                    ++failures;
                }
            }
        }
        std::cout << "Surface normals: " << cases.size() << " GPU cases, " << failures << " failures\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
