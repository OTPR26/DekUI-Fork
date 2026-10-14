// SPDX-License-Identifier: GPL-2.0-or-later
#define main page_switch_checks_main
#include "menu_page_switch_check.cpp"
#undef main
#include "mm3d/menu_memory.h"
constexpr U32 Gameplay=0x2b0000,Graphics=0x2c0000;
void GameplayRequest(Memory& m,U32 dest=I) {
    m.Write(0x656b04,Gameplay);m.Write(C+8,Gameplay);m.Write(C+12,dest);m.Write(C+16,0x101);
}
int main(int argc,char** argv) {
    using Recall=MM3D::MenuMemory::Hook;
    for(auto [page,vt]:std::array<std::pair<U32,U32>,4>{{{I,ItemsVtable},{K,MasksVtable},{Q,GearVtable},{P,MapVtable}}}) {
        auto m=Fixture();m.Write(0x656b04,Gameplay);m.Write(C+8,page);m.Write(G+0x34,0);
        Recall h;h.Tick(m,C);assert(h.last==vt);
        GameplayRequest(m);if(page==P)m.Byte(P+0x78e,1);
        h.Tick(m,C);assert(m.Read(C+12)==page&&h.Pending(m,C));
        if(page==P)assert(Byte(m,P+0x78e)==0);
        // Wait for a fresh native capture even if last pause left ready-looking state.
        m.Write(0x66889c,Graphics);m.Write(Graphics+0x184,0x80000);
        if(page==P||page==Q) {
            h.Ready(m,C,1);assert(Byte(m,C+16)==0&&Byte(m,C+17)==1);
            std::array<U32,16> r{};r[0]=page;r[1]=C;r[2]=page==Q?0x5a6d90:0x5998e0;
            assert(h.Prepare(m,r));assert(r[0]==G+0x11010&&r[1]==0&&r[15]==0x1bb320);
            assert(r[14]==0x10fd5c);
            // Emulate restart and conversion progressing asynchronously.
            for(U32 state:{1,2,4,7}) {
                m.Write(Graphics+0x184,state<<16);h.Ready(m,C,1);assert(Byte(m,C+16)==0);
            }
            m.Write(Graphics+0x184,8<<16);m.Write(G+0x11018,0);
            h.Ready(m,C,1);assert(Byte(m,C+16)==0);
            m.Write(G+0x11018,0x290000);
            h.Ready(m,C,0);assert(Byte(m,C+16)==0); // native resource loading still wins
            h.Ready(m,C,1);assert(Byte(m,C+16)==1);
        } else {
            std::array<U32,16> r{};r[0]=page;r[1]=C;r[2]=page==Q?0x5a6d90:0x59ebe8;
            assert(!h.Prepare(m,r));assert(r[0]==page&&r[1]==C&&r[14]==0x10fd5c);
        }
        if(page==Q) {
            // Actual manager ordering: completed capture, install Gear, Open,
            // clear pending. Gear must not turn state8 back into state1.
            m.Write(G+0x100,Graphics);m.Write(C+8,Q);
            h.GearOpenCapture(m,Graphics,1,Q,G);
            assert(m.Read(Graphics+0x184)==0x80000);
            m.Write(C+12,0);h.Tick(m,C);
            // Gear -> Masks must preserve capture through both Prepare and Close.
            m.Write(G+0x34,0x100);MM3D::MenuPageSwitch::Hook switcher;switcher.Tick(m,C);
            assert(switcher.target==K&&switcher.KeepCapture(m,G+0x11010));
            std::array<U32,16> r{};r[0]=G+0x11010;r[14]=0x123456;
            switcher.CaptureCall(m,r,3);assert(r[15]==0x123456);
            switcher.CaptureCall(m,r,5);assert(r[15]==0x5a6000);
            m.Write(G+0x34,0);
        }
        // Native Open completes; recording and the existing bumper path still work.
        m.Write(C+8,page);m.Write(C+12,0);m.Write(C+16,0x101);h.Tick(m,C);
        assert(h.last==vt&&!h.target);
        GameplayRequest(m,0);h.Tick(m,C);assert(h.last==vt);
        // Reopening again arms a new capture rather than preserving started=true.
        m.Write(C+12,I);h.Tick(m,C);assert(h.Pending(m,C)&&!h.capture_started);
    }
    // First open is native. Explicit map/notebook/Soaring and unrelated sources
    // are never redirected by the remembered inventory page.
    for(unsigned n=0;n<8;++n) {
        auto m=Fixture();Recall h;m.Write(C+8,Q);m.Write(G+0x34,0);h.Tick(m,C);
        GameplayRequest(m);
        switch(n) {
        case 0:h={};break;
        case 1:m.Write(C+12,P);break;
        case 2:m.Write(C+12,0x2d0000);break;
        case 3:m.Write(C+8,K);break;
        case 4:m.Byte(C+16,0);break;
        case 5:m.Byte(C+17,0);break;
        case 6:m.Write(Q,0);break;
        case 7:m.Byte(G+0x13d,1);break;
        }
        auto before=m.words;h.Tick(m,C);assert(m.words==before&&!h.target);
    }
    {
        auto m=Fixture();Recall h;m.Write(C+8,I);m.Write(G+0x34,0);h.Tick(m,C);
        m.Write(C+8,P);m.Byte(P+0x78e,1);h.Tick(m,C);assert(h.last==ItemsVtable);
        m.Write(C+8,K);m.Byte(K+0x9f,4);h.Tick(m,C);assert(h.last==ItemsVtable);
        GameplayRequest(m);h.Tick(m,C);m.Write(C+12,0);h.Tick(m,C);assert(!h.target);
        // New game ownership cannot inherit a remembered page or capture.
        m.Write(UI+0x16c,G+0x40000);m.Byte(G+0x40000+0x13d,4);h.Tick(m,C);assert(!h.last);
    }
    // Gear's ordinary capture request still runs without exact recall ownership.
    for(unsigned n=0;n<6;++n) {
        auto m=Fixture();Recall h;m.Write(C+8,Q);h.Tick(m,C);GameplayRequest(m);h.Tick(m,C);
        h.capture_started=true;m.Write(0x66889c,Graphics);m.Write(G+0x100,Graphics);
        m.Write(Graphics+0x184,0x800aa);m.Write(C+8,Q);
        switch(n) {
        case 0:h={};break;
        case 1:m.Write(C+12,0);break;
        case 2:m.Write(G+0x100,Graphics+0x1000);break;
        case 3:h.capture_started=false;break;
        case 4:m.Write(G+0x11018,0);break;
        case 5:m.Write(C+8,I);break;
        }
        h.GearOpenCapture(m,Graphics,1,Q,G);assert(m.Read(Graphics+0x184)==0x100aa);
    }
    // Without a recall operation, replay the native byte store exactly.
    {
        auto m=Fixture();Recall h;m.Write(C+16,0x12345678);h.Ready(m,C,0xab);
        assert(m.Read(C+16)==0x123456ab);
    }
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);assert(f);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),{});Memory m;
        for(size_t i=0;i+4<=bytes.size();i+=4) {
            U32 w=U32(bytes[i])|U32(bytes[i+1])<<8|U32(bytes[i+2])<<16|U32(bytes[i+3])<<24;
            m.Write(0x100000+U32(i),w);
        }
        assert(MM3D::MenuMemory::CodeMatches(m));
        for(auto pc:MM3D::MenuMemory::PCs) {
            auto word=*m.Read(pc);assert(MM3D::MenuMemory::Instruction(pc,word)!=word);
            m.Write(pc,0);assert(!MM3D::MenuMemory::CodeMatches(m));m.Write(pc,word);
        }
        m.Write(0x5998e0,0);assert(!MM3D::MenuMemory::CodeMatches(m));
    }
    std::cout<<"Menu recall checks passed\n";
}
