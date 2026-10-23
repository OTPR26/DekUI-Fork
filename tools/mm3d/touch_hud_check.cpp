// clang++ -std=c++20 -Isrc tools/mm3d/touch_hud_check.cpp -o /tmp/remasked-touch-hud
#define main native_inventory_fixture_main
#include "native_inventory_check.cpp"
#undef main
#include "mm3d/player_hud.h"
int main(){
    namespace T=MM3D::TouchHud;namespace P=MM3D::PlayerHud;
    auto center=T::Project(.5f,.5f,.1f,1920,1080,0,0,1920,1080,true);
    assert(std::abs(center.x-200)<.001f&&std::abs(center.y-120)<.001f);
    auto edge=T::Project(0,0,.1f,1920,1080,0,0,1920,1080,true);
    assert(edge.x==0&&edge.y==0); // HUD projection remains 400x240.
    for(float x:{88.f,176.f,1494.f,1615.f,1740.f}){
        const auto a=T::Project(x/1920,.5f,.05f,1920,1080,60,0,1800,1080,true);
        assert(std::abs(60+a.x*1800/400-x)<.001f);
    }
    assert(!T::Project(0,0,1,0,0,0,0,0,0,true).Valid());
    Memory m;P::Hook hook;
    auto layout=m.Layout("HudItemIconSet");
    auto icon=m.Widget("icon_item_x_g",340,40),base=m.Widget("btn_x_g",340,40);
    for(auto n:{icon,base}){auto g=m.Alloc(0x80);m.words[n+0x28]=g;m.words[g+4]=6;
        m.words[g+0x20]=std::bit_cast<U32>(20.f);m.words[g+0x24]=std::bit_cast<U32>(20.f);
        m.words[n+0x134]=std::bit_cast<U32>(.4f);}
    m.Children(layout+8,{icon,base});const auto original=m.words;
    T::State state;state.enabled=true;state.anchors[2]={300,190,30};T::Publish(state);
    hook.Begin(m,layout);
    assert(std::abs(std::bit_cast<float>(m.words[icon+0xec])+6.3f-300)<.001f);
    assert(std::abs(std::bit_cast<float>(m.words[icon+0xfc])+6.3f-190)<.001f);
    assert(m.words[icon+0x134]==original.at(icon+0x134)); // Native fade survives.
    assert(std::bit_cast<float>(m.words[base+0xe0])==0); // No duplicate base.
    auto adjusted=m.words;hook.Begin(m,layout);hook.End(m);assert(m.words==adjusted);
    hook.End(m);assert(m.words==original); // Draw hook never leaves guest edits.
    T::Publish({});hook.Begin(m,layout);assert(m.words==original);hook.End(m);
    auto menu=m.Layout("MenuItem");state.enabled=true;T::Publish(state);
    auto before=m.words;hook.Begin(m,menu);hook.End(m);assert(m.words==before);
    // Item image and legacy button bases may be far apart. Ammo must move
    // with the image, preserving its offset and visibility, not the old base.
    {
        Memory image;P::Hook h;auto l=image.Layout("HudItemIconSet");
        auto root=image.Widget("icon_item_x_g"),art=image.Widget("icon_x_btn",340,40),
             ammo=image.Widget("ammo",344,58),base=image.Widget("btn_x_g",10,10);
        const auto quad=[&](U32 n,float w,float ht){auto g=image.Alloc(0x80);image.words[n+0x28]=g;image.words[g+4]=6;
            image.words[n+0x134]=std::bit_cast<U32>(1.f);image.words[g+0x20]=std::bit_cast<U32>(w);image.words[g+0x24]=std::bit_cast<U32>(ht);};
        quad(art,20,20);quad(base,80,40);
        // Invisible numeric panes retain stale origin matrices in the real HUD.
        // They must never stretch the fitted image across the screen.
        auto hidden=image.Widget("inactive_counter",0,20),hiddenDigit=image.Widget("stale_digit",0,20);
        quad(hiddenDigit,20,20);image.words[hidden+0xd4]=14;image.Children(hidden,{hiddenDigit});
        image.Children(art,{hidden});image.Children(root,{art,ammo});image.Children(l+8,{root,base});
        const auto original=image.words;T::State st;st.enabled=true;st.anchors[2]={300,190,30};T::Publish(st);h.Begin(image,l);
        P::Bounds b;assert(h.Measure(image,art,b,true));
        assert(std::abs((b.left+b.right)/2-300)<.001f&&std::abs((b.top+b.bottom)/2-190)<.001f);
        assert(std::abs((b.right-b.left)-12.6f)<.001f);
        assert(std::abs(std::bit_cast<float>(image.words[ammo+0xec])-(b.left+4*.63f))<.001f);
        h.End(image);assert(image.words==original);
    }
    // Each D-pad direction uses its own target; asymmetric source spacing
    // cannot shift the cross or push the left icon outside the viewport.
    {
        Memory image;P::Hook h;auto l=image.Layout("Hud2"),dpad=image.Widget("hud_dpad");
        std::vector<U32> roots;T::State st;st.enabled=true;
        const char* names[]{"icon_u_container","icon_d_container","icon_l_container","icon_r_container"};
        for(unsigned i=0;i<4;++i){auto n=image.Widget(names[i],float(20+i*17),float(50+i*13));roots.push_back(n);
            auto g=image.Alloc(0x80);image.words[n+0x28]=g;image.words[g+4]=6;
            image.words[n+0x134]=std::bit_cast<U32>(1.f);image.words[g+0x20]=std::bit_cast<U32>(12.f);image.words[g+0x24]=std::bit_cast<U32>(16.f);
            st.anchors[6+i]={float(20+i*12),float(100+i*10),20};}
        image.Children(dpad,roots);image.Children(l+8,{dpad});auto original=image.words;T::Publish(st);h.Begin(image,l);
        for(unsigned i=0;i<4;++i){P::Bounds b;assert(h.Measure(image,roots[i],b));
            assert(std::abs((b.left+b.right)/2-st.anchors[6+i].x)<.001f);
            assert(std::abs((b.top+b.bottom)/2-st.anchors[6+i].y)<.001f);assert(std::abs(b.bottom-b.top-10)<.001f);}
        h.End(image);assert(image.words==original);
    }
    T::Publish({});std::cout<<"Touch HUD projection, placement, fades, and restoration passed\n";
}
