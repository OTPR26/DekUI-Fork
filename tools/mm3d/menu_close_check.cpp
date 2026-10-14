#include <cassert>
#include <fstream>
#include <unordered_map>
#include "mm3d/menu_close.h"
namespace C=MM3D::MenuClose;
using C::U32;
struct Memory {
 std::unordered_map<U32,U32> words;
 std::optional<U32> Read(U32 a){auto i=words.find(a);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
};
int main(int argc,char** argv){
 assert(argc==2);Memory m;std::ifstream f(argv[1],std::ios::binary);assert(f);
 U32 w;for(U32 a=0x100000;f.read(reinterpret_cast<char*>(&w),4);a+=4)m.words[a]=w;
 assert(C::CodeMatches(m));
 for(unsigned i=0;i<3;++i){
  assert(C::Instruction(C::PCs[i],C::Words[i])==C::Replacements[i]);
  assert(C::Instruction(C::PCs[i],0)==0);
  m.words[C::PCs[i]]^=1;assert(!C::CodeMatches(m));m.words[C::PCs[i]]^=1;
 }
 for(U32 buttons=0;buttons<4096;++buttons){
  // Decoded native TST immediate: exactly B or Select trigger closing.
  assert(bool(buttons&(C::Replacements[1]&255))==bool(buttons&6));
  assert(bool(buttons&(C::Replacements[2]&255))==bool(buttons&6));
 }
 assert(C::Replacements[0]==0xe3a02000); // MOV r2,0: option edge cannot be set.
}
