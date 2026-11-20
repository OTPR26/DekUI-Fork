// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oot3d_native_control_config.h"
namespace ReMasked {
// ReMasked's automatic physical layout: triggers target/shield; bumpers
// activate the two extra item slots. Preserve any customized shoulder map.
inline bool MigrateDefaultShoulders(Oot3dNativeGame::NativeControlConfig& config) {
    using namespace Oot3dNativeGame;
    constexpr NativeControlAction actions[]{NativeControlAction::L,NativeControlAction::R,
        NativeControlAction::Zl,NativeControlAction::Zr};
    constexpr NativeGamepadButton original[]{NativeGamepadButton::LeftShoulder,NativeGamepadButton::RightShoulder,
        NativeGamepadButton::LeftTrigger,NativeGamepadButton::RightTrigger};
    constexpr NativeGamepadButton remasked[]{NativeGamepadButton::LeftTrigger,NativeGamepadButton::RightTrigger,
        NativeGamepadButton::LeftShoulder,NativeGamepadButton::RightShoulder};
    for(size_t i=0;i<4;++i)if(config.Bindings[size_t(actions[i])].Gamepad!=original[i])return false;
    for(size_t i=0;i<4;++i)config.Bindings[size_t(actions[i])].Gamepad=remasked[i];
    return true;
}
}
