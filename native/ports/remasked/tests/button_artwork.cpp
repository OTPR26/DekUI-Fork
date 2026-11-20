// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../../titles/mm3d/adapter/mm3d_button_artwork.h"
#include <cassert>
#include <iostream>
using namespace Oot3dNativeGame;
int main() {
    auto config=NativeControlDefaults();
    assert(Mm3dNativeArtworkMatches(config,NativePromptSet::Nintendo));
    for(auto family:{NativePromptSet::Keyboard,NativePromptSet::Xbox,NativePromptSet::PlayStation,NativePromptSet::Touch})
        assert(!Mm3dNativeArtworkMatches(config,family));
    for(size_t i=size_t(NativeControlAction::A);i<=size_t(NativeControlAction::DpadRight);++i) {
        auto remapped=config;remapped.Bindings[i].Gamepad=NativeGamepadButton::None;
        assert(!Mm3dNativeArtworkMatches(remapped,NativePromptSet::Nintendo));
    }
    config.ControllerEnabled=false;
    assert(!Mm3dNativeArtworkMatches(config,NativePromptSet::Nintendo));
    std::cout << "Original button artwork: matching Nintendo controls preserved; every remapped button and other family keeps dynamic labels\n";
}
