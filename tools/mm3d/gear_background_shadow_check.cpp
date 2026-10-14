// SPDX-License-Identifier: GPL-2.0-or-later
#define main inventory_legacy_main
#include "native_inventory_check.cpp"
#undef main
int main(){
    Memory m;Hook h;h.gear=true;h.panel=m.Layout("MenuCollect");h.background=m.Layout("CommonBottomBg");
    auto right=m.Brown(h.background),geo=m.Alloc(0x7c);m.words[right+0x28]=geo;m.words[geo+4]=6;
    m.words[right+0x134]=std::bit_cast<U32>(.8f);
    auto groups=m.Alloc(8),entries=m.Alloc(64),record=m.Alloc(0x60);
    m.words[0x13ace8]=0x7cdc9c;m.words[0x7cdc9c+0x28]=groups;m.words[0x7cdc9c+0x2c]=entries;
    m.words[groups]=16;m.words[entries+14*4]=record;m.words[record+4]=14u<<16;
    auto manager=m.Alloc(0x30),from=m.Alloc(0x848),fg=m.Alloc(0x848),top=m.Alloc(0x848);
    m.words[manager+24]=from;m.words[manager+28]=fg;m.words[manager+12]=top;
    m.Queue(from,{h.background});m.Queue(fg,{h.panel});
    background_pose.x=17;background_pose.y=23;background_pose.scale=.75f;
    shadow_width=1.5f;shadow_opacity=.4f;
    h.BackgroundPass(m,manager);assert(h.routed_background==h.background&&h.brown_shadow_steps==2);
    assert(m.words[top+0x844]==4);h.pass=1;
    auto before=m.words;auto f=[&](U32 p){return std::bit_cast<float>(m.words[p]);};
    for(unsigned draw=0;draw<4;++draw) {
        h.Begin(m,h.background);
        if(draw<2) {
            assert(m.words[geo+0x10]==record+0xc);
            assert(f(geo+0x28)==.0625f&&f(geo+0x2c)==.96875f&&f(geo+0x30)==-.0625f&&f(geo+0x34)==0);
            assert(f(right+0xec)+256*f(right+0xe0)==17+320*.75f);
            assert(f(right+0xec)+240*f(right+0xe0)==17+320*.75f+16*.75f*1.5f);
            assert(std::abs(f(right+0x134)-.32f)<.0001f&&f(right+0x128)==.4f);
        }
        assert(f(right+0xfc)==23-(draw%2?180:0));
        h.End(m);assert(m.words==before);
    }
    // Native gradient requires only two spare queue slots, not24 band copies.
    h.ConfigureBrownShadow(m,h.background,2,1,.5f);assert(h.brown_shadow_steps==2);
    h.ConfigureBrownShadow(m,h.background,1,1,.5f);assert(h.brown_shadow_steps==0);
    m.words[record+4]^=1;h.ConfigureBrownShadow(m,h.background,24,1,1);assert(h.brown_shadow_steps==24);
    h.gear=false;m.words[record+4]^=1;h.ConfigureBrownShadow(m,h.background,24,1,1);assert(h.brown_shadow_steps==24);
}
