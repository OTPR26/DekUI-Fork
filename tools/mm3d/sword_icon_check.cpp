#include "mm3d/sword_icon.h"
#include <cassert>
int main(){
 using namespace MM3D::SwordIcon;
 auto icon=std::make_shared<Image>();icon->width=icon->height=1;icon->pixels={12,34,56,78};
 Store(std::shared_ptr<const Image>(icon));
 for(bool top_down:{false,true}) {
  std::vector<std::uint8_t> pixels(512*512*4,99);Apply(0xE6E70FC2BF6BCD60ULL,pixels,512,512,top_down);
  unsigned changed=0;
  for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x){const bool expected=x>=126&&x<252&&(top_down?(y>=252&&y<294):(y>=218&&y<260));const auto at=(y*512+x)*4;
   if(expected){assert(pixels[at]==12&&pixels[at+1]==34&&pixels[at+2]==56&&pixels[at+3]==78);++changed;}else for(unsigned c=0;c<4;++c)assert(pixels[at+c]==99);
  }assert(changed==3*42*42);
 }
 Store(std::shared_ptr<const Image>{});std::vector<std::uint8_t> pixels(512*512*4,99);Apply(0xE6E70FC2BF6BCD60ULL,pixels,512,512);for(auto v:pixels)assert(v==99);
}
