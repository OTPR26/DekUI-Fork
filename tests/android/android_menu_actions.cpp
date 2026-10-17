// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdexcept>
#include <iostream>
#include "mm3d/android_menu.h"
static void Check(bool value) { if (!value) throw std::runtime_error("Android menu request failed"); }
int main() {
    using namespace MM3D::AndroidMenu;
    Check(std::size(BindingLabels)==21);
    Request(Bind,20);int event=pending.exchange(0);
    Check((event>>16)==Bind && (event&65535)==20);
    Request(TouchOpacity,100);Request(SwapScreens);
    event=pending.exchange(0);Check((event>>16)==TouchOpacity && (event&65535)==100);
    Request(SwapScreens);Check((pending.exchange(0)>>16)==SwapScreens);
    { std::scoped_lock lock{mutex};state.capture=4;state.bindings={"A","B"};state.touch=true; }
    auto copy=Snapshot();copy.bindings[0]="Changed";
    Check(Snapshot().bindings[0]=="A" && Snapshot().capture==4 && Snapshot().touch);
    std::cout << "Android menu actions and synchronized snapshots passed\n";
}
