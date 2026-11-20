// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../../titles/mm3d/adapter/mm3d_restoration.h"
#include "../../../titles/mm3d/restoration/engine_impl.h"
#include "../../../titles/mm3d/restoration/port_tests.inc"
int main(){try {
 using namespace RestorationPortTests;
 Mm3dDekui::PauseNavigation::enabled=false;Schedule();
 Mm3dDekui::PauseNavigation::enabled=true;
 Fixture f;f.Play();auto& g=f.g;
 g.Write32(kItemsScreen,0x08500000);g.Write32(kScheduleScreen,0x08501000);
 g.Write32(Fixture::game+G::pad_state__input__new_buttons,Button::Start);
 f.Run("UiScheduleTriggerHook");Check(g.Read32(kScreenContext+12)==0x08500000,"Start opens Items even before Notebook unlock");
 g.Write32(kScreenContext+12,0);g.Write32(Common+C::save__inventory__collect_register,1u<<18);
 f.Run("UiScheduleTriggerHook");Check(g.Read32(kScreenContext+12)==0x08500000,"Notebook ownership does not steal Start");
 g.Write32(kScreenContext+12,0);g.Write32(Fixture::game+G::pad_state__input__new_buttons,Button::Select);
 f.Run("UiScheduleTriggerHook");Check(g.Read32(kScreenContext+12)==0,"Select never opens guest screen");
 g.Write8(Fixture::game+G::type,kStateFirstGame);g.Write32(Fixture::game+G::pad_state__input__new_buttons,Button::Start);
 f.Run("UiScheduleTriggerHook");Check(g.Read32(kScreenContext+12)==0,"title/file selection Start left native");
 std::cout<<"Android Start/Select routing and legacy shortcut regressions passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
