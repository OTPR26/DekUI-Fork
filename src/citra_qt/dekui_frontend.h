// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
class QWidget;
namespace MM3D::DesktopFrontend {
// Startup and settings use the same saved values as the Android-reference menu.
bool Startup(QWidget* parent = nullptr, bool open_mods = false);
void RuntimeInput();
void Settings(QWidget* parent, bool running = false);
} // namespace MM3D::DesktopFrontend
