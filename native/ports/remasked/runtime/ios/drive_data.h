// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#import <Foundation/Foundation.h>
// All gameplay reads remain in the application sandbox. Never call while the
// guest is running. Conflicts are resolved explicitly before any file is changed.
namespace ReMaskedDrive {
enum class Resolution { Ask, Local, Drive };
struct Result { bool ok=false; bool conflict=false; NSString* message=nil; };
NSString* FolderName();
NSString* Status();
bool Enabled();
void Disconnect();
void Sync(NSURL* local, NSURL* selected, Resolution resolution,
          void (^completion)(Result));
// Synchronous transaction shared by the serial worker and Foundation tests.
Result Reconcile(NSURL* local, NSURL* folder, Resolution resolution);
}
