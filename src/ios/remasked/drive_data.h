// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#import <Foundation/Foundation.h>
// All gameplay reads remain in the application sandbox. Never call while the
// guest is running. Conflicts are resolved explicitly before any file is changed.
namespace ReMaskedDrive {
enum class Resolution { Ask, Local, Drive };
enum class Content { Native, AzaharData, AzaharSaves, AzaharBackup, AzaharCheck };
inline NSString* AzaharContainerIdentifier() {
    return @"iCloud.io.github.otpr26.remasked.azaharprototype.ios";
}
inline NSString* AzaharSavesPath() {
    return @"Azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/00040000/00125500/data/00000001";
}
struct Result { bool ok=false; bool conflict=false; NSString* message=nil; bool pendingImport=false; };
NSString* FolderName();
NSString* Status();
bool Enabled();
void Disconnect();
void Sync(NSURL* local, NSURL* selected, Resolution resolution,
          void (^completion)(Result), Content content=Content::Native);
// Synchronous transaction shared by the serial worker and Foundation tests.
Result Reconcile(NSURL* local, NSURL* folder, Resolution resolution, Content content=Content::Native);
}
