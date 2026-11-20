#include "../../../titles/mm3d/dekui/camera_player_height.h"
#include <cassert>
#include <fstream>
#include <unordered_map>
using namespace Mm3dDekui::CameraPlayerHeight;
struct Memory {std::unordered_map<U32,U32> data;std::optional<U32> Read(U32 at){auto it=data.find(at);return it==data.end()?std::optional<U32>{}:it->second;}};
int main(int argc,char** argv) {
 assert(argc==2);Memory m;std::ifstream file(argv[1],std::ios::binary);assert(file);
 for(auto range:{std::pair{0x1f2c0cu,0x1f2c90u},std::pair{0x171d20u,0x171d50u}})
  for(U32 at=range.first;at<range.second;at+=4){U32 w;file.seekg(at-0x100000);file.read(reinterpret_cast<char*>(&w),4);assert(file);m.data[at]=w;}
 // Every camera-wrapper call site must qualify; direct actor-height calls must not.
 unsigned cameraCalls=0,directCalls=0;
 file.seekg(0,std::ios::end);const auto bytes=static_cast<U32>(file.tellg());
 for(U32 at=0x100000;at<0x100000+bytes;at+=4){U32 w;file.seekg(at-0x100000);file.read(reinterpret_cast<char*>(&w),4);if((w&0xff000000)!=0xeb000000)continue;
  const auto delta=static_cast<std::int32_t>((w&0xffffff)<<8)>>6;const U32 target=at+8+delta;
  if(target==0x171d20||target==0x1f2c0c){m.data[at]=w;U32 previous;file.seekg(at-0x100000-4);file.read(reinterpret_cast<char*>(&previous),4);m.data[at-4]=previous;
  const bool directCamera=at==0x3dedd4||at==0x447a60||at==0x44b9d8;
  assert(CameraCaller(m,at+4)==(target==0x171d20||directCamera));if(target==0x171d20)++cameraCalls;else ++directCalls;}
 }
 assert(cameraCalls==32&&directCalls==17);
 assert(CodeMatches(m));m.data[Pc]^=1;assert(!CodeMatches(m));m.data[Pc]^=1;
 assert(Instruction(Pc,Word)==(0x0f000000|Svc));assert(Instruction(Pc,Word^1)==(Word^1));
 constexpr U32 actor=0x08000000,context=0x08100000,root=0x08200000,call=0x448164;
 const U32 cameraBl=0xeb000000|(((0x171d20-call-8)>>2)&0xffffff);
 m.data[call]=cameraBl;m.data[0x72f5c0]=root;m.data[root+0x16c]=context;
 m.data[actor]=0x20000;m.data[context+0x13c]=0x402;m.data[context+0x20dc]=actor;
 m.data[actor+0x5c]=std::bit_cast<U32>(.01f);
 std::array<U32,16> r{};std::array<U32,64> f{};r[1]=actor+0x11000;r[14]=call+4;
 for(unsigned form=0;form<5;++form)for(bool enabled:{false,true})for(bool camera:{false,true}) {
  r[0]=form;m.data[actor+0x1fc]=form<<24;m.data[call]=camera?cameraBl:0xeb000000;
  const auto before=m.data;f[0]=std::bit_cast<U32>(32.f);
  for(unsigned repeat=0;repeat<30;++repeat) {
   Apply(m,r,f,enabled);
   const float expected=enabled&&camera&&form==4?44.f*Mm3dDekui::PlayerModelScale::AdultScale:44.f;
   assert(std::abs(std::bit_cast<float>(f[1])-expected)<1e-5f);
   assert(f[0]==std::bit_cast<U32>(32.f));assert(m.data==before);
  }
 }
 r[0]=4;m.data[actor+0x1fc]=4u<<24;m.data[call]=cameraBl;
 m.data[actor+0x5c]=std::bit_cast<U32>(.1f);Apply(m,r,f,true);assert(f[1]==std::bit_cast<U32>(44.f));
 m.data[actor+0x5c]=std::bit_cast<U32>(.01f);m.data[context+0x20dc]=actor+4;Apply(m,r,f,true);assert(f[1]==std::bit_cast<U32>(44.f));
}
