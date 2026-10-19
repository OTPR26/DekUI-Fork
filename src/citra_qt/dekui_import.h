// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
class QVBoxLayout;
class QWidget;
namespace MM3D::DesktopImport {
QString NativeSettingsPath();
void ApplyPendingSettings();
void SyncTextureSource();
void ChooseTextures(QWidget* parent, bool clear = false);
bool CaptureBinding(QWidget* parent, unsigned index, bool clear = false);
void RefreshBindings();
void EnsureController();
void Finalize(const QString& stage, const QString& root, const QString& pack, bool dark,
              bool twilight = false);
void BindingControls(QVBoxLayout* layout, QWidget* parent);
void VolumeControl(QVBoxLayout* layout, bool running);
} // namespace MM3D::DesktopImport
