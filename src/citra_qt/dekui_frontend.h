// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
class QWidget;
namespace MM3D::DesktopFrontend {
// Startup and settings use the same saved values as the Android-reference menu.
QWidget* CreateLauncher(QWidget* parent, std::function<void()> start, std::function<void()> quit);
void OpenLauncherMods(QWidget* launcher);
void ApplySelectedIcon();
void RuntimeInput();
#ifdef __APPLE__
void SyncCloudSaves(QWidget* parent, bool manual = false);
void OpenCloudSettings(QWidget* parent, bool running);
#endif
void Settings(QWidget* parent, bool running = false);
} // namespace MM3D::DesktopFrontend
