// SPDX-License-Identifier: GPL-2.0-or-later
#define main page_switch_checks_main
#include "menu_page_switch_check.cpp"
#undef main
#include "mm3d/menu_backdrop.h"
using Backdrop=MM3D::MenuBackdrop::Hook;
constexpr U32 SC=0x72f5bc; // Inline ScreenContext, first word is its vtable.
constexpr U32 BG=0x300000,ALL=0x301000,LEFT=0x302000,RIGHT=0x303000,
 LP=0x304000,RP=0x305000,LG=0x306000,RG=0x307000,GRAPHICS=0x308000,
 MANAGER=0x309000,QUEUE=0x30a000,PLAY=0x30b000,GAME_GRAPHICS=0x30c000;
void Widget(Memory& m,U32 p,const char* name) {
    m.Write(p+0x10,p+0x300);
    for(unsigned i=0;;++i){m.Byte(p+0x300+i,name[i]);if(!name[i])break;}
}
void Children(Memory& m,U32 p,std::initializer_list<U32> children) {
    m.Write(p+0x14,p+0x400);m.Write(p+0x18,children.size());unsigned i=0;
    for(auto c:children)m.Write(p+0x400+4*i++,c);
}
Memory BackgroundFixture(U32 page=Q) {
    auto m=Fixture();
    for(U32 offset:{0u,4u,8u,12u,16u})m.words.erase(C+offset);
    m.Write(SC,0x630000);m.Write(SC+4,UI);m.Write(SC+8,page);
    m.Write(SC+12,0);m.Write(SC+16,0x101);m.Write(G+0x34,0);
    m.Write(0x656b04,PLAY);m.Write(G+0xaac,0x12340001);
    m.Write(0x66889c,GRAPHICS);m.Write(G+0x100,GAME_GRAPHICS);m.Write(GRAPHICS+0x184,0x80000);
    // Live Gear: global capture manager is ready=8, game graphics is a separate
    // object with state=1. Neither equal pointers nor equal states are required.
    m.Write(GAME_GRAPHICS+0x184,0x10000);
    m.Write(0x6b7ba0,BG);m.Layout(BG,"UpCommonBG");
    Widget(m,ALL,"all");Widget(m,LEFT,"picture_left_g");Widget(m,RIGHT,"picture_right_g");
    Widget(m,LP,"picture_left_pane");Widget(m,RP,"picture_right_pane");
    Children(m,BG+8,{ALL});Children(m,ALL,{LEFT,RIGHT});Children(m,LEFT,{LP});Children(m,RIGHT,{RP});
    m.Write(LP+0x28,LG);m.Write(RP+0x28,RG);m.Write(LG+4,4);m.Write(RG+4,4);
    m.Write(LG+0x10,0);m.Write(RG+0x10,0);
    m.Write(MANAGER+8,QUEUE);for(unsigned i=0;i<16;++i)m.Write(QUEUE+0x804+4*i,0);
    m.Write(QUEUE+0x844,0);return m;
}
int main(int argc,char** argv) {
    for(U32 page:{Q,P}) {
        auto m=BackgroundFixture(page);Backdrop h;assert(h.Active(m));
        h.Pause(m,G,page,0xaac,1);assert(m.Read(G+0xaac)==0x12340003);
        assert(m.Read(LG+0x10)==0x290000&&m.Read(RG+0x10)==0x290058);
        h.Pass(m,MANAGER,1);assert(m.Read(QUEUE+0x844)==0);
        h.Pass(m,MANAGER,0);assert(m.Read(QUEUE+4)==BG&&m.Read(QUEUE+0x804)==1&&m.Read(QUEUE+0x844)==1);
        h.Pass(m,MANAGER,0);assert(m.Read(QUEUE+0x844)==1);
        h.Release(m);assert(m.Read(G+0xaac)==0x12340001);
        // Native full pause belongs to the game and is not undone by us.
        h.Pause(m,G,page,0xaac,3);h.Release(m);assert(m.Read(G+0xaac)==0x12340003);
    }
    {
        // A plausible context behind word0 must not substitute for the real
        // inline object. Word0 is the ScreenContext's virtual-method table.
        auto m=BackgroundFixture();Backdrop h;
        for(U32 offset:{4u,8u,12u,16u})m.Write(0x630000+offset,*m.Read(SC+offset));
        m.Write(SC+4,0);auto before=m.words;
        assert(!h.Active(m));h.Pass(m,MANAGER,0);assert(m.words==before);
    }
    for(unsigned n=0;n<10;++n) {
        auto m=BackgroundFixture();Backdrop h;
        switch(n) {
        case 0:m.Write(GRAPHICS+0x184,7<<16);break;
        case 1:m.Write(G+0x11018,0);break;
        case 2:m.Layout(BG,"Other");break;
        case 3:m.Write(ALL+0x18,3);break;
        case 4:m.Write(LEFT+0x18,2);break;
        case 5:m.Write(RP+0x28,LG);break;
        case 6:m.Write(LG+4,3);break;
        case 7:m.Write(SC+8,P);m.Byte(P+0x78e,1);break;
        case 8:m.Write(SC+8,PLAY);break;
        case 9:m.Write(0x66889c,0);break;
        }
        assert(!h.Active(m));auto before=m.words;h.Pass(m,MANAGER,0);assert(m.words==before);
        h.Pause(m,G,*m.Read(SC+8),0xaac,1);assert(m.Read(G+0xaac)==0x12340001);
    }
    {
        auto m=BackgroundFixture();Backdrop h;h.Pause(m,G,Q,0xaac,1);
        m.Write(G+0xaac,0x43210000);h.Release(m);assert(m.Read(G+0xaac)==0x43210000);
        h.Pause(m,G,Q,0xaac,1);MM3D::GearMenu::enabled=false;h.Tick(m);
        assert(m.Read(G+0xaac)==0x43210001);MM3D::GearMenu::enabled=true;
        h.Pause(m,G,Q,0xaac,1);m.Write(UI+0x16c,G+0x40000);
        auto before=m.words;h.Release(m);assert(m.words==before);
    }
    for(U32 page:{Q,P}) {
        auto m=BackgroundFixture(page);Backdrop h;m.Write(SC+8,PLAY);m.Write(SC+12,page);
        std::array<U32,16> r{};r[0]=page;r[1]=SC;r[2]=page==Q?0x5a6d90:0x5998e0;
        assert(h.Prepare(m,r));assert(r[15]==0x1bb320&&r[0]==G+0x11010&&r[14]==0x10fd5c);
        m.Write(GRAPHICS+0x184,1<<16);assert(h.PreparedReady(m,SC,1)==0);
        m.Write(GRAPHICS+0x184,8<<16);assert(h.PreparedReady(m,SC,1)==1&&h.PreparedReady(m,SC,0)==0);
        m.Write(SC+8,page);assert(!h.KeepGearCapture(m,GAME_GRAPHICS,page,G));
        // Replay the native store to the independent per-frame graphics. It
        // enables model depth clearing without restarting the global capture.
        m.Write(GAME_GRAPHICS+0x184,0x55aa);
        MM3D::MenuMemory::Hook recall;
        recall.GearOpenCapture(m,GAME_GRAPHICS,1,page,G);
        assert(m.Read(GAME_GRAPHICS+0x184)==0x155aa);
        assert(m.Read(GRAPHICS+0x184)==0x80000&&h.Active(m));
        m.Write(G+0x100,GRAPHICS);
        assert(h.KeepGearCapture(m,GRAPHICS,page,G)==(page==Q));
        m.Write(G+0x100,GAME_GRAPHICS);
        m.Write(SC+12,0);h.Tick(m);assert(!h.prepare_context);
        // Next reopening must capture again, even with ready-looking old state.
        m.Write(SC+8,PLAY);m.Write(SC+12,page);r[0]=page;r[1]=SC;r[2]=page==Q?0x5a6d90:0x5998e0;
        assert(h.Prepare(m,r));
    }
    {
        auto m=BackgroundFixture(P);Backdrop h;m.Write(SC+8,PLAY);m.Write(SC+12,P);m.Byte(P+0x78e,1);
        std::array<U32,16> r{};r[0]=P;r[1]=SC;r[2]=0x5998e0;assert(!h.Prepare(m,r));
    }
    for(bool keep:{false,true}) {
        auto m=BackgroundFixture(P);Backdrop h;std::array<U32,16> r{};
        r[0]=P;r[1]=SC;r[14]=0x123456;r[15]=0x5970b4;
        h.CloseMap(m,r,keep);assert(r[4]==P);
        if(keep)assert(r[0]==P&&r[1]==SC&&r[14]==0x123456&&r[15]==0x5970b4);
        else assert(r[7]==SC&&r[0]==G+0x11010&&r[14]==0x5970b8&&r[15]==0x198864);
    }
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);assert(f);Memory m;
        std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)),{});
        for(size_t i=0;i+4<=b.size();i+=4)m.Write(0x100000+i,U32(b[i])|U32(b[i+1])<<8|U32(b[i+2])<<16|U32(b[i+3])<<24);
        assert(MM3D::MenuBackdrop::CodeMatches(m));
        for(auto pc:MM3D::MenuBackdrop::PCs) {
            auto w=*m.Read(pc);assert(MM3D::MenuBackdrop::Instruction(pc,w)!=w);
            m.Write(pc,0);assert(!MM3D::MenuBackdrop::CodeMatches(m));m.Write(pc,w);
        }
        m.Write(0x1b03bc,0x72f5c0);assert(!MM3D::MenuBackdrop::CodeMatches(m));
    }
    std::cout<<"Shared menu backdrop checks passed\n";
}
