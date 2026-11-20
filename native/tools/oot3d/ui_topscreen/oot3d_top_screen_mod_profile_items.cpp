// TopScreen mod profile: item lanes, item queries and item selection.
#include "oot3d_top_screen_mod_profile_internal.h"

namespace Oot3dNativeGame {

bool ResolveTopScreenItemLane(const TopScreenItemLaneQuery &query) noexcept {
  if (query.CompatibilityModeActive)
    return true;
  if (!query.ExtendedButtonHeld) {
    return query.NativeResult;
  }
  return (query.SuppressionFlags & query.SuppressionMask) == 0U
             ? true
             : query.NativeResult;
}

std::span<const TopScreenItemQueryContract>
TopScreenVerifiedItemQueryContracts() noexcept {
  return kVerifiedItemQueryContracts;
}

std::array<bool, 4> BuildTopScreenItemCompatibilityOverrides(
    const TopScreenExtendedInputFrame &input, std::uint8_t itemISlotIdentity,
    std::uint8_t itemIISlotIdentity) noexcept {
  if (input.RestorationLayout || input.GameplayDpadActionsOwned) {
    return {};
  }
  const bool itemI = input.DpadLeftHeld && itemISlotIdentity == 0x45U;
  const bool itemII = input.DpadRightHeld && itemIISlotIdentity == 0x46U;
  return {itemI, itemII, itemI, itemII};
}

bool HasTopScreenSlotItemOverrideInput(
    const TopScreenExtendedInputFrame &input) noexcept {
  return !input.RestorationLayout && !input.GameplayDpadActionsOwned &&
         (input.DpadLeftHeld || input.DpadRightHeld);
}

bool HasTopScreenItemQueryOverrideInput(
    const TopScreenExtendedInputFrame &input) noexcept {
  if (HasTopScreenSlotItemOverrideInput(input)) {
    return true;
  }
  if (input.RestorationLayout) {
    return (input.ZrHeld &&
            (input.XPressed || input.YPressed || input.XHeld || input.YHeld)) ||
           (input.ZrPressed && (input.XHeld || input.YHeld));
  }
  return input.ZrPressed || input.ZlPressed || input.ZrHeld || input.ZlHeld;
}

bool ResolveTopScreenItemQuery(TopScreenItemQuery query,
                               const TopScreenItemQueryState &state) noexcept {
  const auto index = ItemQueryIndex(query);
  const bool extendedButton = [&]() {
    if (state.Input.RestorationLayout) {
      switch (query) {
      case TopScreenItemQuery::ItemIPressed:
        return (state.Input.ZrHeld && state.Input.XPressed) ||
               (state.Input.ZrPressed && state.Input.XHeld);
      case TopScreenItemQuery::ItemIIPressed:
        return (state.Input.ZrHeld && state.Input.YPressed) ||
               (state.Input.ZrPressed && state.Input.YHeld);
      case TopScreenItemQuery::ItemIHeld:
        return state.Input.ZrHeld && state.Input.XHeld;
      case TopScreenItemQuery::ItemIIHeld:
        return state.Input.ZrHeld && state.Input.YHeld;
      }
      return false;
    }
    switch (query) {
    case TopScreenItemQuery::ItemIPressed:
      return state.Input.ZrPressed;
    case TopScreenItemQuery::ItemIIPressed:
      return state.Input.ZlPressed;
    case TopScreenItemQuery::ItemIHeld:
      return state.Input.ZrHeld;
    case TopScreenItemQuery::ItemIIHeld:
      return state.Input.ZlHeld;
    }
    return false;
  }();
  return ResolveTopScreenItemLane(
      {state.NativeResults[index], extendedButton,
       state.CompatibilityOverrides[index], state.SuppressionFlags,
       kVerifiedItemQueryContracts[index].SuppressionMask});
}

std::uint8_t
ResolveTopScreenSlotItemId(const TopScreenSlotItemQuery &query) noexcept {
  if (query.NativeSpecialStateActive) {
    return query.NativeItemId;
  }
  if (query.Slot == 3U && query.ItemICompatibilityOverride) {
    return 0x45U;
  }
  if (query.Slot == 4U && query.ItemIICompatibilityOverride) {
    return 0x46U;
  }
  return query.NativeItemId;
}

TopScreenItemsSelectionPlan ResolveTopScreenItemsSelectionBegin(
    const TopScreenItemsSelectionBegin &input) noexcept {
  if (input.NativePageState != 2U) {
    return {};
  }
  if (input.ZrPressed) {
    return {true, 5U};
  }
  if (input.ZlPressed) {
    return {true, 0x17U};
  }
  return {};
}

bool CompleteTopScreenItemsSelection(std::uint32_t pendingSelection,
                                     std::uint32_t nativeSelectionState,
                                     std::uint32_t *selection,
                                     std::uint16_t *cursorY) noexcept {
  if (pendingSelection == 0U || nativeSelectionState != 0x0BU) {
    return false;
  }
  if (selection != nullptr) {
    *selection = pendingSelection;
  }
  if (cursorY != nullptr) {
    *cursorY = pendingSelection == 5U ? 7U : 0xBFU;
  }
  return true;
}

} // namespace Oot3dNativeGame
