#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int ReMaskedIOS_TouchControlsAvailable(void);
void ReMaskedIOS_SetTouchControlsEnabled(int enabled);
void ReMaskedIOS_StopTouchControls(void);
void ReMaskedIOS_SetTouchOpacity(float opacity);
void ReMaskedIOS_SetTouchScale(float scale);
void ReMaskedIOS_SetTouchOptions(int labels,int drag);
void ReMaskedIOS_SetControlSize(int control,float scale);
void ReMaskedIOS_SetExtraShoulders(int enabled);
void ReMaskedIOS_SetTouchHudIcons(int enabled);
void ReMaskedIOS_SetFaceButtonLayout(int layout);
void ReMaskedIOS_SetTouchLeftStickFloating(int floating);
void ReMaskedIOS_SetTouchControlsMenuVisible(int visible);

void ReMaskedIOS_SetManualJump(int mode);
void ReMaskedIOS_EditTouchLayout(void);
void ReMaskedIOS_SetTouchDualLayout(int dual);

#ifdef __cplusplus
}
#endif
