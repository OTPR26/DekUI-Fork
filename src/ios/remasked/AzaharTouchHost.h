#pragma once

#import <UIKit/UIKit.h>
#include <cstdint>

inline constexpr int ReMaskedTouchPictograph = 16;

void ReMaskedIOSTouchButton(int button, bool pressed);
void ReMaskedIOSTouchAxis(int axis, int16_t value);
void ReMaskedAzaharReleaseTouch();
CGRect ReMaskedAzaharTouchCanvas(CGRect safeFrame);
CGRect ReMaskedAzaharBottomPicture();

// A, B, X, Y, L, R, then the four direction hit regions.
void ReMaskedAzaharPublishTouchHud(const CGRect* controls);
void ReMaskedIOS_RefreshTouchLayout();

void ReMaskedAzaharSetTouchVisibility(unsigned hiddenControls, bool hideWithController);
void ReMaskedAzaharResetTouchLayout();
