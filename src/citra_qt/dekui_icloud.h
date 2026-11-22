// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

namespace MM3D::DesktopCloud {

enum class Resolution { Ask, Local, Drive };

struct Result {
    QString message;
    bool ok = false;
    bool conflict = false;
};

QString DriveFolder();
// Call only while emulation is stopped. This syncs the Azahar save slots and
// their archive metadata using the same on-disk names as the iPhone app.
Result Sync(const QString& user_root, Resolution resolution = Resolution::Ask);
// Filesystem transaction used inside the coordinated iCloud access and by the fixture test.
Result Reconcile(const QString& user_root, const QString& documents, Resolution resolution);

} // namespace MM3D::DesktopCloud
