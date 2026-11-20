// SPDX-License-Identifier: GPL-3.0-or-later
#include "save_sync.h"
#include "fast/extension/graphics_settings_window.h"
#include <imgui.h>
#include <memory>

bool ReMaskedAppleHudAvailable();
bool ReMaskedAppleHudEnabled();
void ReMaskedAppleSetHudEnabled(bool enabled);
namespace {
class HudPanel final : public Fast::Extension::GraphicsSettingsPanelTab {
public:
    const char* Label() const noexcept override { return "ReMasked HUD"; }
    const char* Group() const noexcept override { return Fast::Extension::kGameSettingsGroup; }
    void Draw() override {
        bool enabled = ReMaskedAppleHudEnabled();
        ImGui::BeginDisabled(!ReMaskedAppleHudAvailable());
        if (ImGui::Checkbox("Use ReMasked / Project Restoration HUD", &enabled)) ReMaskedAppleSetHudEnabled(enabled);
        ImGui::EndDisabled();
        ImGui::TextWrapped("Uses the HUD layout from your ReMasked setup, with your custom textures. Changes apply when you reopen the app.");
    }
};

class SavesPanel final : public Fast::Extension::GraphicsSettingsPanelTab {
public:
    const char* Label() const noexcept override { return "Saves & iCloud"; }
    void Draw() override {
        char status[512]{};
        ReMaskedSaveSyncStatus(status, sizeof(status));
        ImGui::TextWrapped("%s", status);
        bool enabled = ReMaskedSaveSyncEnabled();
        if (ImGui::Checkbox("Sync saves with iCloud", &enabled)) ReMaskedSaveSyncSetEnabled(enabled);
        if (ImGui::Button("Check iCloud now")) ReMaskedSaveSyncRequest();
        ImGui::TextWrapped("Downloaded saves are applied before gameplay starts. Finish saving, then reopen the app to use progress from another device.");
        if (ReMaskedSaveSyncConflict())
            ImGui::TextWrapped("Different progress exists in iCloud. Reopen the app to choose which copy to use; your current game stays unchanged.");
        ImGui::Separator();
        ImGui::TextWrapped("Your texture pack and HUD are bundled with the app. Save recovery keeps a separate local copy after each game save.");
    }
};
}
std::shared_ptr<Fast::Extension::GraphicsSettingsPanelTab> CreateReMaskedSavesPanel() {
    return std::make_shared<SavesPanel>();
}

std::shared_ptr<Fast::Extension::GraphicsSettingsPanelTab> CreateReMaskedHudPanel() {
    return std::make_shared<HudPanel>();
}
