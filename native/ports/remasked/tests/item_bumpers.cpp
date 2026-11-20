// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../../titles/mm3d/adapter/mm3d_restoration.h"
#include "../../../titles/mm3d/restoration/engine_impl.h"
#include "../../../titles/mm3d/restoration/port_tests.inc"
#include "../runtime/controller_layout.h"
int main() {
    using namespace Oot3dNativeGame;
    try {
        NativeControlConfig config;
        config.Bindings[size_t(NativeControlAction::L)].Gamepad=NativeGamepadButton::LeftShoulder;
        config.Bindings[size_t(NativeControlAction::R)].Gamepad=NativeGamepadButton::RightShoulder;
        config.Bindings[size_t(NativeControlAction::Zl)].Gamepad=NativeGamepadButton::LeftTrigger;
        config.Bindings[size_t(NativeControlAction::Zr)].Gamepad=NativeGamepadButton::RightTrigger;
        auto custom=config;custom.Bindings[size_t(NativeControlAction::L)].Gamepad=NativeGamepadButton::A;
        RestorationPortTests::Check(!ReMasked::MigrateDefaultShoulders(custom),"custom shoulder map preserved");
        RestorationPortTests::Check(ReMasked::MigrateDefaultShoulders(config),"default shoulders migrate");
        RestorationPortTests::Check(config.Bindings[size_t(NativeControlAction::Zl)].Gamepad==NativeGamepadButton::LeftShoulder&&
            config.Bindings[size_t(NativeControlAction::Zr)].Gamepad==NativeGamepadButton::RightShoulder&&
            config.Bindings[size_t(NativeControlAction::L)].Gamepad==NativeGamepadButton::LeftTrigger&&
            config.Bindings[size_t(NativeControlAction::R)].Gamepad==NativeGamepadButton::RightTrigger,"physical layout matches ReMasked");
        RestorationPortTests::Check(!ReMasked::MigrateDefaultShoulders(config),"migration is idempotent");
        RestorationPortTests::DirectItemShoulders();
        RestorationPortTests::TatlDpadState();
        RestorationPortTests::PictographShortcut();
        std::cout<<"ReMasked item bumpers, assignment, usability, release, instrument notes and Tatl states passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
