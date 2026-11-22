// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <atomic>

namespace MM3D::RomRegion {
inline constexpr std::uint64_t USA = 0x0004000000125500ULL;
inline constexpr std::uint64_t EUR = 0x0004000000125600ULL;

inline std::atomic<bool> european_mode{false};

inline void SetEuropeanPreview(bool enabled) {
    european_mode.store(enabled, std::memory_order_relaxed);
}

inline bool EuropeanPreview() {
    return european_mode.load(std::memory_order_relaxed);
}

inline bool UsesRemaskedPresentation(std::uint64_t title) {
#ifdef IOS
    // The iOS host verifies the cartridge before enabling the separate EUR data tree.
    return title == USA || title == EUR;
#else
    return title == USA || (title == EUR && EuropeanPreview());
#endif
}

// The preview has its own user tree. Reuse the common HUD and patch namespace
// inside that tree while retaining the real title ID for the game and its saves.
inline std::uint64_t PresentationAssetTitle(std::uint64_t title) {
    return title == EUR && UsesRemaskedPresentation(title) ? USA : title;
}
} // namespace MM3D::RomRegion
