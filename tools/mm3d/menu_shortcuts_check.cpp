#include <cassert>
#include <fstream>
#include <unordered_map>
#include "mm3d/menu_shortcuts.h"
namespace S=MM3D::MenuShortcuts;
using S::U32;
struct Memory {
 std::unordered_map<U32,U32> words;
 std::optional<U32> Read(U32 a){auto i=words.find(a);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
};
int main(int argc,char** argv){
 assert(argc==2);Memory m;std::ifstream f(argv[1],std::ios::binary);assert(f);
 U32 w;for(U32 a=0x100000;f.read(reinterpret_cast<char*>(&w),4);a+=4)m.words[a]=w;
 assert(S::CodeMatches(m));unsigned changed=0;
 for(auto [a,v]:m.words)if(S::Instruction(a,v)!=v){++changed;assert(a==S::PC&&S::Instruction(a,v)==S::Return);}
 assert(changed==1);
 for(unsigned i=0;i<S::Code.size();++i){m.words[S::Start+i*4]^=1;assert(!S::CodeMatches(m));m.words[S::Start+i*4]^=1;}
 // The early return restores exactly the registers saved at entry, replacing LR with PC.
 assert((S::Code[0]&65535)==0x41f0);assert((S::Return&65535)==0x81f0);
 // Non-ZR branches jump over the new return into the existing Items/notebook paths.
 auto target=[](U32 a,U32 word){return U32(a+8+(int(word<<8)>>6));};
 assert(target(0x7d06b8,m.words[0x7d06b8])==0x7d070c);
 assert(target(0x7d06c0,m.words[0x7d06c0])==0x7d0718);
 // Notebook's continuation still reaches the original test that exits for non-ZR.
 assert(target(0x7d0730,m.words[0x7d0730])==0x7d06d4);
}
