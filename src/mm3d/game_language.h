// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/player_settings.h"

namespace MM3D::PlayerSettings {
// While emulation is running, CFG is owned by the emulation thread.
template<class Config>
void InitializeGameLanguage(Config& config) {
    game_language.store(config.GetSystemLanguage());
    game_language_status.store(0);
    set_game_language.store(-1);
}

// Startup and in-game selections use the same validated NAND transaction.
template<class Config>
bool SaveGameLanguage(Config& config, int requested) {
    if (!SupportedLanguage(requested)) return false;
    const auto previous = config.GetSystemLanguage();
    using Language = decltype(config.GetSystemLanguage());
    config.SetSystemLanguage(static_cast<Language>(requested));
    if (config.GetSystemLanguage() != requested) {
        config.SetSystemLanguage(previous);
        game_language.store(previous);
        game_language_status.store(3);
        return false;
    }
    const bool saved = config.UpdateConfigNANDSavegame().IsSuccess();
    if (!saved) config.SetSystemLanguage(previous);
    game_language.store(config.GetSystemLanguage());
    game_language_status.store(saved ? 2 : 3);
    return saved;
}

template<class Config>
void ApplyPendingGameLanguage(Config& config) {
    const int requested = set_game_language.exchange(-1);
    if (SupportedLanguage(requested)) SaveGameLanguage(config, requested);
}
} // namespace MM3D::PlayerSettings
