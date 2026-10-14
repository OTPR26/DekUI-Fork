// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mm3d/native_inventory.h"
namespace MM3D::MenuClose {
using NativeInventory::U32;
// USA Restoration: retain native close animations, sound and cleanup. Gear's
// option edge becomes zero before it can choose the Options destination.
constexpr std::array<U32,3> PCs{0x5a5798,0x5a57b4,0x599890};
constexpr std::array<U32,3> Words{0xe5942198,0xe3100002,0xe3100002};
constexpr std::array<U32,3> Replacements{0xe3a02000,0xe3100006,0xe3100006};
constexpr std::array<std::pair<U32,U32>,12> Guards{{
 {0x5a5790,0x0a000040},{0x5a5794,0xe594119c},{0x5a579c,0xe3a06000},{0x5a57a0,0xe1c21001},
 {0x5a57ac,0xe5846198},{0x5a57b0,0xe5900034},{0x5a57b8,0x03510000},{0x5a57bc,0x0a00004c},
 {0x599888,0xebffef37},{0x59988c,0xe5d60034},{0x599894,0x0a00000d},{0x599898,0xe5d9008e}
}};
inline U32 Instruction(U32 pc,U32 word){
 for(unsigned i=0;i<PCs.size();++i)if(pc==PCs[i]&&word==Words[i])return Replacements[i];
 return word;
}
template<class M> bool CodeMatches(M& m){
 for(unsigned i=0;i<PCs.size();++i)if(m.Read(PCs[i])!=Words[i])return false;
 for(auto [a,v]:Guards)if(m.Read(a)!=v)return false;
 return true;
}
}
