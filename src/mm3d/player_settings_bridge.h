// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include "mm3d/player_settings.h"

namespace MM3D::PlayerSettings {
// Call on the frontend's settings-owning thread. The renderer only queues
// requests. Platform callbacks apply emulator settings and persist them;
// this bridge does not depend on Qt, JNI or a particular configuration format.
template<class Values, class ApplyRuntime, class Persist>
void ApplyPending(Values& values, ApplyRuntime apply_runtime, Persist persist) {
    bool runtime_changed = false;
    const auto apply = [&](auto& request, auto& setting, auto convert) {
        const int value = request.exchange(-1);
        if (value < 0) return;
        setting.SetGlobal(true);
        setting.SetValue(convert(value));
        runtime_changed = true;
    };
    apply(set_resolution, values.resolution_factor,
          [](int v) { return std::uint32_t(std::clamp(v, 1, 9)); });
    apply(set_volume, values.volume,
          [](int v) { return float(std::clamp(v, 0, 100)) / 100.f; });
    apply(set_vsync, values.use_vsync, [](int v) { return v != 0; });
    apply(set_cache, values.use_disk_shader_cache, [](int v) { return v != 0; });
    apply(set_textures, values.custom_textures, [](int v) { return v != 0; });
    if (runtime_changed) apply_runtime();

    // Hardware shaders are read for each PICA batch. Async compilation is
    // captured by the rasterizer at startup. Preserve desktop behavior: save
    // either preference without forcing a renderer restart or ApplySettings.
    bool changed = runtime_changed;
    const auto shader = [&](auto& request, auto& setting) {
        const int value = request.exchange(-1);
        if (value < 0) return;
        setting.SetGlobal(true);
        setting.SetValue(value != 0);
        changed = true;
    };
    shader(set_hardware_shaders, values.use_hw_shader);
    shader(set_async_shaders, values.async_shader_compilation);
    if (changed) persist();

    resolution.store(values.resolution_factor.GetValue());
    volume.store(int(values.volume.GetValue() * 100.f));
    vsync.store(values.use_vsync.GetValue());
    cache.store(values.use_disk_shader_cache.GetValue());
    textures.store(values.custom_textures.GetValue());
    hardware_shaders.store(values.use_hw_shader.GetValue());
    async_shaders.store(values.async_shader_compilation.GetValue());
}
} // namespace MM3D::PlayerSettings
