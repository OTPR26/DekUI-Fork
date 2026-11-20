// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d_touch_artwork.h"
#include <cassert>
#include <map>
using namespace Oot3dNativeGame;
struct Memory {
 std::map<uint32_t,uint32_t> words;
 std::optional<uint32_t> Read(uint32_t at){auto i=words.find(at);return i==words.end()?std::nullopt:std::optional(i->second);}
 void F(uint32_t at,float x){words[at]=std::bit_cast<uint32_t>(x);}
};
int main(){
 Memory m;constexpr uint32_t n=0x100000,g=0x101000,t=0x102000,d=0x103000,records=0x104000;
 m.words[n+0x28]=g;m.words[n+0xd4]=1;
 for(unsigned i=0;i<4;++i)m.F(n+0x128+i*4,1);
 m.F(n+0xe0,1);m.F(n+0xe4,0);m.F(n+0xf0,0);m.F(n+0xf4,1);m.F(n+0xec,30);m.F(n+0xfc,180);
 m.words[g+4]=6;m.words[g+0x10]=t;m.words[t+0x4c]=d;
 m.words[d+8]=0x6752;m.words[d+12]=0x1401;m.words[d+16]=128|(64<<16);m.words[d+32]=0x110000;
 m.F(g+0x14,0);m.F(g+0x18,0);m.F(g+0x20,12);m.F(g+0x24,12);
 m.F(g+0x28,.25f);m.F(g+0x2c,.5f);m.F(g+0x30,.125f);m.F(g+0x34,.25f);
 auto image=CaptureMm3dTouchArtwork(m,n);assert(image.size()==1);
 assert(image[0].texture.semantic_name=="mm3d/native/touch/128/64/26450/5121");
 assert(image[0].texture.guest_surface_address==0x110000);
 assert(image[0].destination.x>43.33f&&image[0].destination.x<43.34f&&image[0].destination.y==180);
 assert(image[0].corners&&(*image[0].corners)[3].u==.375f);
 m.F(n+0x110,.5f);m.F(n+0x114,0);m.F(n+0x118,.25f);m.F(n+0x11c,0);m.F(n+0x120,.5f);m.F(n+0x124,.125f);
 auto itemOne=CaptureMm3dTouchArtwork(m,n);m.F(n+0x118,.5f);auto itemTwo=CaptureMm3dTouchArtwork(m,n);
 assert(itemOne[0].texture.semantic_name==itemTwo[0].texture.semantic_name);
 assert((*itemOne[0].corners)[0].u==.375f&&(*itemTwo[0].corners)[0].u==.625f);
 assert((*itemOne[0].corners)[0].v==.375f);
 m.F(n+0x110,1);m.F(n+0x118,0);m.F(n+0x120,1);m.F(n+0x124,0);
 m.F(n+0xec,100);assert(image[0].destination.x<44); // own the final emitted values
 m.F(n+0x134,0);assert(CaptureMm3dTouchArtwork(m,n).empty());m.F(n+0x134,1);
 m.words[d+16]=127|(64<<16);assert(CaptureMm3dTouchArtwork(m,n).empty());
 m.words[d+8]=0x6756;m.words[d+12]=0x1401;m.words[d+16]=8|(8<<16);
 for(unsigned i=0;i<64;i+=4)m.words[0x110000+i]=0xffffffff;
 auto firstVerb=CaptureMm3dTouchArtwork(m,n);assert(firstVerb.size()==1);
 m.words[0x110000]=0;auto nextVerb=CaptureMm3dTouchArtwork(m,n);
 assert(nextVerb.size()==1&&firstVerb[0].texture.semantic_name!=nextVerb[0].texture.semantic_name);
 m.words[g+4]=3;m.words[g+0x2fc]=records;m.words[g+0x308]=2;
 for(unsigned k=0;k<2;++k){uint32_t a=records+k*60;m.words[a]=0x120000;m.words[a+4]=0x6756;m.words[a+8]=0x1401;m.words[a+12]=128;m.words[a+16]=128;
  m.F(a+20,float(k*8));m.F(a+24,0);m.words[a+28]=0;m.F(a+32,8);m.F(a+36,12);
  m.F(a+40,float(k)/16);m.F(a+44,0);m.F(a+48,1.f/16);m.F(a+52,12.f/128);m.words[a+56]=~0u;}
 auto text=CaptureMm3dTouchArtwork(m,n);assert(text.size()==2&&text[1].destination.x-text[0].destination.x==8);
 m.words[g+0x308]=129;assert(CaptureMm3dTouchArtwork(m,n).empty());
}
