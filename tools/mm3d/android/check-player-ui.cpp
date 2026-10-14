// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/diagnostics.h"

int main() {
    // This target is checked with the actual Android compiler, and run locally
    // with the Android UI policy defined. No Qt or Vulkan device is needed.
    if (!MM3D::PlayerOverlay()) return 1;
    if (MM3D::ToggleOverlayView(false)) return 2; // renderer not ready
    MM3D::presentation_available = true;
    if (!MM3D::ToggleOverlayView(false) || !MM3D::overlay_visible) return 3;
    if (!MM3D::PlayerOverlay()) return 4; // even an editor request stays player UI
    if (!MM3D::ToggleOverlayView(true) || MM3D::overlay_visible) return 5;
    return 0;
}
