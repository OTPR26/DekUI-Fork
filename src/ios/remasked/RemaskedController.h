// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <libretro.h>

namespace ReMaskedController {

enum Button : unsigned {
    Unassigned, East, South, North, West, LeftBumper, RightBumper, LeftTrigger, RightTrigger,
    MenuButton, OptionsButton, DpadUp, DpadDown, DpadLeft, DpadRight, LeftStickClick, RightStickClick, GuideButton, Count
};

// Stored IDs describe physical positions. Display the connected controller's printed face-button labels.
inline constexpr std::array<const char*, Count> Names{
    "Unbound", "A", "B", "X", "Y", "Select", "Home", "Start", "L3", "R3",
    "L / L1", "R / R1", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "ZL / L2", "ZR / R2"
};
inline constexpr std::array<Button, Count> ChoiceButtons(bool nintendo) {
    return {Unassigned, nintendo ? East : South, nintendo ? South : East,
            nintendo ? North : West, nintendo ? West : North, OptionsButton, GuideButton,
            MenuButton, LeftStickClick, RightStickClick, LeftBumper, RightBumper,
            DpadUp, DpadDown, DpadLeft, DpadRight, LeftTrigger, RightTrigger};
}

using PhysicalButtons = std::uint32_t;
inline constexpr PhysicalButtons PhysicalBit(Button button) { return 1u << button; }

struct Binding {
    const char* key;
    const char* label;
    unsigned gameButton;
    Button defaultButton;
};

inline constexpr std::array Bindings{
    Binding{"Action", "A", RETRO_DEVICE_ID_JOYPAD_A, East},
    Binding{"Sword", "B", RETRO_DEVICE_ID_JOYPAD_B, South},
    Binding{"ItemX", "X", RETRO_DEVICE_ID_JOYPAD_X, North},
    Binding{"ItemY", "Y", RETRO_DEVICE_ID_JOYPAD_Y, West},
    Binding{"Settings", "Select", RETRO_DEVICE_ID_JOYPAD_SELECT, OptionsButton},
    Binding{"Start", "Start", RETRO_DEVICE_ID_JOYPAD_START, MenuButton},
    Binding{"Target", "ZL / L2: targeting", RETRO_DEVICE_ID_JOYPAD_L, LeftTrigger},
    Binding{"Shield", "ZR / R2: shield", RETRO_DEVICE_ID_JOYPAD_R, RightTrigger},
    Binding{"ItemI", "L / L1: item I", RETRO_DEVICE_ID_JOYPAD_L2, LeftBumper},
    Binding{"ItemII", "R / R1: item II", RETRO_DEVICE_ID_JOYPAD_R2, RightBumper},
    Binding{"Up", "D-pad up", RETRO_DEVICE_ID_JOYPAD_UP, DpadUp},
    Binding{"Down", "D-pad down", RETRO_DEVICE_ID_JOYPAD_DOWN, DpadDown},
    Binding{"Left", "D-pad left", RETRO_DEVICE_ID_JOYPAD_LEFT, DpadLeft},
    Binding{"Right", "D-pad right", RETRO_DEVICE_ID_JOYPAD_RIGHT, DpadRight},
    Binding{"Pictograph", "Pictograph Box", RETRO_DEVICE_ID_JOYPAD_R3, RightStickClick}
};

using Assignments = std::array<Button, Bindings.size()>;

inline constexpr std::uint16_t Bit(unsigned button) { return std::uint16_t(1u << button); }

inline std::uint16_t Map(PhysicalButtons pressed, const Assignments& assignments) {
    std::uint16_t game = 0;
    for (unsigned index = 0; index < Bindings.size(); ++index) {
        const auto button = assignments[index];
        if (button != Unassigned && (pressed & PhysicalBit(button)))
            game |= Bit(Bindings[index].gameButton);
    }
    return game;
}

inline std::array<float, 2> Stick(float x, float y, int deadZonePercent) {
    if (deadZonePercent == 0) return {x, y};
    const float deadZone = deadZonePercent / 100.f;
    const float length = std::hypot(x, y);
    if (length <= deadZone) return {};
    const float gain = (std::min(length, 1.f) - deadZone) / ((1.f - deadZone) * length);
    return {x * gain, y * gain};
}

inline bool Trigger(bool pressed, float value, int deadZonePercent) {
    return deadZonePercent == 0 ? pressed : value > deadZonePercent / 100.f;
}
} // namespace ReMaskedController
