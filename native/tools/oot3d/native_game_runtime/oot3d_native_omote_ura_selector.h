#pragma once

#include "oot3d_native_a32_memory.h"

#include <cstdint>

namespace Oot3dNativeGame {

// The title's normal / Master Quest choice is COmoteUraSelector (embedded
// source path ctr/Menu/COmoteUraSelector.cpp), not FileSelect state. It is a
// lane of the root pause UI: PauseUi_Draw returns its owner to the frame
// renderer, which draws it to the lower screen, and SaveContext.game_mode
// stays 1 (gameplay) while it runs.
struct NativeOmoteUraSelectorState {
  // Root pause UI +0x20, written by PauseUi_Enable/DisableOmoteUraSelector.
  bool LaneEnabled = false;
  // gOmoteUraSelector, the exact 0x5CC-byte owner; zero when destroyed.
  std::uint32_t Owner = 0;
  // Owner +0x04 (OmoteUraSelector_UpdateInteractive): 1/2 the title prompt,
  // 3 the panels slide in, 8 the choice, 4..6 the confirmed panels leave,
  // 9 closed.
  std::uint8_t Phase = 0;
  // Owner +0x05: the highlighted panel, 0 left and 1 right.
  std::uint8_t Selection = 0;
  // Owner +0x06: 1 the two-version choice, 2 the single-version prompt.
  std::uint8_t Variant = 0;
  // Guest address of the menu's full-screen background texture: owner
  // +0x498 is the CTXB resource that OmoteUraSelector_LoadTextures builds
  // from rom:/misc/common_bg01.ctxb (element 0), and CtxbResource_BuildTexture
  // stores its texture address at +0x4C. Zero before it is built.
  std::uint32_t BackdropTexture = 0;
};

inline constexpr std::uint32_t kOmoteUraSelectorLaneEnabled =
    0x005043D4U + 0x20U;
inline constexpr std::uint32_t kOmoteUraSelectorOwner = 0x0050BB50U;

inline bool ReadNativeOmoteUraSelectorState(NativeA32Memory &memory,
                                            NativeOmoteUraSelectorState *state) {
  NativeOmoteUraSelectorState result;
  std::uint32_t laneEnabled = 0U;
  if (state == nullptr ||
      !memory.Read32(kOmoteUraSelectorLaneEnabled, &laneEnabled) ||
      !memory.Read32(kOmoteUraSelectorOwner, &result.Owner)) {
    return false;
  }
  result.LaneEnabled = laneEnabled != 0U;
  std::uint32_t backdropResource = 0U;
  if (result.Owner != 0U &&
      (!memory.Read8(result.Owner + 0x04U, &result.Phase) ||
       !memory.Read8(result.Owner + 0x05U, &result.Selection) ||
       !memory.Read8(result.Owner + 0x06U, &result.Variant) ||
       !memory.Read32(result.Owner + 0x498U, &backdropResource))) {
    return false;
  }
  // The background only refines the presentation: without it the menu keeps
  // its native backdrop.
  if (backdropResource != 0U &&
      !memory.Read32(backdropResource + 0x4CU, &result.BackdropTexture)) {
    result.BackdropTexture = 0U;
  }
  *state = result;
  return true;
}

// The two-version choice owns the lower screen from the moment its panels
// enter until the confirmed panels have left. The title prompt before it
// leaves the upper title screen authoritative.
constexpr bool
IsNativeVersionChoicePresented(const NativeOmoteUraSelectorState &state) noexcept {
  return state.LaneEnabled && state.Owner != 0U && state.Variant == 1U &&
         state.Phase >= 3U && state.Phase <= 8U;
}

// The two-version title prompt ("PRESS START") before the panels enter.
constexpr bool
IsNativeVersionChoicePrompting(const NativeOmoteUraSelectorState &state) noexcept {
  return state.LaneEnabled && state.Owner != 0U && state.Variant == 1U &&
         (state.Phase == 1U || state.Phase == 2U);
}

} // namespace Oot3dNativeGame
