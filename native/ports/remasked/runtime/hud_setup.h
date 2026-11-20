// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" bool remasked_prepare_hud(const char* staging,
                                    const uint8_t* archive, size_t archiveSize,
                                    const uint8_t* atlas, size_t atlasSize);
