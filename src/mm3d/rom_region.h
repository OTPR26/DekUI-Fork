// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace MM3D::RomRegion {
inline constexpr std::uint64_t USA = 0x0004000000125500ULL;
inline constexpr std::uint64_t EUR = 0x0004000000125600ULL;

inline bool EuropeanPreview() {
#if defined(MM3D_EUR_PREVIEW)
    return true;
#elif defined(ENABLE_MM3D_GAME_FOLDER) || defined(IOS)
    static const bool enabled = [] {
        const char* value = std::getenv("REMASKED_EUR_PREVIEW");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
#else
    return false;
#endif
}

inline bool UsesRemaskedPresentation(std::uint64_t title) {
    return title == USA || (title == EUR && EuropeanPreview());
}

// The preview has its own user tree. Reuse the common HUD and patch namespace
// inside that tree while retaining the real title ID for the game and its saves.
inline std::uint64_t PresentationAssetTitle(std::uint64_t title) {
    return title == EUR && EuropeanPreview() ? USA : title;
}
} // namespace MM3D::RomRegion
