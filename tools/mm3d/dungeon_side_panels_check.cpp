// SPDX-License-Identifier: GPL-2.0-or-later
#define main inventory_full_main
#include "native_inventory_check.cpp"
#undef main
int main() {
    namespace D=MM3D::DungeonMap;Memory m;D::Hook h;
    const auto layout=m.Layout("CommonBottomBg"),all=m.Widget("all");
    m.Children(layout+8,{all});std::vector<U32> source;
    for(unsigned i=0;i<2;++i) {
        auto n=m.Widget(i?"common_bottom_bg_pane1":"common_bottom_bg_pane0"),g=m.Alloc(0x7c);
        m.words[n+0x28]=g;m.words[g+4]=6;
        m.words[g+0x14]=std::bit_cast<U32>(160.f*i);
        m.words[g+0x20]=std::bit_cast<U32>(160.f);m.words[g+0x24]=std::bit_cast<U32>(240.f);
        m.words[g+0x28]=std::bit_cast<U32>(i?.953125f:.640625f);
        m.words[g+0x30]=std::bit_cast<U32>(i?-.3125f:.3125f);
        m.words[n+0x134]=std::bit_cast<U32>(.6f);
        for(unsigned c=0;c<3;++c)m.words[n+0x128+c*4]=std::bit_cast<U32>(1.f);
        source.push_back(n);
    }
    m.Children(all,source);h.valid=h.dungeon=true;h.pass=1;h.backgrounds[0]=layout;h.routed={layout};
    const auto sp=(m.Alloc(0x9000)+0x9000)&~7u;
    D::Options o;o.top_strip=false;o.panels={{{12,110,.5f},{275,128,.3f}}};D::Apply(o);
    for(float darkness:{0.f,.7f,1.f}) {
        D::panel_darkness=darkness;auto before=m.words;
        auto r=h.PanelRoot(m,layout,sp);assert(r==sp-0x3000&&m.words[r+0x18]==14);
        auto f=[&](U32 p){return std::bit_cast<float>(m.words[p]);};
        for(unsigned i=0;i<8;++i) {
            auto n=m.words[r+0x140+i*4],g=m.words[n+0x28];auto p=o.panels[i/4];
            assert(f(n+0xec)==p.x&&std::abs(f(n+0xfc)-(p.y-(i%4>=2?240*p.scale:0)))<.0001f);
            assert(f(n+0xe0)==p.scale&&f(n+0xf4)==p.scale);
            assert(f(g+0x20)==160&&f(g+0x24)==240); // full texture, both native halves
            assert(f(g+0x30)==(i%2?-.3125f:.3125f));
            assert(f(n+0x128)==1-darkness&&f(n+0x134)==.6f);
        }
        for(unsigned i=8;i<14;++i) {
            auto n=m.words[r+0x140+i*4];assert(f(n+0x128)==0);
            assert(std::abs(f(n+0x134)-.6f*(.32f-.09f*((i-8)%3)))<.0001f);
        }
        assert(h.panel_scratch.End(m)==sp&&m.words==before);
    }
    for(unsigned mode=0;mode<5;++mode) {
        h.dungeon=mode!=0;h.pass=mode==1?3:1;h.routed=mode==2?std::vector<U32>{}:std::vector<U32>{layout};
        D::side_panels=mode!=3;D::panel_darkness=mode==4?NAN:0;
        auto before=m.words;assert(h.PanelRoot(m,layout,sp)==layout+8);
        assert(!h.panel_scratch.End(m)&&m.words==before);
    }
    h.valid=h.dungeon=true;h.pass=1;h.routed={layout};
    for(bool sides:{false,true})for(float scale:{.2f,.3125f,.7f}) {
        o.side_panels=sides;o.top_strip=true;o.top_panel={-15,9,scale};o.panel_darkness=.7f;D::Apply(o);
        const auto before=m.words;auto root=h.PanelRoot(m,layout,sp);
        const unsigned first=sides?14:0;
        assert(root==sp-0x3000&&m.words[root+0x18]==first+8);
        auto f=[&](U32 p){return std::bit_cast<float>(m.words[p]);};
        float previous_right=-15;
        for(unsigned i=0;i<8;++i) {
            auto n=m.words[root+0x140+(first+i)*4],g=m.words[n+0x28];
            const float left=f(n+0xec)+f(g+0x14)*f(n+0xe0);
            assert(std::abs(left-previous_right)<.0001f); // no gaps at any seam
            previous_right=left+f(g+0x20)*f(n+0xe0);
            assert(f(n+0xfc)==9&&f(n+0xe0)==scale&&f(n+0xf4)==scale);
            assert(f(g+0x24)==240&&f(g+0x30)==(i%2?-.3125f:.3125f));
            assert(std::abs(f(n+0x128)-.3f)<.0001f&&f(n+0x134)==.6f);
        }
        assert(std::abs(previous_right-(-15+1280*scale))<.0001f);
        assert(h.panel_scratch.End(m)==sp&&m.words==before);
        h.dungeon=false;assert(h.PanelRoot(m,layout,sp)==layout+8);
        assert(!h.panel_scratch.End(m)&&m.words==before);h.dungeon=true;
    }
    D::Apply(D::Options{});
    DungeonMapShadowChecks();NotebookPromptChecks();
}
