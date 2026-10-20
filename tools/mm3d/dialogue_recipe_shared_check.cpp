// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/dialogue_recipe.h"

bool DialogueCacheShared() {
    using namespace MM3D::DialogueText;
    const std::string native="Synthetic fixture: red";
    const Recipe original{std::uint32_t(native.size()),Hash(native),
                          std::uint32_t(native.size()),Hash(native),{}};
    return Resolve(original,{})==native;
}
