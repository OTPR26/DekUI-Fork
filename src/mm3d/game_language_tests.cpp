// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/game_language.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
enum Language { English = 1, French = 2, Spanish = 5 };
struct Result {
    bool success;
    bool IsSuccess() const { return success; }
};
struct Config {
    Language current = Language::English;
    Language persisted = Language::English;
    bool writable = true;
    bool rejects_language = false;
    int saves = 0;
    Language GetSystemLanguage() const { return current; }
    void SetSystemLanguage(Language language) {
        if (!rejects_language) current = language;
    }
    Result UpdateConfigNANDSavegame() {
        ++saves;
        if (writable) persisted = current;
        return {writable};
    }
    void Restart() { current = persisted; }
};
void Check(bool pass, const char* message) {
    if (!pass) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}
int main() {
    using namespace MM3D::PlayerSettings;
    Config config;
    InitializeGameLanguage(config);
    Check(game_language == 1 && game_language_status == 0, "initialize from CFG");
    ApplyPendingGameLanguage(config);
    Check(config.saves == 0, "idle frames must not write NAND");
    for (int language : {2, 5, 1}) {
        game_language = language;
        game_language_status = 1;
        set_game_language = language;
        ApplyPendingGameLanguage(config);
        Check(set_game_language == -1 && game_language_status == 2,
              "consume selection and report a successful save");
        Check(config.GetSystemLanguage() == language && game_language == language,
              "publish the saved native CFG language");
        config.Restart();
        game_language = 99;
        InitializeGameLanguage(config);
        Check(game_language == language, "selection survives shutdown and restart");
    }
    const int saves = config.saves;
    for (int invalid : {-1, 0, 3, 99}) {
        set_game_language = invalid;
        ApplyPendingGameLanguage(config);
    }
    Check(config.saves == saves, "reject unsupported CFG language IDs");
    config.writable = false;
    set_game_language = 2;
    ApplyPendingGameLanguage(config);
    Check(game_language == 1 && config.GetSystemLanguage() == 1 && game_language_status == 3,
          "failed NAND write restores the previous language and reports failure");
    config.writable = true;
    config.rejects_language = true;
    set_game_language = 5;
    ApplyPendingGameLanguage(config);
    Check(config.saves == saves + 1 && game_language == 1 && game_language_status == 3,
          "region-rejected language must not be reported as saved");
    config.rejects_language = false;
    set_game_language = 5;
    ApplyPendingGameLanguage(config); // Shutdown flush uses the same emulation-thread path.
    config.Restart();
    InitializeGameLanguage(config);
    Check(game_language == 5 && game_language_status == 0,
          "a request immediately before shutdown survives restart");
    std::puts("Game language persistence and failure recovery checks passed.");
}
