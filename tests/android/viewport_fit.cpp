// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "mm3d/widescreen.h"

static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    using namespace MM3D::Widescreen;
    enabled = true;
    UpdateLayoutSupport(true, true, true, true);
    Check(Active(), "Primary landscape top screen enables widescreen");
    // Reproduce Android's second, bottom-only composition after the main screen.
    for (unsigned frame=0; frame<120; ++frame) {
        UpdateLayoutSupport(false, false, true, true);
        Check(Active(), "Secondary display must not disable the next guest frame's projection");
        Check(-1.04f * HorizontalScale > -1.f && 1.04f * HorizontalScale < 1.f,
              "HUD vertices beyond both native side planes survive widescreen clipping");
        UpdateLayoutSupport(true, true, true, true);
    }
    const auto fitted = FitTop({60,0,1860,1080},{},1920,false);
    Check(fitted.left==0 && fitted.top==0 && fitted.right==1920 && fitted.bottom==1080,
          "The full 16:9 canvas fits within all four edges");
    for (auto size : {Common::Rectangle<unsigned>{0,0,1920,1080},
                      Common::Rectangle<unsigned>{0,0,1600,1200},
                      Common::Rectangle<unsigned>{0,0,2560,1080}}) {
        const auto r=FitTop(size,{},size.right,false);
        Check(r.left<=r.right && r.top<=r.bottom && r.right<=size.right && r.bottom<=size.bottom,
              "Aspect fit must remain inside its canvas");
        Check(std::abs(float(r.GetWidth())/r.GetHeight()-Aspect)<0.002f,
              "Aspect fit preserves 16:9 proportions");
    }
    UpdateLayoutSupport(true, true, false, true);
    Check(!Active(), "Portrait primary layout disables unsupported widescreen");
    UpdateLayoutSupport(false, true, true, true);
    Check(!Active(), "A secondary display cannot enable unsupported primary projection");
    UpdateLayoutSupport(true, true, true, false);
    Check(!Active(), "Stereo primary layout remains unsupported");
    UpdateLayoutSupport(true, true, true, true);
    enabled=false;
    Check(!Active(), "Player widescreen preference is still respected");
    std::cout << "Viewport fitting and primary-display ownership checks passed\n";
}
