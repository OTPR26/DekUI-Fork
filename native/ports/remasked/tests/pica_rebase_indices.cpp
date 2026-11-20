// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../../tools/oot3d/native_game_runtime/pica_rebase_indices.h"
#include <cassert>
#include <array>
using Oot3dNativeGame::RebasePicaIndices;
int main(){
 const std::array<uint8_t,3> bytes={5,7,6};
 auto a=RebasePicaIndices(bytes,false,-5);assert(a&&*a==std::vector<uint8_t>({0,2,1}));
 assert(bytes[0]==5);assert(!RebasePicaIndices(bytes,false,-6));assert(!RebasePicaIndices(bytes,false,251));
 const std::array<uint16_t,3> wide={1000,1003,1001};
 auto b=RebasePicaIndices({reinterpret_cast<const uint8_t*>(wide.data()),sizeof(wide)},true,-1000);
 assert(b&&*b==std::vector<uint8_t>({0,0,3,0,1,0}));
 assert(!RebasePicaIndices(bytes,true,0));
 auto unchanged=RebasePicaIndices(bytes,false,0);assert(unchanged&&*unchanged==std::vector<uint8_t>(bytes.begin(),bytes.end()));
}
