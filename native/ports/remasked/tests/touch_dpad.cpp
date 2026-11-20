// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../../titles/mm3d/dekui/touch_hud_placement.h"
#include <stdexcept>
#include <iostream>
int main(){
    using namespace Mm3dDekui;
    NativeInventory::Hook::Affine original{2,0,0,2,30,40};
    TouchHud::dpad.scale=1.5f;TouchHud::dpad.x=7;TouchHud::dpad.y=9;TouchHud::dpad.active=true;
    const auto moved=TouchHud::Compose(TouchHud::dpad,original);
    if(moved.xx!=3||moved.yy!=3||moved.tx!=52||moved.ty!=69)throw std::runtime_error("D-pad touch placement must compose after HUD settings");
    TouchHud::dpad.active=false;const auto cleared=TouchHud::Compose(TouchHud::dpad,original);
    if(cleared.xx!=2||cleared.tx!=30||cleared.ty!=40)throw std::runtime_error("Controller layout not restored");
    std::cout<<"PASS: touch D-pad mapping and controller HUD restoration\n";
}
