// Native title-model guard, independent transforms, and fresh-frame regression.
// clang++ -std=c++20 -Isrc tools/mm3d/native_startup_check.cpp -o build-macos/title-check
#include "mm3d/native_startup.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
namespace T=MM3D::NativeStartup;
using U32=T::U32;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 p){auto it=words.find(p);return it==words.end()?std::nullopt:std::optional<U32>(it->second);}
    void Write(U32 p,U32 v){assert(words.contains(p));words[p]=v;}
    void Load(const char* path,U32 at) {
        std::ifstream f(path,std::ios::binary);assert(f);
        U32 v;while(f.read(reinterpret_cast<char*>(&v),4)){words[at]=v;at+=4;}
    }
    float F(U32 p){return std::bit_cast<float>(words.at(p));}
};
int main(int argc,char** argv) {
    assert(argc==3);Memory code;code.Load(argv[1],0x100000);
    assert(T::CodeMatches(code));code.words[T::MatrixPC]^=1;assert(!T::CodeMatches(code));
    Memory m;m.Load(argv[2],0x2000000);
    constexpr U32 skeleton=0x1000000,wrapper=0x1001000,out=0x1002000;
    m.words[skeleton+0xc]=wrapper;m.words[skeleton+0x24]=out;
    m.words[wrapper]=0x2000000+0x4c; // Actual decoded CMB skeleton, not a copied signature.
    auto fresh=[&]{for(unsigned i=0;i<13;++i)for(unsigned j=0;j<12;++j)
        m.words[out+48*i+4*j]=std::bit_cast<U32>(j==3?float(i):j==7?-float(i):j%5==0?1.f:0.f);};
    fresh();const auto initial=m.words;T::Hook hook;T::Options options;T::Apply(options);
    hook.Enter(m,skeleton);assert(hook.skeleton==skeleton);
    for(unsigned i=0;i<13;++i)hook.Matrix(m,skeleton,i);
    assert(m.words==initial);
    options.logo={20,30,2};options.copyright={-40,-50,.5f};T::Apply(options);
    auto frame=[&]{fresh();hook.Enter(m,skeleton);for(unsigned i=0;i<13;++i)hook.Matrix(m,skeleton,i);};
    frame();auto changed=m.words;
    assert(m.F(out+48+12)==-3.f&&m.F(out+48+28)==4.f);
    for(unsigned i=2;i<13;++i) {
        assert(m.F(out+48*i)==2.f);
        assert(m.F(out+48*i+12)==2.f*i+2.f);
        assert(m.F(out+48*i+28)==-2.f*i-3.f);
        for(unsigned j=8;j<12;++j)assert(m.words.at(out+48*i+4*j)==initial.at(out+48*i+4*j));
    }
    frame();assert(m.words==changed); // No accumulation between frames.
    options.logo={0,0,1};options.copyright={0,0,1};T::Apply(options);frame();assert(m.words==initial);
    m.words[0x2000000+0x5c+44+40]^=1;auto bad=m.words;
    hook.Enter(m,skeleton);assert(!hook.skeleton);hook.Matrix(m,skeleton,1);assert(m.words==bad);
    std::cout<<"Native title checks passed (actual CMB + patched executable).\n";
}
