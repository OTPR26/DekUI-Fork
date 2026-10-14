// Copyright 2026 MM3D presentation contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <dynarmic/interface/A32/a32.h>
#include "mm3d/camera_writer.h"
namespace MM3D::CameraWriter {
// Step returns at an instruction boundary, so backing registers are synchronized.
// Do not emit pre-translation IR: that makes a conditional block nonempty before
// Dynarmic's condition handling and can generate a zero-progress self-loop.
inline Dynarmic::HaltReason StepWithSnapshot(Dynarmic::A32::Jit& jit, Instruction& out) {
    out.pc = jit.Regs()[15];
    out.lr = jit.Regs()[14];
    out.sp = jit.Regs()[13];
    out.thumb = (jit.Cpsr() & 0x20) != 0;
    return jit.Step();
}
}
