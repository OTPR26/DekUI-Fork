// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
namespace MM3D::SwordIcon {
struct Image {unsigned width{},height{};std::vector<std::uint8_t> pixels;};
inline std::shared_ptr<const Image> image;
inline bool Texture(std::uint64_t hash){return hash==0xE6E70FC2BF6BCD60ULL||hash==0xF5B5C27302313096ULL;}
inline void Apply(std::uint64_t hash,std::span<std::uint8_t> pixels,unsigned width,unsigned height,bool top_down=false) {
 if(!Texture(hash)||width<512||height!=width||width%512||pixels.size()<std::size_t(width)*height*4)return;
 const auto source=std::atomic_load(&image);
 if(!source||!source->width||!source->height||source->pixels.size()!=std::size_t(source->width)*source->height*4)return;
 // The shared item atlas uses 42px cells. Replace only the three regular swords;
 // the Great Fairy's Sword, masks, items, portraits, and HUD layout stay intact.
 const unsigned scale=width/512,side=42*scale;
 for(unsigned column=3;column<=5;++column)for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x) {
  const unsigned row=252*scale+y,dy=top_down?row:height-1-row;
  const std::size_t dst=(std::size_t(dy)*width+column*42*scale+x)*4;
  const std::size_t src=(std::size_t(y*source->height/side)*source->width+x*source->width/side)*4;
  std::copy_n(source->pixels.data()+src,4,pixels.data()+dst);
 }
}
}
