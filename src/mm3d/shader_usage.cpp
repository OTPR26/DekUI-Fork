// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <nihstro/shader_bytecode.h>
#include "mm3d/shader_usage.h"
namespace MM3D::Trace {
Usage AnalyzeUsage(std::span<const std::uint32_t> code) {
    Usage result;
    result.known = true;
    for (auto word : code.first(std::min(code.size(), std::size_t(4096)))) {
        const nihstro::Instruction instruction{word};
        const auto op = instruction.opcode.Value();
        const auto info = op.GetInfo();
        const bool dot = op.EffectiveOpCode() == nihstro::OpCode::Id::DP3 ||
            op.EffectiveOpCode() == nihstro::OpCode::Id::DP4 ||
            op.EffectiveOpCode() == nihstro::OpCode::Id::DPH ||
            op.EffectiveOpCode() == nihstro::OpCode::Id::DPHI;
        const auto mark = [&](nihstro::SourceRegister source, bool relative) {
            if (source.GetRegisterType() != nihstro::RegisterType::FloatUniform) return;
            const auto index = source.GetIndex();
            if (index < 0 || index >= 96) return;
            if (relative) result.relative_base[index] = true;
            else { result.direct[index] = true; result.dot[index] |= dot; }
        };
        if (info.type == nihstro::OpCode::Type::Arithmetic) {
            const bool inverted = (info.subtype & nihstro::OpCode::Info::SrcInversed) != 0;
            const bool relative = instruction.common.address_register_index != 0;
            if (info.subtype & nihstro::OpCode::Info::Src1)
                mark(instruction.common.GetSrc1(inverted), !inverted && relative);
            if (info.subtype & nihstro::OpCode::Info::Src2)
                mark(instruction.common.GetSrc2(inverted), inverted && relative);
        } else if (info.type == nihstro::OpCode::Type::MultiplyAdd) {
            const bool inverted = op.EffectiveOpCode() == nihstro::OpCode::Id::MADI;
            const bool relative = instruction.mad.address_register_index != 0;
            mark(instruction.mad.GetSrc1(inverted), false);
            mark(instruction.mad.GetSrc2(inverted), !inverted && relative);
            mark(instruction.mad.GetSrc3(inverted), inverted && relative);
        }
    }
    return result;
}
}
