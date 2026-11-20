// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
bool ReMaskedAppleDuoAvailable();
bool ReMaskedAppleDuoRequested();
void ReMaskedAppleSetDuoRequested(bool enabled);
void ReMaskedAppleDuoFrame();

namespace Oot3dNativeGame::Menu {struct Document;}
void ReMaskedAppleAppendDisplaySettings(Oot3dNativeGame::Menu::Document& document);
