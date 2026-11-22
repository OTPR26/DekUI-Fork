// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <libretro.h>
#include "mm3d/hud_adjustments.h"

inline constexpr unsigned RemaskedGetSettings = RETRO_ENVIRONMENT_PRIVATE | 2;

struct RemaskedGameplayOptions {
    bool disableBomberApproach = false, swordActivateOwls = false, classicDoubleTime = false;
    bool fastTime = false, weakerEnhancedDefense = false;
    bool disableHeartDrops = false, disableRupeeDrops = false, disableFairyDrops = false;
    bool disableMagicDrops = false, disableAmmoPickups = false;
    bool loseRupeesOnDeath = false, endWorldOnDeath = false, advanceTimeOnDeath = false;
    bool doubleShopPrices = false, classicSaving = false;
    int damageMultiplier = 1, redPotionHearts = 20, bluePotionHearts = 20, bottledFairyHearts = 10;
    friend bool operator==(const RemaskedGameplayOptions&, const RemaskedGameplayOptions&) = default;
};

// Shared by the UIKit frontend and the local Azahar core.
struct RemaskedGameSettings {
    // Logo, copyright, Press Start, loading icon. Each pose is X, Y, and scale.
    std::array<std::array<float, 3>, 4> title{{
        {-9, -9, .90f}, {-.89f, 17.92f, .60f}, {0, -3.60f, .47f}, {20.90f, .10f, .54f}}};
    int hudSize = 0;
    int hudOpacity = 100;
    struct HudPart {
        int size = 0, x = 0, y = 0;
        friend bool operator==(const HudPart&, const HudPart&) = default;
    };
    std::array<HudPart, MM3D::HudAdjustments::Count> hudParts{};
    int magicHeight = 0;
    int heartSpacing = 0;
    bool hideMenuPrompts = false;
    bool disableTatlReminders = false;
    bool muteMusic = false;
    bool disableLowHealthBeep = false;
    std::array<float, 3> speed{{1, 1, 1}};
    float cameraDistance = 1;
    RemaskedGameplayOptions gameplay;
    friend bool operator==(const RemaskedGameSettings&, const RemaskedGameSettings&) = default;
};
