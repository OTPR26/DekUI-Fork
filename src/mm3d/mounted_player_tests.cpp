// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/mounted_player.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace {
using namespace MM3D::MountedPlayer;
void Check(bool pass, const char* message) {
    if (!pass) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
Matrix Identity() { return {1,0,0,0,0,1,0,0,0,0,1,0}; }
struct Memory {
    std::unordered_map<U32, U32> words;
    std::optional<U32> Read(U32 at) {
        const auto it=words.find(at);
        return it==words.end()?std::optional<U32>{}:it->second;
    }
    void Write(U32 at, U32 value) { words[at]=value; }
    void PutMatrix(U32 at, const Matrix& matrix) {
        for (unsigned i=0;i<12;++i) words[at+4*i]=std::bit_cast<U32>(matrix[i]);
    }
};
}
int main() {
    using namespace MM3D::MountedPlayer;
    const float ratio=MM3D::PlayerModelScale::AdultScale;
    const auto root=Identity();auto pelvis=root;
    pelvis[3]=3*ratio;pelvis[7]=30*ratio;pelvis[11]=-4*ratio;
    auto view=Identity();view[0]=0;view[2]=1;view[8]=-1;view[10]=0;
    const auto shift=Offset(root,pelvis,view,ratio);
    Check(bool(shift), "finite rider pivot");
    Check(std::abs(pelvis[11]+(*shift)[0]+4)<.00001f &&
          std::abs(pelvis[7]+(*shift)[1]-30)<.00001f &&
          std::abs(-pelvis[3]+(*shift)[2]+3)<.00001f,
          "camera-space rider attachment returns to native saddle position");
    Check(Offset(root,pelvis,view,1)==std::optional<std::array<float,3>>({0,0,0}),
          "native model is unchanged");
    Check(!Offset(root,pelvis,view,0), "reject zero scale");
    auto malformed=pelvis;malformed[7]=std::numeric_limits<float>::quiet_NaN();
    Check(!Offset(root,malformed,view,ratio), "reject invalid pose matrices");
    Check(Weight(488,false,0,0,30)==1 && Weight(487,false,10,0,30)==1 &&
          Weight(541,false,10,0,0)==1 && Weight(542,false,10,0,69)==1,
          "scripted intro riding does not require the gameplay riding flag");
    Check(Weight(336,false,0,0,30)==0 && Weight(336,true,0,0,30)==1,
          "ordinary gameplay follows the native riding flag");
    Check(Weight(376,true,0,0,30)==0 && Weight(376,true,30,0,30)==1 &&
          Weight(375,true,0,0,30)==1 && Weight(375,true,30,0,30)==0,
          "mount and dismount transitions blend continuously");
    Check(Weight(411,false,0,0,30)==1 && Weight(411,false,30,0,30)==0,
          "falling from Epona releases the saddle correction");
    Memory m;constexpr U32 actor=0x8000000,context=0x8100000,controller=0x8200000;
    m.words[actor]=0x20000;m.words[actor+0x1fc]=4u<<24;
    m.words[context+0x13c]=0x402;m.words[context+0x20dc]=actor;
    m.words[actor+0x11db0]=0;m.words[actor+0x370]=488;
    m.words[actor+0x368]=controller;m.words[actor+0x5c]=std::bit_cast<U32>(.01f);
    m.words[actor+0x37c]=0;m.words[actor+0x384]=0;m.words[actor+0x388]=std::bit_cast<U32>(30.f);
    Hook hook;auto before=m.words;
    hook.BeginActor(m,actor,context,true);
    Check(hook.weight==1 && hook.player_instance==controller+0xd4,
          "target the player's active animated model instance");
    Check(before==m.words, "reading rider state never changes gameplay or collision");
    constexpr U32 instance=controller+0xd4, wrapper=0x8300000, skeleton=0x8400000,
                  source=0x8500000, output=0x8600000, camera=0x8700000;
    m.words[instance+0xc]=wrapper;m.words[wrapper]=skeleton;
    m.words[instance+0x1c]=source;m.words[instance+0x24]=output;
    m.words[skeleton]=0x206c6b73;m.words[skeleton+8]=26;
    for(unsigned i=0;i<26;++i) {
        m.words[skeleton+16+i*44]=MM3D::LinkPoseCapture::BoneWords[i];
        m.words[skeleton+56+i*44]=MM3D::LinkPoseCapture::Hashes[i];
        m.PutMatrix(output+i*48,Identity());
    }
    m.PutMatrix(source,root);m.PutMatrix(source+48,pelvis);m.PutMatrix(camera,view);
    before=m.words;
    hook.Enter(m,instance+4,camera);
    hook.MatrixBuilt(m,instance,0);
    Check(m.words==before,"unrelated model instances remain untouched");
    hook.Enter(m,instance,camera);
    Check(hook.instance==instance,"recognize the active human skeleton");
    hook.MatrixBuilt(m,instance,26);
    Check(m.words==before,"out-of-range bones remain untouched");
    for(unsigned bone=0;bone<26;++bone)hook.MatrixBuilt(m,instance,bone);
    for(const auto& [at,value]:before) {
        if(at>=output && at<output+26*48 && ((at-output)%48)==12)
            Check(m.words[at]==std::bit_cast<U32>((*shift)[0]),"apply camera-space X translation");
        else if(at>=output && at<output+26*48 && ((at-output)%48)==28)
            Check(m.words[at]==std::bit_cast<U32>((*shift)[1]),"apply camera-space Y translation");
        else if(at>=output && at<output+26*48 && ((at-output)%48)==44)
            Check(m.words[at]==std::bit_cast<U32>((*shift)[2]),"apply camera-space Z translation");
        else Check(m.words[at]==value,"preserve source pose, rotations, scales and gameplay state");
    }
    for(unsigned bone=0;bone<26;++bone)m.PutMatrix(output+bone*48,Identity());
    hook.Enter(m,instance,camera);hook.MatrixBuilt(m,instance,0);
    Check(m.words[output+28]==std::bit_cast<U32>((*shift)[1]),
          "freshly built frames never compound the offset");
    m.words[skeleton+56]^=1;
    before=m.words;hook.Enter(m,instance,camera);hook.MatrixBuilt(m,instance,0);
    Check(m.words==before,"mismatched skeletons remain untouched");
    hook.BeginActor(m,actor,context,false);Check(hook.weight==0, "native and PAK models excluded");
    m.words[actor+0x1fc]=3u<<24;
    hook.BeginActor(m,actor,context,true);Check(hook.weight==0, "transformation models excluded");
    std::puts("Mounted player pivot and transition checks passed.");
}
