// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QKeySequence>

inline QKeySequence DefaultFullscreenShortcut() {
#ifdef Q_OS_MAC
    // Qt swaps Control/Meta names on macOS; together these are Control+Command.
    return QKeySequence(Qt::CTRL | Qt::META | Qt::Key_F);
#else
    return QKeySequence(Qt::Key_F11);
#endif
}
