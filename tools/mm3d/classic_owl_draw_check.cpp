// SPDX-License-Identifier: GPL-2.0-or-later
#include "mm3d/classic_owl_draw.h"
#include <cassert>
#include <iostream>
using namespace MM3D::ClassicSaving;
using U32=std::uint32_t;
struct Memory {
 std::vector<U32> words=std::vector<U32>(0x200000/4);
 std::optional<U32> Read(U32 p){if(p<0x100000||p>=0x300000||(p&3))return {};return words[(p-0x100000)/4];}
 void Write(U32 p,U32 v){assert(Read(p));words[(p-0x100000)/4]=v;}
 void Half(U32 p,U32 v){auto w=*Read(p&~3u);auto shift=(p&2)*8;Write(p&~3u,(w&~(65535u<<shift))|(v<<shift));}
};
int main(){
 for(auto labels:{std::u32string_view(U"YesNo"),std::u32string_view(U"OuiNon"),std::u32string_view(U"SíNo")}) {
 const unsigned yes_count=labels==U"SíNo"?2:3;
 Memory m;constexpr U32 window=0x180000,root=window+8,arrow=0x181000,text=0x182000,plate=0x183000;
 constexpr U32 children=0x184000,geo=0x185000,pgeo=0x186000,glyphs=0x187000;
 constexpr U32 font=0x188000,header=0x189000,pages=0x18a000,chars=0x18b000,sp=0x280000;
 for(U32 node:{root,arrow,text,plate}){m.Write(node+0x24,0xffff);m.Write(node+0xd4,15);m.Write(node+0x134,std::bit_cast<U32>(1.f));}
 m.Write(root+0x14,children);m.Write(root+0x18,3);
 m.Write(children,arrow);m.Write(children+4,text);m.Write(children+8,plate);
 m.Write(window+0x318,arrow);m.Write(text+0x28,geo);m.Write(plate+0x28,pgeo);
 m.Write(geo+4,3);m.Write(pgeo+4,6);m.Write(geo+0x28,font);
 m.Write(geo+0x2fc,glyphs);m.Write(geo+0x300,labels.size()+1);m.Write(geo+0x304,256);m.Write(geo+0x308,labels.size()+1);
 m.Write(font+4,header);m.Write(font+0xc,pages);m.Write(font+0x10,chars);
 m.Half(header+10,8);m.Half(header+12,12);m.Write(header+0x10,1);m.Write(header+0x14,11);
 m.Half(header+0x26,8);m.Half(header+0x28,8);m.Write(pages,0x100);m.Half(pages+4,128);m.Half(pages+6,64);
 const char32_t sorted[]=U"NOSYeinosuí";
 for(unsigned i=0;i<11;++i){U32 p=chars+12*i;m.Write(p,sorted[i]);m.Half(p+4,8);m.Half(p+10,i);}
 for(unsigned i=0;i<labels.size();++i){auto glyph=MM3D::NotebookFont::Lookup(m,font,labels[i],float(i*8));assert(glyph);
  for(unsigned j=0;j<15;++j)m.Write(glyphs+(i+1)*60+j*4,glyph->words[j]);}
 m.Write(glyphs+32,std::bit_cast<U32>(8.f)); // unrelated question glyph
 const std::array<float,4> green{.2f,1.f,.3f,.85f};
 for(unsigned c=0;c<4;++c)m.Write(geo+0x78+2*16+c*4,std::bit_cast<U32>(green[c]));
 for(unsigned i=1;i<=yes_count;++i)m.Write(glyphs+i*60+56,2);
 const auto original=m.words;enabled=true;exit_world_alpha=.5f;
 OwlDraw draw;const U32 out=draw.Root(m,window,root,sp,window,0);
 assert(out!=root);assert(m.Read(out+0x140)==root); // reverse traversal: black, then textbox
 const auto black=*m.Read(out+0x144), bg=*m.Read(black+0x28);
 assert((*m.Read(bg+4)&65535)==6);assert(m.Read(bg+0x10)==0);
 assert(m.Read(black+0x134)==std::bit_cast<U32>(.5f));
 assert(m.Read(glyphs+32)==std::bit_cast<U32>(8.f));
 for(unsigned i=0;i<labels.size();++i)assert(m.Read(glyphs+(i+1)*60+32)==(i>=yes_count?0u:std::bit_cast<U32>(8.f)));
 assert(m.Read(arrow+0x134)==0&&(*m.Read(arrow+0xd4)&1)==0);
 assert(draw.End(m)==sp);assert(m.words==original);
 draw.Root(m,window,root,sp,window,25);
 for(unsigned i=0;i<labels.size();++i)assert(m.Read(glyphs+(i+1)*60+32)==(i>=yes_count?0u:std::bit_cast<U32>(8.f)));
 assert(m.Read(glyphs+60+56)==14);
 const float brightness=std::bit_cast<float>(*m.Read(geo+0x78+14*16+4));
 assert(brightness>.4f&&brightness<1.f);
 for(unsigned c=0;c<4;++c) {
  const float actual=std::bit_cast<float>(*m.Read(geo+0x78+14*16+c*4));
  assert(std::abs(actual-green[c]*(c==3?1.f:brightness))<.00001f);
 }
 draw.End(m);assert(m.words==original);
 enabled=false;assert(draw.Root(m,window,root,sp,window,0)==root);draw.End(m);assert(m.words==original);
 }
 std::cout<<"Owl live-count glyph selection, brightness pulse, cursor hiding, sprite fade and restoration passed.\n";
}
