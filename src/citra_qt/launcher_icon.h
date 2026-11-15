// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

namespace MM3D::LauncherIcon {
bool SaveIco(const QString& resource, const QString& destination);
bool SameApplication(const QString& target, const QString& application);
#ifdef Q_OS_WIN
bool UpdateShortcut(const QString& shortcut, const QString& application, const QString& icon_path);
#endif
void ApplyWindows(const QString& icon_path);
} // namespace MM3D::LauncherIcon
