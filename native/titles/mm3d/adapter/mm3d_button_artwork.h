// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oot3d_native_control_config.h"

namespace Oot3dNativeGame {
// Keep the title's original HD artwork only when it still describes every
// binding. A remapped key, keyboard or another pad family needs dynamic labels.
inline bool Mm3dNativeArtworkMatches(const NativeControlConfig& controls, NativePromptSet prompts) {
    if (prompts != NativePromptSet::Nintendo || !controls.ControllerEnabled) return false;
    const auto native = NativeControlPreset(NativeControlProfile::Controller);
    for (const auto action : {NativeControlAction::A, NativeControlAction::B,
            NativeControlAction::X, NativeControlAction::Y, NativeControlAction::L,
            NativeControlAction::R, NativeControlAction::Zl, NativeControlAction::Zr,
            NativeControlAction::Start, NativeControlAction::Select,
            NativeControlAction::DpadUp, NativeControlAction::DpadDown,
            NativeControlAction::DpadLeft, NativeControlAction::DpadRight}) {
        const auto index = static_cast<size_t>(action);
        if (controls.Bindings[index].Gamepad != native.Bindings[index].Gamepad) return false;
    }
    return true;
}
}
