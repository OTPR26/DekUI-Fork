// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Foundation/Foundation.h>

// Returns an error message or nil after installing the verified ROM and HUD.
NSString* ImportEuropeanROM(NSURL* input, NSURL* dataRoot, NSURL* destination);
