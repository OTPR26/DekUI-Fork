// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <UIKit/UIKit.h>
#include "oot3d_menu_model.h"

@interface ReMaskedSettingsPanel : UIViewController
- (instancetype)initWithDocument:(Oot3dNativeGame::Menu::Document)document
                           input:(std::function<std::optional<Oot3dNativeGame::Menu::Input>()>)input
                           close:(void (^)(void))close;
- (void)refreshSettings;
@end
