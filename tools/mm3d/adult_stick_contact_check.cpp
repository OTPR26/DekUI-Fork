// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/adult_stick_contact.h"
#include <cassert>
#include <fstream>
#include <unordered_map>
#include <limits>
#include <iostream>
using namespace MM3D::AdultStickContact;
struct Memory {std::unordered_map<U32,U32> words;std::optional<U32> Read(U32 a){auto it=words.find(a);return it==words.end()?std::optional<U32>{}:it->second;}};
int main(int argc,char** argv) {
 assert(argc==2);Memory m;std::ifstream file(argv[1],std::ios::binary);assert(file);
 for(U32 a=0x4aa9b8;a<0x4aaa3c;a+=4){U32 w;file.seekg(a-0x100000);file.read(reinterpret_cast<char*>(&w),4);assert(file);m.words[a]=w;}
 assert(CodeMatches(m));m.words[PC]^=1;assert(!CodeMatches(m));m.words[PC]^=1;
 assert(Instruction(PC,Word)==(0xef000000|FirstSVC));assert(Instruction(PC,Word^1)==(Word^1));
 constexpr U32 actor=0x8000000,context=0x8100000;
 m.words[actor]=0x20000;m.words[actor+0x1f8]=7u<<24;m.words[context+0x13c]=0x402;m.words[context+0x20dc]=actor;
 std::array<U32,16> r{};std::array<U32,64> f{};r[11]=actor;r[8]=context;
 for(unsigned form=0;form<5;++form)for(bool enabled:{false,true})for(float y:{0.f,45.f,67.f,80.f,100.f,110.f,150.f}) {
  m.words[actor+0x1fc]=form<<24;const auto mem=m.words;const auto regs=r;f.fill(0x12345678);f[1]=std::bit_cast<U32>(y);f[3]=std::bit_cast<U32>(67.f);const auto original=f;
  Apply(m,r,f,enabled);assert(m.words==mem&&r==regs);
  for(unsigned i=0;i<f.size();++i)if(i!=1)assert(f[i]==original[i]);
  const float delta=std::bit_cast<float>(f[1]);
  if(!enabled||form!=4)assert(delta==y-67.f);
  else {assert(delta<=y-67.f);if(y<=67)assert(delta==y-67.f);if(y>=67&&y<=110)assert(std::abs(delta)<.01f);}
 }
 // Radius remains native: an adult hand at Y=100 is accepted within 20 units
 // horizontally, but not at 21. The original lower boundary is unchanged.
 auto distance=[](float x,float y,float z){float v=VerticalDistance(y,67,true);return x*x+v*v+z*z;};
 assert(distance(19,100,0)<400);assert(distance(21,100,0)>400);
 assert(distance(0,46,0)>400);assert(distance(0,140,0)>400);
 assert(VerticalDistance(100,68,true)==32);assert(std::isnan(VerticalDistance(std::numeric_limits<float>::quiet_NaN(),67,true)));
 m.words[actor+0x1fc]=4u<<24;m.words[actor+0x1f8]=9u<<24;f[1]=std::bit_cast<U32>(100.f);f[3]=std::bit_cast<U32>(67.f);assert(!Apply(m,r,f,true));assert(std::bit_cast<float>(f[1])==33.f);
 std::cout<<"Torch code fingerprint, native replay, Adult Link/form/item guards, contact boundaries and unrelated state preservation passed.\n";
}
