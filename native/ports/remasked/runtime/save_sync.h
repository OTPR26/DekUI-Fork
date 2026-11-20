// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if defined(__APPLE__)
extern "C" {
void ReMaskedSaveSyncConfigure(const char* saves);
bool ReMaskedSaveSyncRecoverLocal(const char* saves);
void ReMaskedSaveSyncTick(bool startup);
void ReMaskedSaveSyncRequest();
bool ReMaskedSaveSyncBusy();
bool ReMaskedSaveSyncCloudMatchesLocal();
void ReMaskedSaveSyncCommitted();
bool ReMaskedSaveSyncEnabled();
void ReMaskedSaveSyncSetEnabled(bool enabled);
void ReMaskedSaveSyncStatus(char* text, unsigned long size);
bool ReMaskedSaveSyncConflict();
void ReMaskedSaveSyncResolve(bool useCloud);
}
#else
inline void ReMaskedSaveSyncConfigure(const char*) {}
inline bool ReMaskedSaveSyncRecoverLocal(const char*) { return true; }
inline void ReMaskedSaveSyncTick(bool) {}
inline void ReMaskedSaveSyncCommitted() {}
#endif
