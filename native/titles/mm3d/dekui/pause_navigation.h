// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace Mm3dDekui::PauseNavigation {
// Android ReMasked keeps app settings on Select. Other ports retain their
// existing layout until separately requested and tested.
#if defined(__ANDROID__) || defined(REMASKED_IOS_GAME)
inline std::atomic<bool> enabled{true};
#else
inline std::atomic<bool> enabled{false};
#endif
inline constexpr bool NotebookUnlocked(std::uint32_t collect) { return (collect & (1u << 18)) != 0; }
struct SettingsPress {
    bool held=false;
    bool Update(bool down, bool pulse=false) {
        const bool pressed=(down&&!held)||pulse;
        held=down;
        return pressed;
    }
};
}
