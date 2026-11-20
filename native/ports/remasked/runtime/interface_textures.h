// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <charconv>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace ReMasked {
// The verified owner manifest identifies UI artwork without treating personal
// character textures, backup folders or the whole HD scenery pack as pinned UI.
inline std::vector<uint64_t> InterfaceTextureHashes(const std::filesystem::path& content) {
    std::ifstream stream(content / "owner-content.json");
    nlohmann::json manifest; stream >> manifest;
    std::set<uint64_t> hashes;
    for (const auto& [name, digest] : manifest.at("files").items()) {
        const std::filesystem::path path(name);
        bool ui = false;
        if (!path.is_absolute()) {
            auto part = path.begin();
            if (part != path.end() && (*part == "HudOverlay" || *part == "HenrikoMobile")) {
                ++part;
                ui = part != path.end() && (*part == "UI" || *part == "CUSTOM_HUD");
            }
        }
        if (!ui || name.find("..") != std::string::npos) continue;
        const auto file = path.filename().string();
        if (!file.starts_with("tex1_") || !file.ends_with("_mip0.png")) continue;
        const auto start = file.find('_', 5);
        if (start == std::string::npos || start + 17 >= file.size() || file[start+17] != '_') continue;
        uint64_t hash = 0;
        const auto parsed = std::from_chars(file.data()+start+1, file.data()+start+17, hash, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == file.data()+start+17 && hash) hashes.insert(hash);
    }
    if (hashes.size() > 1024) throw std::runtime_error("Too many interface texture identities");
    return {hashes.begin(), hashes.end()};
}
}
