// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string_view>
namespace ReMaskedSaveSync {
enum class Action { None, Upload, Download, Conflict };
inline Action Decide(std::string_view local, std::string_view remote, std::string_view base) {
    if (local == remote) return Action::None;
    if (remote.empty()) return local.empty() ? Action::None : (base.empty() ? Action::Upload : Action::Conflict);
    if (local.empty()) return Action::Download; // Cache eviction is never a cloud deletion.
    if (base.empty()) return Action::Conflict;
    if (local == base) return Action::Download;
    if (remote == base) return Action::Upload;
    return Action::Conflict;
}
inline bool SaveName(std::string_view name) {
    return name == "save0.bin" || name == "save1.bin" || name == "save2.bin";
}
}
