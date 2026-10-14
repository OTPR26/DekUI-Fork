// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

namespace MM3D::Notebook::Paths {
inline QString Relative(const char* name) {
    return QStringLiteral("setup/original/romfs/layout/") + QString::fromLatin1(name);
}
inline bool Standard(QString path, const char* name) {
    path.replace('\\', '/'); // Recognize older Windows paths on every platform.
    const auto relative = Relative(name);
    return path.isEmpty() || path == relative || path.endsWith('/' + relative);
}
inline QString Resolve(const QString& saved, const char* name, const QString& current_default) {
    // Never fall back to an old installation, even if its files still exist.
    // Missing resources in the current installation must report their own error.
    return Standard(saved, name) ? current_default : saved;
}
inline QString Stored(const QString& runtime_path, const char* name) {
    return Standard(runtime_path, name) ? Relative(name) : runtime_path;
}
}
