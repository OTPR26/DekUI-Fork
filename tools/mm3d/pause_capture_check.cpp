// Focused pause-capture guards and upload-orientation checks.
#include <cassert>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>
#include "mm3d/pause_capture.h"
#include "mm3d/atlas_render.h"
using namespace MM3D::PauseCapture;
struct Memory {
    std::unordered_map<U32,U32> words;
    std::optional<U32> Read(U32 at){auto i=words.find(at);return i==words.end()?std::nullopt:std::optional<U32>(i->second);}
};
struct Fake : Backend {
    unsigned captures=0,bindings=0,resets=0,eye=0;U32 address=0;Probes probes{};
    void CapturePause(U32 p,unsigned e)override{++captures;address=p;eye=e;}
    void BindPause(U32 p,unsigned e,const Probes& v)override{++bindings;address=p;eye=e;probes=v;}
    void ResetPause()override{++resets;}
};
int main(int argc,char** argv) {
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    Memory m;for(auto pc:PCs){U32 word;file.seekg(pc-0x100000);file.read(reinterpret_cast<char*>(&word),4);m.words[pc]=word;}
    for(U32 pc:{0x120658u,0x141d94u,0x19886cu}){U32 word;file.seekg(pc-0x100000);file.read(reinterpret_cast<char*>(&word),4);m.words[pc]=word;}
    assert(CodeMatches(m));
    for(unsigned i=0;i<3;++i){assert(Instruction(PCs[i],Words[i])==(0xef000000|FirstSVC+i));assert(Instruction(PCs[i],Words[i]^1)==(Words[i]^1));}
    m.words[PCs[0]]^=1;assert(!CodeMatches(m));
    assert(Physical(0x14109120)==0x20109120&&Physical(0x30109120)==0x20109120);
    assert(Physical(0x1f100000)==0x18100000&&Physical(0x08000000)==0);
    Fake fake;backend=&fake;Hook h;
    const U32 manager=0x700000,desc=0x710000,raw=0x720000;
    m.words[manager+0xc]=0x14109120;m.words[manager+0x10]=0x1414f630;
    m.words[manager+0x14]=0x14195b40;m.words[manager+0x18]=0x141db050;
    h.Convert(m,manager,0,0,0);assert(fake.captures==0); // Photo mode is untouched.
    h.Convert(m,manager,0,0,1);assert(fake.captures==1&&fake.address==0x20109120);
    h.Convert(m,manager,1,1,1);assert(fake.captures==2&&fake.address==0x201db050&&fake.eye==1);
    h.Convert(m,manager,2,0,1);assert(fake.captures==2);
    m.words[desc+8]=0x1907;m.words[desc+12]=0x8363;m.words[desc+0x10]=0x1000200;m.words[desc+0x14]=raw;
    m.words[manager+4]=raw;m.words[manager+8]=raw+0x40000;
    Probes expected{};
    for(unsigned i=0;i<expected.size();++i){expected[i]=0x1234+i;U32 at=raw+2*(ProbePoints[i][1]*512+ProbePoints[i][0]);m.words[at&~3u]=expected[i]<<((at&2)*8);}
    const auto before=m.words;h.Bind(m,desc,0x14b1e480);
    assert(fake.bindings==1&&fake.eye==0&&fake.address==0x20b1e480&&fake.probes==expected&&m.words==before);
    m.words[desc+12]=0x1401;h.Bind(m,desc,0x14b1e480);assert(fake.bindings==1);m.words=before;
    enabled=false;h.Convert(m,manager,0,0,1);h.Bind(m,desc,0x14b1e480);assert(fake.bindings==1&&fake.captures==2);enabled=true;
    h.Reset();assert(!h.manager&&fake.resets==1);h.Bind(m,desc,0x14b1e480);assert(fake.bindings==1);backend=nullptr;
    for(unsigned flip=0;flip<2;++flip) {
        std::vector<U32> pixels(512*256);
        for(unsigned i=0;i<expected.size();++i) {
            auto x=ProbePoints[i][0],y=flip?255-ProbePoints[i][1]:ProbePoints[i][1];
            // Independent reference interleave lookup, not the implementation loop.
            constexpr U32 interleave[8]={0,1,4,5,16,17,20,21};
            U32 offset=(y/8)*4096+(x/8)*64+interleave[x%8]+2*interleave[y%8];pixels[offset]=expected[i];
        }
        assert(Orientation([&](U32 off){return std::optional<U32>(pixels[off/2]);},expected)==flip+1);
        auto wrong=expected;wrong[2]^=1;
        assert(!Orientation([&](U32 off){return std::optional<U32>(pixels[off/2]);},wrong));
    }
    // Compile these Vulkan shader fixtures separately through glslang.
    for(unsigned cube=0;cube<2;++cube) {
        std::string source="#version 450\nlayout(set=0,binding=2,std140) uniform Data { vec4 blend_color; };\n";
        source+="layout(set=1,binding=0) uniform "+std::string(cube?"samplerCube":"sampler2D")+" tex0;\n";
        source+="layout(set=1,binding=1) uniform sampler2D tex1;\nlayout(set=1,binding=2) uniform sampler2D tex2;\n";
        source+="layout(location=0) in vec2 texcoord0;\nlayout(location=1) in vec2 texcoord1;\nlayout(location=2) in vec2 texcoord2;\nlayout(location=0) out vec4 color;\n";
        source+="layout(location=3) in vec4 primary_color;\n";
        source+="vec4 sampleTexUnit0() { return texture(tex0,"+std::string(cube?"vec3(texcoord0,1)":"texcoord0")+"); }\n";
        source+="vec4 sampleTexUnit1() { return texture(tex1,texcoord1); }\nvec4 sampleTexUnit2() { return textureLod(tex2, texcoord1,0); }\nvoid main() { color=sampleTexUnit0()+sampleTexUnit1()+sampleTexUnit2(); }\n";
        auto result=MM3D::Atlas::Shader(source);
        assert(result.find("raw.yx / vec2(240.0,400.0)")!=std::string::npos);
        std::ofstream("build-macos/pause-capture-"+std::to_string(cube)+".frag")<<result;
    }
    std::cout<<"Pause hooks, capture/upload ownership, fallback, stereo selection and orientation checks passed.\n";
}
