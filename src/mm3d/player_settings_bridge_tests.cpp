// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/player_settings_bridge.h"
#include <cstdio>
#include <cstdlib>

namespace {
template<class T> struct Setting {
    T value{};
    bool global = false;
    void SetGlobal(bool enabled) { global = enabled; }
    void SetValue(T v) { value = v; }
    T GetValue() const { return value; }
};
struct Values {
    Setting<std::uint32_t> resolution_factor{3};
    Setting<float> volume{1.f};
    Setting<bool> use_vsync{true}, use_disk_shader_cache{true}, custom_textures{false};
    Setting<bool> use_hw_shader{false}, async_shader_compilation{false};
};
void Check(bool pass, const char* message) {
    if (!pass) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}
int main() {
    using namespace MM3D::PlayerSettings;
    Values values;
    int applied = 0, saved = 0;
    const auto tick = [&] { ApplyPending(values, [&] { ++applied; }, [&] { ++saved; }); };
    tick();
    Check(applied == 0 && saved == 0, "idle polling must not apply or save");
    Check(resolution == 3 && volume == 100 && vsync == 1, "publish initial settings");

    set_resolution = 99; set_volume = 200; set_vsync = 0;
    set_cache = 0; set_textures = 1;
    tick();
    Check(applied == 1 && saved == 1, "batch related changes into one apply/save");
    Check(resolution == 9 && volume == 100 && vsync == 0 && cache == 0 && textures == 1,
          "clamp sliders and apply disabled booleans");
    Check(values.resolution_factor.global && values.volume.global && values.custom_textures.global,
          "explicit requests select global settings");
    tick();
    Check(applied == 1 && saved == 1, "requests consumed exactly once");

    set_resolution = 0; set_volume = 0;
    tick();
    Check(resolution == 1 && volume == 0, "lower clamp and mute volume");
    set_hardware_shaders = 1; set_async_shaders = 1;
    tick();
    Check(applied == 2 && saved == 3, "shader preferences save without renderer reapply");
    Check(hardware_shaders == 1 && async_shaders == 1, "publish shader settings");
    set_hardware_shaders = 0; set_async_shaders = 0;
    tick();
    Check(hardware_shaders == 0 && async_shaders == 0 && applied == 2 && saved == 4,
          "shader preferences can be disabled");

    open_textures = true; controls = true; quit = true; set_game_language = 5;
    tick();
    Check(open_textures && controls && quit && set_game_language == 5,
          "leave platform actions and emulation-thread language requests for their owners");
    Check(saved == 4 && applied == 2, "platform-only actions must not write graphics settings");

    // An update queued during persistence must survive to the following poll.
    set_volume = 75;
    ApplyPending(values, [&] { ++applied; }, [&] { ++saved; set_volume = 25; });
    Check(volume == 75 && set_volume == 25, "do not lose a newly queued request");
    tick();
    Check(volume == 25 && saved == 6 && applied == 4, "apply subsequent request next poll");
    std::puts("Shared player settings bridge checks passed");
}
