#include <cassert>
#include <fstream>
#include <unordered_map>
#include "mm3d/menu_sound.h"
using namespace MM3D::MenuSound;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 a){auto it=words.find(a);return it==words.end()?std::nullopt:std::optional<U32>(it->second);}
    void Write(U32 a,U32 v){words[a]=v;}
};
int main(int argc,char** argv){
    Memory m;constexpr U32 ctx=0x9000000,page=ctx+0x100,next=page+0x100;
    m.words[ctx+8]=page;m.words[ctx+12]=0;
    for(U32 v:{MM3D::MenuPageSwitch::ItemsVtable,MM3D::MenuPageSwitch::MasksVtable,
        MM3D::MenuPageSwitch::GearVtable,MM3D::MenuPageSwitch::MapVtable}) {
        m.words[page]=v;m.words[Request+4]=0x0100076c;m.words[Request]=0x12345601;
        const auto before=m.words;Store(m,ctx,Request,0);assert(m.words==before);
        m.words[ctx+12]=next;m.words[next]=MM3D::MenuPageSwitch::ItemsVtable;
        assert(AllowRepeat(m,ctx,Request));m.words[next]=0;assert(!AllowRepeat(m,ctx,Request));m.words[ctx+12]=0;
        for(U32 id:{0x0100076du,0x01000775u,0x01000791u,0u}){
            m.words[Request+4]=id;m.words[Request]=0x12345601;
            Store(m,ctx,Request,0);assert(m.words[Request]==0x12345600);
        }
    }
    m.words[Request+4]=0x0100076c;m.words[page]=0;
    assert(!AllowRepeat(m,ctx,Request));assert(!AllowRepeat(m,0,Request));
    assert(MM3D::MenuSound::Instruction(PC,Word)==0x9fffff98);assert(MM3D::MenuSound::Instruction(PC+4,Word)==Word);
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    U32 word;for(U32 a=0x100000;file.read(reinterpret_cast<char*>(&word),4);a+=4)m.words[a]=word;
    assert(MM3D::MenuSound::CodeMatches(m));m.words[PC]^=1;assert(!MM3D::MenuSound::CodeMatches(m));
}
