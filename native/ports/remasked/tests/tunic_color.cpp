// SPDX-License-Identifier: GPL-2.0-or-later
#include "../runtime/tunic_color.h"
#include <array>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv){
    if(argc!=2)return 2;setenv("REMASKED_ADULT_TUNIC_COLOR",argv[1],1);
    const std::array<uint8_t,12> original{20,100,25,173,150,90,70,255,90,90,90,40};
    auto pixels=original;ReMasked::TunicColor::Apply(0xF855116F497CFEC2ULL,pixels);
    const int color=ReMasked::TunicColor::Selected();
    if((color>=3)==(pixels==original))throw std::runtime_error("cloth policy not applied correctly");
    if(pixels[3]!=173||!std::equal(pixels.begin()+4,pixels.end(),original.begin()+4))throw std::runtime_error("skin/gray/alpha changed");
    auto unrelated=original;ReMasked::TunicColor::Apply(123,unrelated);
    if(unrelated!=original)throw std::runtime_error("unrelated texture changed");
    std::cout<<"PASS: tunic "<<color<<", cloth-only policy and unrelated materials/alpha preserved\n";
}
