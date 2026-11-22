#include "mm3d/player_model_scale.h"
#include "mm3d/camera_player_height.h"
#include <cassert>
#include <fstream>
#include <unordered_map>
#include <limits>
using namespace MM3D::PlayerModelScale;
struct Memory {std::unordered_map<U32,U32> data;std::optional<U32> Read(U32 at){auto it=data.find(at);return it==data.end()?std::optional<U32>{}:it->second;}};
int main(int argc,char** argv) {
 Memory m;
 if(argc>1) {
  std::ifstream file(argv[1],std::ios::binary);assert(file);
  for(U32 at=0x182a2c;at<0x182ae4;at+=4){U32 word;file.seekg(at-0x100000);file.read(reinterpret_cast<char*>(&word),4);assert(file);m.data[at]=word;}
  assert(CodeMatches(m));m.data[0x182a50]^=1;assert(!CodeMatches(m));m.data[0x182a50]^=1;
  for(auto [first,end]:{std::pair{0x1f2c0cu,0x1f2c90u},std::pair{0x171d20u,0x171d50u},
                       std::pair{0x1f2e1cu,0x1f2ea0u},std::pair{0x171e7cu,0x171eacu}})
   for(U32 at=first;at<end;at+=4){U32 word;file.seekg(at-0x100000);file.read(reinterpret_cast<char*>(&word),4);assert(file);m.data[at]=word;}
  namespace Camera=MM3D::CameraPlayerHeight;
  assert(Camera::CodeMatches(m,Camera::Pc)||Camera::CodeMatches(m,Camera::RestorationPc));
 }
 constexpr U32 actor=0x08000000,context=0x08100000;const U32 normal=std::bit_cast<U32>(.01f);
 m.data[actor]=0x20000;m.data[actor+0x1fc]=4u<<24;m.data[context+0x13c]=0x402;m.data[context+0x20dc]=actor;
 for(U32 off:{0x58,0x5c,0x60})m.data[actor+off]=normal;
 std::array<U32,16> r{};std::array<U32,64> f{};r[4]=actor;r[6]=context;
 for(unsigned form=0;form<5;++form)for(bool enabled:{false,true})for(unsigned hook=0;hook<4;++hook) {
  m.data[actor+0x1fc]=form<<24;auto before=m.data;Apply(m,r,f,hook,enabled);
  U32 actual=hook==0?f[4]:r[hook==3?0:2];
  if(enabled&&form==4)assert(std::abs(std::bit_cast<float>(actual)-.01f*AdultScale)<1e-8f);
  else assert(actual==normal);
  assert(before==m.data); // No persistent scale/collision/velocity writes.
  for(unsigned i=0;i<25;++i)Apply(m,r,f,hook,enabled);
  assert((hook==0?f[4]:r[hook==3?0:2])==actual); // No compounding.
 }
 m.data[actor+0x1fc]=4u<<24;m.data[context+0x20dc]=actor+4;assert(!HumanPlayer(m,actor,context));
 assert(Scale(std::bit_cast<U32>(.1f),true)==std::bit_cast<U32>(.1f));
 assert(Scale(0x80000000,true)==0x80000000);
 assert(Scale(std::bit_cast<U32>(std::numeric_limits<float>::infinity()),true)==std::bit_cast<U32>(std::numeric_limits<float>::infinity()));
 for(unsigned i=0;i<4;++i){assert(Instruction(PCs[i],Words[i])==(0xef000000|FirstSVC+i));assert(Instruction(PCs[i],Words[i]^1)==(Words[i]^1));}
 namespace Camera=MM3D::CameraPlayerHeight;
 constexpr U32 root=0x08200000,camera_call=0x448164;
 const U32 camera_bl=0xeb000000|(((0x171d20u-camera_call-8)>>2)&0xffffff);
 m.data[0x72f5c0]=root;m.data[root+0x16c]=context;m.data[context+0x20dc]=actor;
 r[1]=actor+0x11000;r[14]=camera_call+4;
 f[0]=std::bit_cast<U32>(32.f);
 for(unsigned form=0;form<5;++form)for(bool enabled:{false,true})for(bool camera:{false,true}) {
  r[0]=form;m.data[actor+0x1fc]=form<<24;m.data[camera_call]=camera?camera_bl:0xeb000000;
  const auto before=m.data;
  const auto expected=enabled&&camera&&form==4?std::bit_cast<U32>(44.f*AdultScale):Camera::NativeHeight;
  for(unsigned frame=0;frame<4;++frame) {
   Camera::Apply(m,r,f,enabled);
   assert(f[1]==expected&&f[0]==std::bit_cast<U32>(32.f)&&m.data==before);
  }
 }
 for(auto pc:{Camera::Pc,Camera::RestorationPc})
  assert(Camera::Instruction(pc,Camera::Word)==(0x0f000000|Camera::Svc));
 constexpr U32 restoration_caller=0x447908;
 m.data[restoration_caller]=0xeb000000|(((0x1f2e1cu-restoration_caller-8)>>2)&0xffffff);
 m.data[restoration_caller-4]=0xe59000a4;
 assert(Camera::CameraCaller(m,restoration_caller+4));
 m.data[restoration_caller-4]=0;
 assert(!Camera::CameraCaller(m,restoration_caller+4));
 assert(Camera::Instruction(Camera::Pc,Camera::Word^1)==(Camera::Word^1));

}
