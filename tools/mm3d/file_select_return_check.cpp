// Focused Options -> File Select regression using native widget/queue fixtures.
#define main native_inventory_checks_main
#include "native_inventory_check.cpp"
#undef main
namespace F=MM3D::NativeFileSelect;
int main() {
    Memory m;F::Hook h;F::Options o;o.stone_spacing=9.3f;
    o.poses[F::Summary]={-32.5f,24.8f,.81f};o.poses[F::Actions]={231.2f,57.9f,.65f};F::Apply(o);
    auto panel=m.Layout("FileSelect"),all=m.Widget("all"),summary=m.Layout("UpPlateDetail");
    m.Children(panel+8,{all});std::vector<U32> branches;
    for(const char* name:{"cursor_g","black_screen","popup_window_g","message_g","delete_g","copy_g","start_g","plate_detail_g","null_plate_g","btn_return_l"})branches.push_back(m.Widget(name));
    m.Children(all,branches);std::vector<U32> cursors;
    for(const char* name:{"cursor_option_g","cursor_copy_del_g","cursor_start_g","cursor_plate_g"})cursors.push_back(m.Widget(name));
    m.Children(branches[0],cursors);
    std::vector<U32> cards;
    for(const char* name:{"plate_x_n","plate_y_n","plate_z_n"}){
        auto n=m.Widget(name,0,0),card=m.Layout("FilePlate");m.words[n+0x20]=card;cards.push_back(n);
    }
    m.Children(branches[8],cards);m.words[branches[8]+0xd4]&=~1u;
    const auto widgets=m.Alloc(20);m.words[panel+0x154]=widgets;m.words[panel+0x158]=5;
    for(unsigned i=0;i<3;++i)m.words[widgets+i*4]=cards[i];m.words[widgets+12]=branches[6];m.words[widgets+16]=branches[8];
    auto manager=m.Alloc(32),bottom=m.Alloc(0x848),top=m.Alloc(0x848),detail=m.Alloc(0x848);
    m.words[manager+28]=bottom;m.words[manager+20]=top;m.words[manager+16]=detail;
    m.Queue(bottom,{panel});m.Queue(detail,{summary});const auto native=m.words;
    h.Pass(m,manager,5);assert(h.active&&!h.stone_anchors&&h.presentation&&h.detail_progress==1);
    const auto routed=m.words;
    h.Pass(m,manager,3);h.Begin(m,panel);
    assert(std::abs(std::bit_cast<float>(m.words[branches[6]+0xec])-o.poses[F::Actions].x)<.0001f);
    assert(std::abs(std::bit_cast<float>(m.words[branches[6]+0xfc])-o.poses[F::Actions].y)<.0001f);
    for(auto card:cards){assert(m.words[card+0xec]==native.at(card+0xec));assert(m.words[card+0xfc]==native.at(card+0xfc));}
    h.End(m);assert(m.words==routed);
    h.Pass(m,manager,2);h.Begin(m,summary);
    assert(m.words[summary+8+0xec]==std::bit_cast<U32>(o.poses[F::Summary].x));
    assert(m.words[summary+8+0xe0]==std::bit_cast<U32>(o.poses[F::Summary].scale));
    h.End(m);assert(m.words==routed);
    // Starting/backing out animates FileSelect's own black_screen, distinct
    // from the global bottom fade. Capture it before top routing hides it.
    for(float cover:{0.f,.25f,.75f,1.f,.75f,.25f,0.f}) {
        m.words=routed;
        m.words[branches[1]+0xd4]|=1u;
        for(U32 offset:{0x128u,0x12cu,0x130u,0x134u})
            m.words[branches[1]+offset]=std::bit_cast<U32>(cover);
        const auto before=m.words;
        h.Pass(m,manager,3);h.Begin(m,panel);
        assert(std::abs(h.prompt_opacity-(1-cover))<.0001f);
        assert(!(m.words[branches[1]+0xd4]&1u));
        h.End(m);assert(m.words==before);
    }
    // Visible malformed cards are still rejected, rather than weakening validation.
    m.words=native;m.words[branches[8]+0xd4]|=1;const auto invalid=m.words;
    h.Pass(m,manager,5);assert(!h.active&&m.words==invalid);
    // Going back to the three cards restores normal spacing/cursor anchors.
    for(unsigned i=0;i<3;++i)m.words[cards[i]+0xfc]=std::bit_cast<U32>(46.f+52*i);
    h.Pass(m,manager,5);assert(h.active&&h.stone_anchors);
    for(unsigned i=0;i<3;++i)assert(h.stone_y[i]==46.f+52*i);
    F::Apply({});FileSelectPresentationChecks();std::cout<<"File select Options return checks passed\n";
}
