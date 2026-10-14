// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <nihstro/inline_assembly.h>
#include "video_core/pica/cpu_vertex_replay.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// Previous replay implementation kept as a differential correctness reference.
// It intentionally copies the shader and allocates outputs on every replay.
void ReferenceReplay(Pica::ShaderEngine& engine, const Pica::ShaderSetup& native_setup,
                     const Pica::RegsInternal& regs, const Pica::Uniforms& uniforms,
                     const std::vector<Pica::AttributeBuffer>& inputs,
                     const std::vector<u32>& order, Pica::PipelineRegs::TriangleTopology topology,
                     const Pica::PrimitiveAssembler::TriangleHandler& handler) {
    for (const auto index : order) Check(index < inputs.size(), "Reference index valid");
    auto setup = native_setup;
    setup.uniforms = uniforms;
    engine.SetupBatch(setup, regs.vs.main_offset);
    Pica::ShaderUnit unit;
    std::vector<Pica::OutputVertex> outputs;
    outputs.reserve(inputs.size());
    for (const auto& input : inputs) {
        Pica::AttributeBuffer output{};
        unit.LoadInput(regs.vs, input);
        engine.Run(setup, unit);
        unit.WriteOutput(regs.vs, output);
        outputs.emplace_back(regs.rasterizer, output);
    }
    Pica::PrimitiveAssembler assembler{topology};
    for (const auto index : order) assembler.SubmitVertex(outputs[index], handler);
}

void Run(bool jit) {
    using namespace Pica;
    using Topology = PipelineRegs::TriangleTopology;
    using Op = nihstro::OpCode::Id;
    const auto binary = nihstro::InlineAsm::CompileToRawBinary({
        {Op::MUL, nihstro::DestRegister::MakeTemporary(0),
                  nihstro::SourceRegister::MakeFloat(20), nihstro::SourceRegister::MakeInput(3)},
        {Op::ADD, nihstro::DestRegister::MakeOutput(0),
                  nihstro::SourceRegister::MakeFloat(0), nihstro::SourceRegister::MakeTemporary(0)},
        {Op::END},
    });
    ShaderSetup setup;
    for (unsigned i = 0; i < binary.program.size(); ++i) setup.UpdateProgramCode(i, binary.program[i].hex);
    for (unsigned i = 0; i < binary.swizzle_table.size(); ++i) setup.UpdateSwizzleData(i, binary.swizzle_table[i].hex);
    setup.uniforms = {};
    setup.uniforms.f[20] = Common::Vec4<f24>::AssignToAll(f24::One());
    RegsInternal regs{};
    regs.vs.max_input_attribute_index.Assign(0);
    // Exercise non-identity input routing: attribute zero is loaded to v3.
    regs.vs.input_attribute_to_register_map_low = 3;
    regs.vs.output_mask.Assign(1);
    regs.rasterizer.vs_output_total.Assign(1);
    using Semantic = RasterizerRegs::VSOutputAttributes;
    regs.rasterizer.vs_output_attributes[0].map_x.Assign(Semantic::POSITION_X);
    regs.rasterizer.vs_output_attributes[0].map_y.Assign(Semantic::POSITION_Y);
    regs.rasterizer.vs_output_attributes[0].map_z.Assign(Semantic::POSITION_Z);
    regs.rasterizer.vs_output_attributes[0].map_w.Assign(Semantic::POSITION_W);
    auto engine = CreateEngine(jit);
    engine->SetupBatch(setup, 0);
    const auto original_cached = setup.cached_shader;
    const auto original_hash = setup.GetProgramCodeHash();
    auto midpoint = setup.uniforms;
    midpoint.f[20] = Common::Vec4<f24>::AssignToAll(f24::FromFloat32(2));
    midpoint.f[0].x = f24::FromFloat32(10);

    for (const auto topology : {Topology::List, Topology::Strip, Topology::Fan}) {
        CpuVertexReplay batch;
        const std::vector<u32> order = topology == Topology::List
            ? std::vector<u32>{0,1,2,2,1,3} : std::vector<u32>{0,1,2,3};
        batch.Begin(static_cast<u32>(order.size()), topology);
        for (unsigned i = 0; i < 4; ++i) {
            AttributeBuffer input{};
            input[0] = {f24::FromFloat32(float(i)), f24::Zero(), f24::Zero(), f24::One()};
            Check(batch.AddInput(input) == i, "Input identity retained");
            input[0].x = f24::FromFloat32(999); // Must not change the saved draw.
        }
        for (unsigned i = 0; i + 1 < order.size(); ++i) batch.AddVertex(order[i]);
        Check(!batch.Ready(), "Incomplete captures cannot replay");
        batch.AddVertex(order.back());
        Check(batch.Ready(), "Complete capture available");
        std::vector<float> native, modified;
        const auto append = [](std::vector<float>& out) {
            return [&out](const OutputVertex& a, const OutputVertex& b, const OutputVertex& c) {
                out.insert(out.end(), {a.pos.x.ToFloat32(), b.pos.x.ToFloat32(), c.pos.x.ToFloat32()});
            };
        };
        Check(batch.Replay(*engine, setup, regs, setup.uniforms, append(native)), "Native replay runs");
        Check(batch.Replay(*engine, setup, regs, midpoint, append(modified)), "Midpoint replay runs");
        const std::vector<float> expected = topology == Topology::List
            ? std::vector<float>{0,1,2,2,1,3} : topology == Topology::Strip
            ? std::vector<float>{0,1,2,2,1,3} : std::vector<float>{0,1,2,0,2,3};
        Check(native == expected && modified.size() == expected.size(), "Topology and indexed order preserved");
        for (unsigned i = 0; i < expected.size(); ++i)
            Check(std::abs(modified[i] - (2 * expected[i] + 10)) < .0001f,
                  "Camera and bone uniform overrides affect replay vertices");
        std::vector<float> after;
        auto replacements = batch.Inputs();
        for (auto& input : replacements) input[0].x = input[0].x + f24::FromFloat32(3);
        std::vector<float> particles;
        Check(batch.Replay(*engine, setup, regs, midpoint, append(particles), replacements),
              "Particle inputs replay with midpoint uniforms");
        for (unsigned i = 0; i < expected.size(); ++i)
            Check(std::abs(particles[i] - (2 * (expected[i] + 3) + 10)) < .0001f,
                  "Replacement vertices preserve indexed topology and attribute routing");
        replacements.pop_back();
        Check(!batch.Replay(*engine, setup, regs, midpoint, append(particles), replacements),
              "Partial replacement input rejected");
        batch.Replay(*engine, setup, regs, setup.uniforms, append(after));
        Check(after == native, "Replay does not mutate native inputs or uniforms");
        Check(setup.uniforms.f[20].x.ToFloat32() == 1 && setup.uniforms.f[0].x.ToFloat32() == 0 &&
              setup.cached_shader == original_cached && setup.GetProgramCodeHash() == original_hash,
              "Guest shader state stays unchanged");
        batch.Begin(0, topology);
        Check(!batch.Ready(), "No stale replay after reset");
    }
    CpuVertexReplay invalid;
    invalid.Begin(65537, Topology::List);
    Check(!invalid.Ready(), "Oversized capture rejected");
    invalid.Begin(3, Topology::List);
    invalid.AddInput(AttributeBuffer{});
    for (unsigned i = 0; i < 3; ++i) invalid.AddVertex(99);
    Check(!invalid.Replay(*engine, setup, regs, midpoint, [](const auto&, const auto&, const auto&) {
        throw std::runtime_error("Invalid capture emitted geometry");
    }), "Invalid input index rejected before drawing");

    CpuVertexReplay reusable;
    const auto check_reference = [&](ShaderEngine& replay_engine, unsigned count) {
        reusable.Begin(count, Topology::List);
        std::vector<u32> order;
        for (unsigned i = 0; i < count; ++i) {
            AttributeBuffer input{};
            input[0] = {f24::FromFloat32(float(i)), f24::FromFloat32(2),
                        f24::FromFloat32(3), f24::One()};
            order.push_back(reusable.AddInput(input));
            reusable.AddVertex(order.back());
        }
        std::vector<float> expected, actual;
        const auto append = [](std::vector<float>& out) {
            return [&out](const OutputVertex& a, const OutputVertex& b, const OutputVertex& c) {
                for (const auto* vertex : {&a, &b, &c})
                    for (unsigned i = 0; i < 4; ++i) out.push_back(vertex->pos[i].ToFloat32());
            };
        };
        ReferenceReplay(replay_engine, setup, regs, midpoint, reusable.Inputs(), order,
                        Topology::List, append(expected));
        Check(reusable.Replay(replay_engine, setup, regs, midpoint, append(actual)),
              "Reused replay completes");
        Check(actual == expected, "Reused shader/output storage matches original replay");
    };
    for (unsigned count : {3u, 96u, 6u, 3u}) check_reference(*engine, count);
    const auto original_word = setup.GetProgramCode()[0];
    setup.UpdateProgramCode(0, binary.program[1].hex);
    check_reference(*engine, 6); // Dirty program must invalidate the snapshot.
    setup.UpdateProgramCode(0, original_word);
    engine->SetupBatch(setup, 0);
    setup.GetProgramCodeHash();
    setup.GetSwizzleDataHash();
    check_reference(*engine, 6); // Clean but different hash must also invalidate it.
    const auto original_swizzle = setup.GetSwizzleData()[0];
    setup.UpdateSwizzleData(0, original_swizzle ^ (1u << 4));
    check_reference(*engine, 6);
    setup.UpdateSwizzleData(0, original_swizzle);
    engine->SetupBatch(setup, 0);
    setup.GetProgramCodeHash();
    setup.GetSwizzleDataHash();
    check_reference(*engine, 6);
    regs.vs.main_offset.Assign(1);
    check_reference(*engine, 6);
    regs.vs.main_offset.Assign(0);
    auto other_engine = CreateEngine(!jit);
    check_reference(*other_engine, 6);
    check_reference(*engine, 6);

    std::cout << "PASS CPU vertex replay " << (jit ? "JIT" : "interpreter")
              << ": indexed reuse, list/strip/fan, snapshot lifetime, uniform isolation, reset, bounds, "
                 "shader/swizzle/entry/engine changes and scratch reuse\n";
}
}
int main() {
    try { Run(false); Run(true); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
