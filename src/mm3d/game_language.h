// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/player_settings.h"

namespace MM3D::PlayerSettings {
// CFG is owned by the emulation thread, including while the game menu is open.
template<class Config>
void InitializeGameLanguage(Config& config) {
    game_language.store(config.GetSystemLanguage());
    game_language_status.store(0);
    set_game_language.store(-1);
}

template<class Config>
void ApplyPendingGameLanguage(Config& config) {
    const int requested = set_game_language.exchange(-1);
    if (!SupportedLanguage(requested)) return;
    const auto previous = config.GetSystemLanguage();
    using Language = decltype(config.GetSystemLanguage());
    config.SetSystemLanguage(static_cast<Language>(requested));
    if (config.GetSystemLanguage() != requested) {
        config.SetSystemLanguage(previous);
        game_language.store(previous);
        game_language_status.store(3);
        return;
    }
    const bool saved = config.UpdateConfigNANDSavegame().IsSuccess();
    if (!saved) config.SetSystemLanguage(previous);
    game_language.store(config.GetSystemLanguage());
    game_language_status.store(saved ? 2 : 3);
}
} // namespace MM3D::PlayerSettings
