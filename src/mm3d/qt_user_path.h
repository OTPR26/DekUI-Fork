// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include "common/file_util.h"
#ifdef ANDROID
#include "common/android_utils.h"
#endif

namespace MM3D {
// Qt bypasses FileUtil, so Android virtual user paths must be resolved first.
// Return only for Qt I/O; FileUtil still expects its virtual paths on Android.
inline QString QtUserPath(FileUtil::UserPath path) {
#ifdef ANDROID
    return QString::fromStdString(AndroidUtils::TranslateFilePath(FileUtil::GetUserPath(path)));
#else
    return QString::fromStdString(FileUtil::GetUserPath(path));
#endif
}
} // namespace MM3D
