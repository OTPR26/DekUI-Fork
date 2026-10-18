// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/item_slide_interpolation.h"
#include "mm3d/inventory_interpolation.h"
#include "mm3d/file_select_interpolation.h"
#include "mm3d/message_interpolation.h"
#include "mm3d/banner_interpolation.h"
#include "mm3d/magic_meter_interpolation.h"
#include "mm3d/heart_interpolation.h"

namespace MM3D::CpuInterpolation {
// Shared with the recorder tests: midpoint uniform overrides require retained
// vertex inputs even for UI batches that do not use the world-model shader.
inline bool CaptureShader(std::uint64_t shader, std::uint64_t swizzle, unsigned vertices,
                          float color_reference, std::uint64_t frame,
                          std::uint32_t generation,
                          const MagicMeterInterpolation::Row& uv0={},
                          const MagicMeterInterpolation::Row& uv1={}) {
    return (shader == 0x89b1ab98da3d9241ULL && swizzle == 0x6f0b398b1fc186a9ULL) ||
           // Retain candidate font batches, including partial credits lines.
           // Replay separately validates glyph geometry, tint and scroll motion.
           (FileSelectInterpolation::Layout(shader, swizzle, vertices) && vertices <= 78 &&
            std::isfinite(color_reference) && std::floor(color_reference) == color_reference &&
            -color_reference >= 10 + 4 * (vertices / 6) && -color_reference <= 95) ||
           MessageInterpolation::Candidate(shader, swizzle, vertices, color_reference) ||
           (color_reference==1.f && MessageInterpolation::Candidate(shader,swizzle,vertices,color_reference,true) &&
            MessageInterpolation::Active(frame,generation)) ||
           MagicMeterInterpolation::Candidate(shader,swizzle,vertices,color_reference,uv0,uv1) ||
           HeartInterpolation::Candidate(shader,swizzle,vertices,color_reference,uv0,uv1) ||
           (ItemSlideInterpolation::Candidate(shader, swizzle, vertices) &&
            ItemSlideInterpolation::HasRecentPopup(frame, generation)) ||
           (FileSelectInterpolation::Layout(shader, swizzle, vertices) &&
            (FileSelectInterpolation::Owner(frame, generation) ||
             InventoryInterpolation::Owner(frame, generation) ||
             BannerInterpolation::Owner(frame, generation)));
}
}
