// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/custom_textures/material.h"
#include "video_core/pica/regs_texturing.h"

namespace MM3D {
template <class Surface>
bool ApplyHudTextureSampling(const Surface& surface, Pica::TexturingRegs::TextureConfig& config) {
    if (!surface.IsCustom() || !surface.material) {
        return false;
    }
    switch (surface.material->hash) {
    case 0x0d012e5ac6d2eb36ULL: // Restoration 8× atlas
    case 0xa4890349af9fffb4ULL: // ReMasked ETC1A4 atlas
    case 0xe2cebcb19da2608fULL: // Decoded-pixel hash of the same atlas
        using Filter = Pica::TexturingRegs::TextureConfig::TextureFilter;
        config.min_filter.Assign(Filter::Linear);
        config.mag_filter.Assign(Filter::Linear);
        return true;
    default:
        return false;
    }
}
} // namespace MM3D
