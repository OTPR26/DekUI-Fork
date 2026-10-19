// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QSettings>
#include <QString>
#include "common/settings.h"

namespace MM3D::ControllerBindings {
inline constexpr int Buttons[]{Settings::NativeButton::A, Settings::NativeButton::B,
    Settings::NativeButton::X, Settings::NativeButton::Y, Settings::NativeButton::Select,
    Settings::NativeButton::Start, Settings::NativeButton::Home, Settings::NativeButton::L,
    Settings::NativeButton::R, Settings::NativeButton::ZL, Settings::NativeButton::ZR,
    Settings::NativeButton::Up, Settings::NativeButton::Down, Settings::NativeButton::Left,
    Settings::NativeButton::Right};
// Startup and gameplay share this transaction. Publish to disk and the loaded
// profile together so the next emulator settings save cannot restore old binds.
inline bool Save(const QString& path, int index, const QString& binding, bool analog = false) {
    if (index < 0 || index >= (analog ? Settings::NativeAnalog::NumAnalogs
                                    : Settings::NativeButton::NumButtons))
        return false;
    QSettings ini(path, QSettings::IniFormat);
    const int selected = ini.value(QStringLiteral("Controls/profile"), 0).toInt();
    const int count = ini.value(QStringLiteral("Controls/profiles/size"), 0).toInt();
    if (selected < 0 || (count > 0 && selected >= count))
        return false;
    const QString prefix = count > 0 ? QStringLiteral("Controls/profiles/%1/").arg(selected + 1)
                                     : QStringLiteral("Controls/");
    const auto name = analog ? QString::fromStdString(Settings::NativeAnalog::mapping[index])
                             : QString::fromUtf8(Settings::NativeButton::mapping[index]);
    ini.setValue(prefix + name, binding);
    ini.setValue(prefix + name + QStringLiteral("/default"), false);
    // Explicit mappings remain custom even when their values match the
    // automatic template.
    const bool automatic = ini.value(prefix + QStringLiteral("name")).toString() ==
                           QStringLiteral("Controller (automatic)");
    if (automatic) {
        ini.setValue(prefix + QStringLiteral("name"), QStringLiteral("Controller (custom)"));
        ini.setValue(prefix + QStringLiteral("name/default"), false);
    }
    ini.sync();
    if (ini.status() != QSettings::NoError)
        return false;
    if (selected < static_cast<int>(Settings::values.input_profiles.size())) {
        if (selected != Settings::values.current_input_profile_index)
            Settings::LoadProfile(selected);
        auto& current = Settings::values.current_input_profile;
        if (analog)
            current.analogs[index] = binding.toStdString();
        else
            current.buttons[index] = binding.toStdString();
        if (automatic)
            current.name = "Controller (custom)";
        Settings::SaveProfile(selected);
    }
    return true;
}
} // namespace MM3D::ControllerBindings
