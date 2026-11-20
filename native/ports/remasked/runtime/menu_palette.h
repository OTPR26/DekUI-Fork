// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oot3d_menu_presentation.h"
namespace ReMasked {
inline Oot3dNativeGame::Menu::Palette MenuPalette(){
    Oot3dNativeGame::Menu::Palette p;
    p.Text={242.f/255,242.f/255,242.f/255,1};
    p.Accent={185.f/255,125.f/255,242.f/255,1};
    p.Border={134.f/255,99.f/255,170.f/255,1};
    p.Surface={8.f/255,7.f/255,13.f/255,.94f};
    p.Control={18.f/255,12.f/255,28.f/255,.20f};
    p.Interactive={185.f/255,125.f/255,242.f/255,.20f};
    p.Selected={185.f/255,125.f/255,242.f/255,.32f};
    return p;
}
}
