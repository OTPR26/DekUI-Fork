// SPDX-License-Identifier: GPL-3.0-or-later
#include "oot3d_native_direct_aot.h"
#include <cstddef>
extern "C" size_t rm_title_entry_count() {
    using namespace Oot3dNativeGame;
    if (!Oot3dWholeAotPluginV2Available() ||
        !Oot3dWholeAotPluginObservesMemoryWrites() ||
        !Oot3dWholeAotPluginExitRequestSupported()) return 0;
    return Oot3dWholeAotEntryPoints().size();
}
