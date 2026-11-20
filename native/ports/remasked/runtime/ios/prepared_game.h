// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>

namespace ReMaskedIOS {
constexpr auto PreparedGameDirectory = "GameContent/PrivateGame";
constexpr auto PreparedGameManifest = "process-manifest.json";

inline nlohmann::json PreparedGameLaunchProfile() {
    const std::string root = "${profile_dir}";
    return {
        {"format", "oot3d_native_game_launch_profile_v1"},
        {"arguments", {
            "--a32-process-manifest", root + "/" + PreparedGameDirectory + "/" + PreparedGameManifest,
            "--resource-root", "${runtime_dir}/GameContent/resources",
            "--renderer", "vulkan", "--config", root + "/native-game.json",
            "--initialize-config", "product",
            "--save-data", root + "/saves", "--log-directory", root + "/logs",
            "--crash-directory", root + "/logs/crashes",
            "--savestate-directory", root + "/savestates",
            "--texture-load-directory", root + "/textures/load",
            "--texture-dump-directory", root + "/textures/dump",
            "--title-id", "0004000000125500", "--width", "1280", "--height", "720",
            "--gameplay-timing", "native30_no_interpolation", "--presentation-rate", "30",
            "--serial-presentation"
        }}
    };
}
}
