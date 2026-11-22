// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace ReMaskedDual {
inline std::atomic<bool> Requested{false},Available{false},Active{false},MenuVisible{false};
inline std::atomic<unsigned> Touch{0},Width{0},Height{0},HostButtons{0};
// Native tab callbacks synthesize L/R after the touchscreen release. Actual
// L/R and ZL/ZR host buttons must still use ReMasked's controller page order.
inline bool NativePageInput(bool dual,unsigned hostButtons){return dual&&(hostButtons&0xc300u)==0;}
enum class CastLayout : unsigned { Off, SingleScreen, TopScreen };
inline std::atomic<CastLayout> AndroidCast{CastLayout::Off};
inline bool ExternalScanout(CastLayout cast, bool dual, bool lower) {
    switch (cast) {
    case CastLayout::SingleScreen: return !lower;
    case CastLayout::TopScreen: return !lower;
    default: return dual && lower;
    }
}
inline thread_local bool LowerScanout=false;
struct ScanoutScope {bool Before;ScanoutScope():Before(LowerScanout){LowerScanout=true;}~ScanoutScope(){LowerScanout=Before;}};
inline void SetTouch(int x,int y,bool down){Touch=down&&x>=0&&x<320&&y>=0&&y<240?0x80000000u|(unsigned(x)<<8)|unsigned(y):0;}
}
