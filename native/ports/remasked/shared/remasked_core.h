// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RMCore RMCore;
typedef struct RMHudElement {
    float x, y, width, height, opacity;
    int visible;
} RMHudElement;
// Input values intentionally match TriAevum's service ABI, not Android keycodes.
enum { RM_A=0, RM_B=1, RM_SELECT=2, RM_START=3, RM_RIGHT=4, RM_LEFT=5,
       RM_UP=6, RM_DOWN=7, RM_R=8, RM_L=9, RM_X=10, RM_Y=11, RM_ZL=14, RM_ZR=15 };
RMCore* rm_create(const char* json, size_t length);
void rm_destroy(RMCore* core);
// Returns required bytes including NUL. Insufficient buffers are not modified.
size_t rm_copy_config(RMCore* core, char* output, size_t capacity);
int rm_set_hud_number(RMCore* core, const char* key, double value);
double rm_hud_number(RMCore* core, const char* key);
int rm_hud_element(RMCore* core, unsigned index, RMHudElement* output);
int rm_hud_bounds(RMCore* core, RMHudElement* output);
int rm_input_button(RMCore* core, unsigned button, int down);
int rm_input_source_button(RMCore* core, unsigned source, unsigned button, int down);
int rm_input_sticks(RMCore* core, float lx, float ly, float rx, float ry);
uint32_t rm_input_mask(RMCore* core);
void rm_release_inputs(RMCore* core);
void rm_release_source(RMCore* core, unsigned source);
unsigned rm_settings_count(RMCore* core);
#ifdef REMASKED_NATIVE_NRI
// Optional real-backend diagnostic; does not imply game rendering is ready.
int rm_renderer_device_check(void);
#endif
#ifdef REMASKED_NATIVE_TITLE
size_t rm_title_entry_count(void);
#endif
#ifdef __cplusplus
}
#endif
