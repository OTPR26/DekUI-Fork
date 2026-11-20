// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace Oot3dNativeGame::Menu {struct Document;}
void ReMaskedIOSLaunch();
void ReMaskedIOSFrame();
void ReMaskedIOSAppendSettings(Oot3dNativeGame::Menu::Document&);
void ReMaskedIOSRoute(const std::string&);
void ReMaskedIOSTouchButton(int,bool);
void ReMaskedIOSTouchAxis(int,int);

void ReMaskedIOSApplyIcon(int);

namespace Oot3dNativeGame {class InGameMenu;}
void ReMaskedIOSPresentSettings(Oot3dNativeGame::InGameMenu&);
void ReMaskedIOSNativeMenuFrame();
