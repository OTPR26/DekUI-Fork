// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <vector>
#include <span>
#include "video_core/pica/primitive_assembly.h"
#include "video_core/pica/regs_internal.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/pica/shader_unit.h"
#include "video_core/shader/shader.h"

namespace Pica {
// Capture the inputs actually loaded by the native CPU draw, including cache
// hits and resolved default attributes. Replay never rereads guest memory or
// alters the guest shader setup / primitive assembler.
class CpuVertexReplay {
public:
    void Begin(u32 count, PipelineRegs::TriangleTopology topology_) {
        inputs.clear();
        order.clear();
        expected = count <= 65536 ? count : 0;
        topology = topology_;
    }
    u32 AddInput(const AttributeBuffer& input) {
        if (!expected || inputs.size() >= expected) return 0;
        inputs.push_back(input);
        return static_cast<u32>(inputs.size() - 1);
    }
    void AddVertex(u32 input) {
        if (expected && order.size() < expected) order.push_back(input);
    }
    bool Ready() const { return expected && order.size() == expected && !inputs.empty(); }
    const std::vector<AttributeBuffer>& Inputs() const { return inputs; }
    const std::vector<u32>& Order() const { return order; }

    bool Replay(ShaderEngine& engine, const ShaderSetup& native_setup,
                const RegsInternal& regs, const Uniforms& uniforms,
                const PrimitiveAssembler::TriangleHandler& triangle_handler,
                std::span<const AttributeBuffer> replacement_inputs = {}) {
        if (!Ready()) return false;
        if (!replacement_inputs.empty() && replacement_inputs.size() != inputs.size()) return false;
        for (const auto index : order) if (index >= inputs.size()) return false;
        // Keep an owned snapshot: never temporarily overwrite the native uniforms.
        // SetupBatch still runs for every draw to handle entry-point/engine changes.
        if (!setup.HasSameProgram(native_setup)) setup = native_setup;
        setup.uniforms = uniforms;
        engine.SetupBatch(setup, regs.vs.main_offset);
        ShaderUnit unit;
        outputs.clear();
        outputs.reserve(inputs.size());
        const std::span<const AttributeBuffer> replay_inputs = replacement_inputs.empty()
            ? std::span<const AttributeBuffer>{inputs} : replacement_inputs;
        for (const auto& input : replay_inputs) {
            AttributeBuffer output{};
            unit.LoadInput(regs.vs, input);
            engine.Run(setup, unit);
            unit.WriteOutput(regs.vs, output);
            outputs.emplace_back(regs.rasterizer, output);
        }
        PrimitiveAssembler assembler{topology};
        for (const auto index : order) assembler.SubmitVertex(outputs[index], triangle_handler);
        return true;
    }
private:
    u32 expected{};
    PipelineRegs::TriangleTopology topology{};
    std::vector<AttributeBuffer> inputs;
    std::vector<u32> order;
    ShaderSetup setup;
    std::vector<OutputVertex> outputs;
};
}
